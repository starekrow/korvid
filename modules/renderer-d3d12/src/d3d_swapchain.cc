// Korvid renderer-d3d12: the swapchain and the frames in flight. Flip model (FLIP_DISCARD),
// the only kind for D3D12: its buffers are B8G8R8A8_UNORM and the render target views over
// them B8G8R8A8_UNORM_SRGB, which encodes as Vulkan's sRGB swapchain does. Frame pacing is
// the swapchain's latency object (signaled when a buffer is free), the frames' own pacing a
// fence value each. A resize waits for the GPU: DXGI resizes only buffers nothing references,
// where Vulkan's fenced recreation retires the old chain with the frame. Resizes are rare.
//
// A transparent window (slice 13, KR39) has no redirection bitmap, and an HWND swapchain
// presents no alpha. Its swapchain is made for composition instead (premultiplied, stretch
// scaling, no tearing), and DirectComposition shows it: a desktop device, a target for the
// window and one visual whose content it is. The sprite and UI blends keep the frame
// premultiplied over a clear of nothing, as Vulkan's do.
#include "renderer_internal.h"
#include "korvid/core/format.h"
#include "korvid/core/time.h"
#pragma comment(lib, "dcomp.lib")

namespace kv { namespace KV_RB_NS {

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
        rc_run_garbage(r, f);
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

// The chain's buffers and their views, at RTVs rtv_base and on.
bool get_buffers(Renderer* r, Swapchain& s, u32 rtv_base) {
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
        r->device->CreateRenderTargetView(s.buffers[i], &rv, rb_rtv(r, rtv_base + i));
    }
    return true;
}

UINT swapchain_flags(const Swapchain& s) {
    return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (s.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
}

// The swapchain given to the window through DirectComposition, its objects made the first
// time. Each new swapchain (after a minimize) is set as the visual's content again.
bool compose(Swapchain& s, HWND window, IDXGISwapChain1* sc) {
    if (!s.dcomp_device) {
        IDCompositionDesktopDevice* dev = nullptr;
        D3D_CHECK(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&dev)));
        s.dcomp_device = dev;
        IDCompositionTarget* target = nullptr;
        D3D_CHECK(dev->CreateTargetForHwnd(window, TRUE, &target));
        s.dcomp_target = target;
        IDCompositionVisual2* visual = nullptr;
        D3D_CHECK(dev->CreateVisual(&visual));
        s.dcomp_visual = visual;
        D3D_CHECK(target->SetRoot(visual));
    }
    D3D_CHECK(((IDCompositionVisual2*)s.dcomp_visual)->SetContent(sc));
    D3D_CHECK(((IDCompositionDesktopDevice*)s.dcomp_device)->Commit());
    return true;
}

void destroy_chain(Swapchain& s) {
    if (s.dcomp_visual) ((IDCompositionVisual2*)s.dcomp_visual)->SetContent(nullptr);
    if (s.dcomp_device) ((IDCompositionDesktopDevice*)s.dcomp_device)->Commit();
    rb_release(s.dcomp_visual);
    rb_release(s.dcomp_target);
    rb_release(s.dcomp_device);
    release_buffers(s);
    if (s.waitable) CloseHandle(s.waitable);
    rb_release(s.handle);
    s = Swapchain{};
}

