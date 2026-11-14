// Korvid renderer-d3d12: the swapchain and the frames in flight. Flip model (FLIP_DISCARD),
// the only kind for D3D12: its buffers are B8G8R8A8_UNORM and the render target views over
// them B8G8R8A8_UNORM_SRGB, which encodes as Vulkan's sRGB swapchain does. Frame pacing is
// the swapchain's latency object (signaled when a buffer is free), the frames' own pacing a
// fence value each. A resize waits for the GPU: DXGI resizes only buffers nothing references,
// where Vulkan's fenced recreation retires the old chain with the frame. Resizes are rare.
#include "renderer_internal.h"

namespace kv {

bool rb_create_frames(Renderer* r) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        FrameInFlight& f = r->frames[i];
        D3D_CHECK(r->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)));
        D3D_CHECK(r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator, nullptr, IID_PPV_ARGS(&f.cmd)));
        D3D_CHECK(f.cmd->Close()); // created recording; each frame resets it
        rb_name(f.cmd, i ? "frame 1" : "frame 0");
    }
    return true;
}

void rb_destroy_frames(Renderer* r) {
    for (FrameInFlight& f : r->frames) {
        f.garbage_owned = f.garbage.len;
        rb_run_garbage(r, f);
        f.garbage.free();
        f.dirty_slots.free();
        rb_release(f.cmd);
        rb_release(f.allocator);
    }
}

namespace {

void release_buffers(Swapchain& s) {
    for (ID3D12Resource*& b : s.buffers) rb_release(b);
}

bool get_buffers(Renderer* r) {
    Swapchain& s = r->swapchain;
    DXGI_SWAP_CHAIN_DESC1 d{};
    s.handle->GetDesc1(&d);
    s.width  = d.Width;
    s.height = d.Height;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format        = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (u32 i = 0; i < kBackBuffers; ++i) {
        D3D_CHECK(s.handle->GetBuffer(i, IID_PPV_ARGS(&s.buffers[i])));
        rb_name(s.buffers[i], "back buffer");
        r->device->CreateRenderTargetView(s.buffers[i], &rv, rb_rtv(r, i));
    }
    return true;
}

UINT swapchain_flags(const Swapchain& s) {
    return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

} // namespace

void rb_destroy_swapchain(Renderer* r) {
    Swapchain& s = r->swapchain;
    release_buffers(s);
    if (s.waitable) CloseHandle(s.waitable);
    rb_release(s.handle);
    s = Swapchain{};
}

bool rb_recreate_swapchain(Renderer* r) {
    r->swapchain_dirty = false;
    Swapchain& s = r->swapchain;
    u32 w = r->want_width, h = r->want_height;
    if (w == 0 || h == 0) { // minimized: nothing to draw into until the next resize
        if (s.handle) { rb_wait_idle(r); rb_destroy_swapchain(r); }
        return true;
    }
    if (s.handle && s.width == w && s.height == h) return true;
    if (s.handle) {
        rb_wait_idle(r);
        release_buffers(s);
        D3D_CHECK(s.handle->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapchain_flags(s)));
    } else {
        IDXGIFactory5* f5 = nullptr;
        BOOL tearing = FALSE;
        if (SUCCEEDED(r->factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
            f5->Release();
        }
        s.tearing = tearing != FALSE;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width            = w;
        d.Height           = h;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = kBackBuffers;
        d.Scaling          = DXGI_SCALING_NONE;
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
        d.Flags            = swapchain_flags(s);
        IDXGISwapChain1* sc1 = nullptr;
        D3D_CHECK(r->factory->CreateSwapChainForHwnd(r->queue, r->window, &d, nullptr, nullptr, &sc1));
        HRESULT hr = sc1->QueryInterface(IID_PPV_ARGS(&s.handle));
        sc1->Release();
        D3D_CHECK(hr);
        // The window's Alt+Enter is the platform's to handle, not DXGI's.
        r->factory->MakeWindowAssociation(r->window, DXGI_MWA_NO_ALT_ENTER);
        D3D_CHECK(s.handle->SetMaximumFrameLatency(kFramesInFlight));
        s.waitable = s.handle->GetFrameLatencyWaitableObject();
    }
    if (!get_buffers(r)) return false;
    KV_INFO("d3d12", "swapchain %ux%u, %u buffers, B8G8R8A8 sRGB views, vsync %s, tearing %s", s.width, s.height, kBackBuffers,
            r->vsync ? "on" : "off", s.tearing ? "allowed" : "not supported");
    // The pipelines know only the format, which never changes: made once.
    if (!r->pipelines[0] && !rb_create_pipelines(r)) return false;
    return true;
}

} // namespace kv
