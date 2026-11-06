// Korvid platform: the host window. A platform service over SDL3, owned below the
// outer engine. It is not an entity. Platform changes arrive as events on the bus.
#pragma once
#include "korvid/core/event.h"
#include "korvid/platform/input.h"

namespace kv {

struct WindowDesc {
    const char* title  = "korvid";
    u32         width  = 1280;
    u32         height = 720;
    bool        vsync  = true; // a preference the renderer reads; SDL does not apply it
};

struct HostWindow {
    bool open(const WindowDesc& desc);
    void close();
    bool is_open() const { return sdl_window_ != nullptr; }

    u32  width()  const { return width_; }
    u32  height() const { return height_; }
    bool vsync()  const { return vsync_; }
    bool focused() const { return focused_; }

    // Translates OS events into bus events and updates input.raw. Call once per frame.
    void pump(EventBus& bus, Input& input);

    // Stand-in until a renderer presents frames. Wayland maps a window only after a
    // buffer is attached to its surface, so a window nobody draws to never appears.
    // Uses SDL's software framebuffer; a Vulkan swapchain replaces it and this goes away.
    void present_blank();

    // The SDL_Window* as an opaque pointer. The renderer module turns it into a
    // surface on its side; no graphics API headers are included here.
    void* native_handle() const { return sdl_window_; }

    void* sdl_window_ = nullptr;
    u32   width_      = 0;
    u32   height_     = 0;
    bool  vsync_      = true;
    bool  focused_    = false;
};

} // namespace kv
