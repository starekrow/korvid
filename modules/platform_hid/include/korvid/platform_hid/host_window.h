// Korvid platform: the host window. A platform service, owned below the outer engine:
// over SDL3 on Linux and macOS, native Win32 on Windows. It is not an entity. Platform
// changes arrive as events on the bus.
#pragma once
#include "korvid/core/event.h"
#include "korvid/platform_hid/input.h"

namespace kv {

// What the window's surface is prepared for, decided at creation time.
enum class WindowSurface : u8 {
    Software, // the system draws it; present_blank() works, nothing else draws
    Vulkan,   // a renderer creates a Vulkan surface from native_handle()
    Metal,    // a renderer makes a CAMetalLayer view from native_handle() (the Apple family, through SDL)
};

struct WindowDesc {
    const char*   title   = "korvid";
    u32           width   = 1280;
    u32           height  = 720;
    bool          vsync   = true; // a preference the renderer reads; the window does not apply it
    WindowSurface surface = WindowSurface::Software;
    bool          borderless = false; // no title bar or frame; still resizable by the desktop
};

// TODO(wayland): mouse resizing stalls on GNOME Wayland. GNOME has no server-side
// decorations, so SDL3 uses libdecor, which repaints the frame synchronously inside every
// xdg configure; Mutter sends one configure per pointer motion, so the event pump never
// drains while the mouse moves. Cost scales with output size (2.5-4.3 ms per configure at
// display scale 2 on a 4K panel, enough to stall completely; ~10 fps on a laptop panel).
// Keyboard resizes and borderless windows are unaffected. Remedy when it matters: open
// borderless on Wayland and provide edge resizing through SDL_SetWindowHitTest, so the
// engine owns the frame and each configure costs only a few protocol calls. Diagnose with
// `example_testbed --hitch-ms 50` plus WAYLAND_DEBUG=1; the hitch lines use libwayland's timestamps.

struct HostWindow {
    // Creates the window; false (with the reason logged) if the system refuses. Asserts if
    // open. width and height are logical: scaled by the display's DPI on Windows.
    bool open(const WindowDesc& desc);
    void close();
    bool is_open() const { return native_ != nullptr; }

    // The framebuffer in pixels (not window units: a scaled desktop differs), as of the
    // last pump. The renderer is told at render rate when it changes.
    u32  width()  const { return width_; }
    u32  height() const { return height_; }
    bool vsync()  const { return vsync_; }  // the preference from the desc; the renderer applies it
    bool focused() const { return focused_; } // input focus, as of the last pump

    // Translates OS events into bus events and updates input.raw. Call once per frame.
    void pump(EventBus& bus, Input& input);
    // Framebuffer pixels per window unit: what pointer coordinates are scaled by.
    f32  pixel_scale() const;

    // For WindowSurface::Software only: fills the window with a flat color. Wayland maps a
    // window only after a buffer is attached to its surface, so a window nobody draws to
    // never appears. A no-op on a Vulkan window, where the renderer presents.
    void present_blank();
    // Text input from the OS on or off (an input method's window, a phone's keyboard).
    void text_input(bool on);

    // The window as an opaque pointer: an SDL_Window* where SDL is the backend, an HWND on
    // Windows. The renderer turns it into a surface on its side; no graphics API headers
    // are included here.
    void* native_handle() const { return native_; }

    // The fields are the module's; read them through the accessors.
    void*         native_     = nullptr;
    WindowSurface surface_    = WindowSurface::Software;
    u32   width_      = 0;
    u32   height_     = 0;
    bool  vsync_      = true;
    bool  focused_    = false;
};

} // namespace kv
