// Korvid render: the renderer as the game sees it. Link-time dispatch: the bound backend
// module defines these. No graphics API type appears here; the window is an opaque handle.
#pragma once
#include "korvid/render/packet.h"

namespace kv {

struct Renderer; // opaque; owned by the backend

struct RendererDesc {
    void* native_window = nullptr; // HostWindow::native_handle()
    u32   width         = 0;       // current framebuffer size in pixels
    u32   height        = 0;
    bool  vsync         = true;
    bool  validation    = KV_DEBUG != 0; // request the validation layers when present
};

struct RenderFrame {
    Span<const RenderPacket> packets;
    f32   alpha = 0.0f; // interpolation point between prev and cur, [0, 1)
    rect2 view;         // the world rect that fills the window, +y up
    color clear_color{0.09f, 0.09f, 0.11f, 1.0f};
};

Renderer* renderer_create(const RendererDesc& desc);
void      renderer_destroy(Renderer* r);

// The window's framebuffer size changed; the backend recreates what depends on it.
void      renderer_resize(Renderer* r, u32 width, u32 height);

// Draws and presents one frame. Returns false when nothing was presented
// (zero-size window, swapchain being rebuilt); that is not an error.
bool      renderer_render(Renderer* r, const RenderFrame& frame);

} // namespace kv