// A chain for `window` at w x h, made or resized: the main window's (RTVs 0 on) or another
// surface's. A transparent window's is a composition one. 0 x 0 (minimized) lets go of it.
// `label`: the log line's name for it.
bool make_chain(Renderer* r, Swapchain& s, HWND window, u32 w, u32 h, bool transparent, u32 rtv_base, const char* label) {
    if (w == 0 || h == 0) { // minimized: nothing to draw into until the next resize
        if (s.handle) { rb_wait_idle(r); destroy_chain(s); }
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
        s.tearing = tearing != FALSE && !transparent; // a composition swapchain does not tear
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width            = w;
        d.Height           = h;
        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = kBackBuffers;
        d.Scaling          = transparent ? DXGI_SCALING_STRETCH : DXGI_SCALING_NONE; // composition takes only stretch
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode        = transparent ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
        d.Flags            = swapchain_flags(s);
        IDXGISwapChain1* sc1 = nullptr;
        if (transparent) D3D_CHECK(r->factory->CreateSwapChainForComposition(r->queue, &d, nullptr, &sc1));
        else D3D_CHECK(r->factory->CreateSwapChainForHwnd(r->queue, window, &d, nullptr, nullptr, &sc1));
        bool shown = !transparent || compose(s, window, sc1);
        HRESULT hr = sc1->QueryInterface(IID_PPV_ARGS(&s.handle));
        sc1->Release();
        D3D_CHECK(hr);
        if (!shown) return false;
        // The window's Alt+Enter is the platform's to handle, not DXGI's.
        r->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        D3D_CHECK(s.handle->SetMaximumFrameLatency(kFramesInFlight));
        s.waitable = s.handle->GetFrameLatencyWaitableObject();
    }
    if (!get_buffers(r, s, rtv_base)) return false;
    KV_INFO("d3d12", "%s %ux%u, %u buffers, B8G8R8A8 sRGB views, vsync %s, tearing %s%s", label, s.width, s.height, kBackBuffers,
            r->vsync ? "on" : "off", s.tearing ? "allowed" : "not supported",
            transparent ? ", premultiplied alpha through DirectComposition" : "");
    return true;
}

} // namespace

void rb_destroy_swapchain(Renderer* r) { destroy_chain(r->swapchain); }

bool rb_recreate_swapchain(Renderer* r) {
    r->swapchain_dirty = false;
    if (!make_chain(r, r->swapchain, r->window, r->want_width, r->want_height, r->transparent, 0, "swapchain")) return false;
    // The pipelines know only the format, which never changes: made once.
    if (r->swapchain.handle && !r->pipelines[0] && !rb_create_pipelines(r)) return false;
    return true;
}

// ---- other windows' surfaces ------------------------------------------------------------

// Their chains are made at their first acquire, at the size they have then.
bool rb_surface_create(Renderer*, Surface& s) {
    s.chain = Swapchain{};
    s.image = 0;
    return true;
}

void rb_surface_destroy(Renderer* r, Surface& s) {
    if (s.chain.handle) rb_wait_idle(r); // its buffers may be in a frame in flight
    destroy_chain(s.chain);
}

bool rb_surface_acquire(Renderer* r, FrameInFlight&, Surface& s, f64* waited) {
    if (s.resized || !s.chain.handle) {
        char label[32];
        format(label, sizeof label, "surface %u", r->surface_at);
        if (!make_chain(r, s.chain, (HWND)s.native, s.want_width, s.want_height, s.transparent, kRtvSurfaces + r->surface_at * kBackBuffers, label)) return false;
    }
    s.have_image = s.chain.handle != nullptr && r->ui_pipeline != nullptr;
    s.width  = s.chain.width;
    s.height = s.chain.height;
    if (s.have_image) {
        KV_ZONE_WAIT("acquire surface");
        f64 t = time_now();
        WaitForSingleObjectEx(s.chain.waitable, 1000, TRUE);
        *waited += time_now() - t;
        s.image = s.chain.handle->GetCurrentBackBufferIndex();
    }
    return true;
}

bool rb_surface_present(Renderer* r, FrameInFlight&, Surface& s, f64* waited) {
    KV_ZONE_WAIT("present surface");
    f64 t = time_now();
    UINT flags = !r->vsync && s.chain.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = s.chain.handle->Present(r->vsync ? 1 : 0, flags);
    *waited += time_now() - t;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        KV_ERROR("d3d12", "Present (surface %u): %s (removed: %s)", r->surface_at, hr_name(hr), hr_name(r->device->GetDeviceRemovedReason()));
        return false;
    }
    if (FAILED(hr)) KV_ERROR("d3d12", "Present (surface %u): %s", r->surface_at, hr_name(hr));
    return true;
}

} } // namespace kv::KV_RB_NS
