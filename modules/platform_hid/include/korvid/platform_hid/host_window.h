// Korvid platform: the host window. A platform service, owned below the outer engine:
// over SDL3 on Linux and macOS, native Win32 on Windows, native UIKit on iOS. It is not an entity. Platform
// changes arrive as events on the bus.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/math/rect2.h"
#include "korvid/platform_hid/input.h"

namespace kv {

// What the window's surface is prepared for, decided at creation time.
enum class WindowSurface : u8 {
    Software, // the system draws it; present_blank() works, nothing else draws
    Vulkan,   // a renderer creates a Vulkan surface from native_handle()
    Metal,    // a renderer finds a CAMetalLayer through native_handle(): an SDL window's view on macOS, the UIView on iOS
    D3D12,    // a renderer makes a DXGI swapchain for native_handle(), the HWND (Windows only)
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
// `testbed --hitch-ms 50` plus WAYLAND_DEBUG=1; the hitch lines use libwayland's timestamps.

// The process's entry, for a frame loop's main: calls run(argc, argv, ctx) and returns what
// it returns, as a plain call on the desktop. On iOS the OS owns the main thread's loop:
// this starts the application (UIApplicationMain, which never returns), runs `run` once it
// has launched, from inside the OS's loop, and ends the process with its result. A HostWindow
// there pumps that loop itself, so `run` is the same loop on every platform.
int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx);

// The pointer's shape over the window (platform_cursor). Nothing on a touch screen.
enum class CursorShape : u8 { Arrow, IBeam, Hand, Move, ResizeEW, ResizeNS, Forbidden, Count };

// Typed text from a host: into the input state, and as TextTyped events (split at code
// points). Every host's text goes through it.
void input_typed(EventBus& bus, Input& input, const char* utf8);

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
    // The same for the events already queued, without asking the OS for more: for code
    // running inside an OS modal loop (on_modal), where asking would re-enter the loop
    // that holds the thread.
    void drain(EventBus& bus, Input& input);
    // Some OSes hold the thread that pumps in a loop of their own while the window is being
    // resized: on macOS a live resize runs AppKit's event-tracking loop inside pump until
    // the mouse is released, so no frame runs for as long as the drag. While such a loop
    // runs, the backend calls fn(ctx) about every 4 ms from inside it. Null stops it. A
    // backend whose window has a thread of its own (Win32) never calls it. The module sets
    // this; a frame loop asks for it through platform_modal_frame.
    void on_modal(void (*fn)(void* ctx), void* ctx);
    // Framebuffer pixels per window unit: what pointer coordinates are scaled by.
    f32  pixel_scale() const;
    // The framebuffer's edges that the system covers or rounds off (a phone's camera
    // housing, its corners, the home indicator), in pixels: left, top, right, bottom. Zero
    // on the desktop. Controls drawn on the screen stay inside them.
    void safe_insets(f32 out[4]) const { for (u32 i = 0; i < 4; ++i) out[i] = insets_[i]; }
    // Whether the screen is touched rather than pointed at: a phone's or a tablet's.
    bool touch_screen() const;

    // For WindowSurface::Software only: fills the window with a flat color. Wayland maps a
    // window only after a buffer is attached to its surface, so a window nobody draws to
    // never appears. A no-op on a Vulkan window, where the renderer presents.
    void present_blank();
    // Text input from the OS on or off (an input method's window, a phone's keyboard).
    void text_input(bool on);
    // Where the caret is, in framebuffer pixels: the input method's candidates open beside
    // it, and a phone's keyboard keeps out of its way. `caret` is its x within the rect.
    void text_area(rect2 r, f32 caret);
    // The system clipboard's text (UTF-8), appended to `out`; false when it has none or the
    // host cannot read it. Setting it: false when the host cannot.
    bool clipboard_get(Array<char>& out);
    bool clipboard_set(const char* utf8);
    void cursor(CursorShape shape);

    // Some OSes take the window's surface away while the program runs and give another
    // later (Android, whenever the app leaves the screen). fn(ctx, null) is called from
    // inside pump before the old surface is gone: whoever draws to it lets go there, since
    // on return it is destroyed. fn(ctx, native) is called, from pump too, when there is a
    // new one, which native_handle() then returns. Never called where the window's surface
    // lives as long as the window. Null stops it. A frame loop asks through
    // platform_surface_hook.
    void on_surface(void (*fn)(void* ctx, void* native), void* ctx) { surface_fn_ = fn; surface_ctx_ = ctx; }

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
    f32   insets_[4]  = {};
    void (*modal_fn_)(void*) = nullptr;
    void* modal_ctx_  = nullptr;
    void (*surface_fn_)(void*, void*) = nullptr;
    void* surface_ctx_ = nullptr;
    void* modal_timer_ = nullptr; // the backend's: on macOS a CFRunLoopTimer in the event-tracking mode
};

} // namespace kv
