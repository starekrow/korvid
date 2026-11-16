// Korvid renderer-d3d12: the factory, the adapter, the device and its one queue, the fence
// every frame signals, and the debug layer, whose messages are drained into the log once a
// frame (the info queue, which every runtime has; the callback form is Windows 11's only).
#include "renderer_internal.h"
#include "korvid/core/format.h"

namespace kv { namespace KV_RB_NS {

const char* hr_name(HRESULT hr) {
    switch (hr) {
    case S_OK:                                 return "S_OK";
    case E_FAIL:                               return "E_FAIL";
    case E_INVALIDARG:                         return "E_INVALIDARG";
    case E_OUTOFMEMORY:                        return "E_OUTOFMEMORY";
    case E_NOTIMPL:                            return "E_NOTIMPL";
    case E_NOINTERFACE:                        return "E_NOINTERFACE";
    case DXGI_ERROR_DEVICE_REMOVED:            return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG:               return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET:              return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:     return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:              return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_UNSUPPORTED:               return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_FOUND:                 return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_WAS_STILL_DRAWING:         return "DXGI_ERROR_WAS_STILL_DRAWING";
    case D3D12_ERROR_ADAPTER_NOT_FOUND:        return "D3D12_ERROR_ADAPTER_NOT_FOUND";
    case D3D12_ERROR_DRIVER_VERSION_MISMATCH:  return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
    default: break;
    }
    static thread_local char buf[16];
    format(buf, sizeof buf, "0x%08x", (u32)hr);
    return buf;
}

namespace {

// The first hardware adapter that makes a device at feature level 11_0, the high-performance
// one first where DXGI 1.6 can say which that is.
bool pick_adapter(Renderer* r) {
    IDXGIFactory6* f6 = nullptr;
    r->factory->QueryInterface(IID_PPV_ARGS(&f6));
    for (u32 i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))
                        : r->factory->EnumAdapters1(i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a->Release(); continue; } // WARP: not for games
        if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            KV_INFO("d3d12", "adapter %u '%s': no D3D12 device", i, name);
            a->Release();
            continue;
        }
        r->adapter = a;
        format(r->adapter_name, sizeof r->adapter_name, "%s", name);
        KV_INFO("d3d12", "adapter '%s': %llu MB dedicated video memory", name, (unsigned long long)(d.DedicatedVideoMemory >> 20));
        break;
    }
    rb_release(f6);
    if (!r->adapter) { KV_ERROR("d3d12", "no hardware adapter makes a D3D12 device"); return false; }
    return true;
}

// What the renderer needs past feature level 11_0: an unbounded SRV table (binding tier 2)
// and shader model 6.0.
bool check_features(Renderer* r) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    D3D_CHECK(r->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o));
    if (o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) {
        KV_ERROR("d3d12", "resource binding tier %d: the bindless textures need tier 2", (int)o.ResourceBindingTier);
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
    if (FAILED(r->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
        KV_ERROR("d3d12", "the device does not run shader model 6.0 (DXIL)");
        return false;
    }
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs{D3D_ROOT_SIGNATURE_VERSION_1_1};
    if (FAILED(r->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof rs)) || rs.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1) {
        KV_ERROR("d3d12", "root signature 1.1 is not supported: the runtime is older than Windows 10 1607");
        return false;
    }
    return true;
}

} // namespace

bool rb_create_device(Renderer* r) {
    if (r->validation) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        } else {
            KV_WARN("d3d12", "the debug layer was asked for and is not installed (Settings > System > Optional features > Graphics Tools)");
            r->validation = false;
        }
    }
    D3D_CHECK(CreateDXGIFactory2(r->validation ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&r->factory)));
    if (!pick_adapter(r)) return false;
    D3D_CHECK(D3D12CreateDevice(r->adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&r->device)));
    rb_name(r->device, "korvid");
    if (!check_features(r)) return false;

    if (r->validation && SUCCEEDED(r->device->QueryInterface(IID_PPV_ARGS(&r->info_queue)))) {
        r->info_queue->SetMessageCountLimit(4096);
        // Clearing to a color other than the resource's optimized one is what every frame
        // does (the clear color is the game's); that note is noise.
        D3D12_MESSAGE_ID deny[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs  = KV_COUNTOF(deny);
        filter.DenyList.pIDList = deny;
        r->info_queue->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D_CHECK(r->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&r->queue)));
    rb_name(r->queue, "direct queue");
    D3D_CHECK(r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence)));
    r->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!r->fence_event) { KV_ERROR("d3d12", "CreateEvent failed (%lu)", (unsigned long)GetLastError()); return false; }
    return true;
}

void rb_destroy_device(Renderer* r) {
    rb_release(r->info_queue);
    rb_release(r->fence);
    if (r->fence_event) CloseHandle(r->fence_event);
    r->fence_event = nullptr;
    rb_release(r->queue);
    if (r->device && r->validation) {
        // What is still alive, by name, now that everything should be gone.
        ID3D12DebugDevice* dd = nullptr;
        if (SUCCEEDED(r->device->QueryInterface(IID_PPV_ARGS(&dd)))) {
            r->device->Release();
            r->device = nullptr;
            dd->ReportLiveDeviceObjects(D3D12_RLDO_SUMMARY | D3D12_RLDO_IGNORE_INTERNAL);
            dd->Release();
        }
    }
    rb_release(r->device);
    rb_release(r->adapter);
    rb_release(r->factory);
}

void rb_drain_messages(Renderer* r) {
    ID3D12InfoQueue* q = r->info_queue;
    if (!q) return;
    u64 n = q->GetNumStoredMessages();
    for (u64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || len == 0) continue;
        alignas(8) u8 stack[1024];
        D3D12_MESSAGE* m = len <= sizeof stack ? (D3D12_MESSAGE*)stack : (D3D12_MESSAGE*)alloc_in(mem_general(), len);
        if (m && SUCCEEDED(q->GetMessage(i, m, &len))) {
            switch (m->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                ++r->validation_errors;
                KV_ERROR("d3d12", "debug layer: %s", m->pDescription);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                ++r->validation_warnings;
                KV_WARN("d3d12", "debug layer: %s", m->pDescription);
                break;
            default: break; // info and messages: what a capture tool wants, not the log
            }
        }
        if ((u8*)m != stack) free_in(mem_general(), m, len);
    }
    q->ClearStoredMessages();
}

bool rb_wait_fence(Renderer* r, u64 value) {
    if (value == 0 || r->fence->GetCompletedValue() >= value) return true;
    D3D_CHECK(r->fence->SetEventOnCompletion(value, r->fence_event));
    WaitForSingleObject(r->fence_event, INFINITE);
    return true;
}

bool rb_wait_idle(Renderer* r) {
    if (!r->queue || !r->fence) return true;
    u64 v = r->fence_next++;
    D3D_CHECK(r->queue->Signal(r->fence, v));
    return rb_wait_fence(r, v);
}

} } // namespace kv::KV_RB_NS
