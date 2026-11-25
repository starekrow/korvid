// Korvid platform: the host window. A platform service, owned below the outer engine:
// over SDL3 on Linux and macOS, native Win32 on Windows, native UIKit on iOS. It is not an entity. Platform
// changes arrive as events on the bus.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/math/rect2.h"
#include "korvid/platform_hid/events.h"
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
    // An alpha channel and no frame: what is drawn clear shows what is behind the window,
    // where the platform composites (HostWindow::transparent() says whether it does). The
    // renderer is told through RendererDesc::transparent and presents premultiplied alpha.
    bool          transparent = false;
};

// What a part of a window is for, when the OS asks what is under a point (HostWindow::grips):
// the OS moves the window from a Move grip and resizes it from an edge or a corner, with its
// own snapping, tiling and keyboard. Edges and corners by compass point.
enum class WindowGrip : u8 { Move, N, NE, E, SE, S, SW, W, NW, None };
// One part: a rect in framebuffer pixels, top-left origin, and what it is for.
struct WindowGripRect { rect2 rect; WindowGrip grip; };
// The most a window keeps; more are dropped, with a warning once.
constexpr u32 KV_WINDOW_GRIPS = 64;

// TODO(wayland): mouse resizing stalls on GNOME Wayland. GNOME has no server-side
// decorations, so SDL3 uses libdecor, which repaints the frame synchronously inside every
// xdg configure; Mutter sends one configure per pointer motion, so the event pump never
// drains while the mouse moves. Cost scales with output size (2.5-4.3 ms per configure at
// display scale 2 on a 4K panel, enough to stall completely; ~10 fps on a laptop panel).
// Keyboard resizes and borderless windows are unaffected. A borderless window resized by
// its own edges (HostWindow::grips, SDL_SetWindowHitTest underneath) owns its frame, and
// each configure costs only a few protocol calls: a program that draws its frame does not
// stall. What is left: a window with the desktop's frame still stalls, and the frame loop
// opens one unless --borderless. Diagnose with `testbed --hitch-ms 50` plus WAYLAND_DEBUG=1;
// the hitch lines use libwayland's timestamps.

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
    // Whether it is seen, as of the last pump, and if not why (WindowVisibility).
    bool         visible() const { return hidden_ == 0; }
    WindowHidden hidden_why() const;
    // The backend's: one reason to be hidden (Hidden* below) on or off, as the OS says it.
    // After a pump the module calls send_visibility, which emits WindowVisibility once if
    // whether it is seen, or why, is not what was last sent: a restore that clears one
    // reason and then another is one event.
    enum : u8 { HiddenMinimized = 1, HiddenUnmapped = 2, HiddenOccluded = 4 };
    void set_hidden(EventBus& bus, u8 reason, bool on);
    void send_visibility(EventBus& bus);

    // Translates OS events into bus events and updates input.raw. Call once per frame.
    void pump(EventBus& bus, Input& input);
    // The same for the events already queued, without asking the OS for more: for code
    // running inside an OS modal loop (on_modal), where asking would re-enter the loop
    // that holds the thread.
    void drain(EventBus& bus, Input& input);
    // Blocks till the OS has something for the next pump, wake() is called, or `seconds`
    // pass (KR26), and takes nothing off the queue. True when it ended before the time.
    // The module's waiter for platform_wait.
    bool wait(f64 seconds);
    // Ends the wait under way, or the next to begin. Any thread.
    void wake();
    // Seconds from the OS's stamp on the last key or button pressed to the pump that read
    // it (on Win32, from the window thread's queueing of it): a measure of how soon the loop
    // saw it. 0 where nothing is stamped.
    f64  input_delay() const { return input_delay_; }
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
    // For WindowSurface::Software: the window's pixels to draw into (a CPU renderer's,
    // renderer-cpu), until present_software() shows them. Four bytes a pixel, rows `pitch`
    // bytes apart; R, G, B then the fourth (ignored) in memory when `rgba`, else B, G, R. The
    // size is the window's now. False when there is no window, it is not a software one, or
    // the system will not give its pixels.
    // `rotates`: each frame is a different buffer (iOS's ring), so last frame's pixels are not
    // in it; a caller that redraws only what changed keeps its own and copies it in.
    // `alpha`: the fourth byte is alpha, premultiplied, and shows: a transparent window's
    // (transparent()). Else it is ignored, and the frame is opaque.
    struct SoftwareFrame { u8* pixels = nullptr; u32 width = 0, height = 0, pitch = 0; bool rgba = false; bool rotates = false; bool alpha = false; };
    bool software_frame(SoftwareFrame* out);
    // The size the next software_frame() is asked to be, smaller than the window's: a host
    // that can scale a frame up to the window as it shows it (iOS: Core Animation does) gives
    // a frame this size, nearest-neighbour scaled. Others give the window's size as always,
    // and the caller scales. 0 x 0: the window's size.
    void software_size(u32 w, u32 h) { software_want_[0] = w; software_want_[1] = h; }
    void present_software();
    // present_software() waiting for the display (vsync), kept across the surface's
    // re-creation on a resize. True when the system does it: the presents then pace the
    // frames, as a swapchain's do. False where it cannot (a Win32 DIB): the caller paces.
    bool software_vsync(bool on);
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
    // Minimizes the window, or restores it: for tests and demo scripts (KR21's). Wayland
    // restores nothing a program asks to: the user does. Nothing on a phone.
    void minimize(bool on);
    // While on, the pointer keeps reporting to this window outside it (a drag); false where
    // the platform cannot.
    bool capture_mouse(bool on);

    // ---- a window that is not a rectangle --------------------------------------------
    // Whether what is drawn clear shows what is behind: asked for (WindowDesc::transparent)
    // and composited. False where the system draws no alpha (X11 with no compositor): the
    // window is opaque there, and shape() is what cuts it out.
    bool transparent() const { return transparent_; }
    // The window's input shape: `alpha`, one byte a pixel, `w` x `h` with rows `w` bytes
    // apart, scaled to the window. Where it is under 128 the window takes no input, and a
    // click there goes to what is behind; where there is no compositor it is cut out of
    // the picture too, one bit a pixel. Copied: give it again when the picture changes; a
    // resize keeps it, scaled to the new size. Null for the whole rect. Drawn alpha and
    // the shape are two things: a soft edge is drawn at half alpha and is in or out here.
    // False (with a line logged, once) where the window is not a transparent one, or the
    // platform cannot. Allocates a copy, and costs O(window pixels) now and at each resize.
    bool shape(const u8* alpha, u32 w, u32 h);
    // What each part of the window is for (WindowGrip), for a window with no frame: when the
    // OS asks what is under a point, the last rect in the list that holds it answers, and
    // the OS moves or resizes the window itself. A press on a grip is the OS's: the program
    // sees no button for it. Copied, KV_WINDOW_GRIPS at most; an empty list gives the
    // window back its ordinary input. False where the platform cannot ask (a phone).
    bool grips(const WindowGripRect* list, u32 count);
    // What the last grips() list says is under (x, y), framebuffer pixels: None when no rect
    // holds it. What a backend answers the OS with, from the thread that pumps.
    WindowGrip grip_at(f32 x, f32 y) const;
    // What a resize, the OS's or the user's, is held to, in framebuffer pixels: 0 for no
    // bound. Kept, and given again as the scale changes.
    void bounds(u32 min_w, u32 min_h, u32 max_w, u32 max_h);

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
    u8    hidden_     = 0;    // Hidden* reasons in force
    WindowHidden hidden_sent_ = WindowHidden::None; // what the last WindowVisibility said
    f32   insets_[4]  = {};
    void (*modal_fn_)(void*) = nullptr;
    void* modal_ctx_  = nullptr;
    void (*surface_fn_)(void*, void*) = nullptr;
    void* surface_ctx_ = nullptr;
    void* modal_timer_ = nullptr; // the backend's: on macOS a CFRunLoopTimer in the event-tracking mode
    void* software_    = nullptr; // the backend's: the surface software_frame() locked, until present_software()
    void* software_vsynced_ = nullptr; // SDL: the surface software_vsync() was last applied to
    bool  software_vsync_   = false;   // what software_vsync() asked for
    u32   software_want_[2] = {};      // software_size()'s
    f64   input_delay_ = 0.0;          // input_delay()'s
    u32   wake_queued_ = 0;            // atomic: a wake the backend queued that no pump has taken yet
    bool  transparent_ = false;        // transparent()'s
    Array<u8> shape_;                  // shape()'s mask, as given; empty: none
    u32   shape_w_ = 0, shape_h_ = 0;
    WindowGripRect grips_[KV_WINDOW_GRIPS] = {}; // grips()'s list
    u32   grip_count_ = 0;
    u32   bounds_[4] = {};             // bounds()': min w, min h, max w, max h
    f32   bounds_scale_ = 0.0f;        // the backend's: the pixel scale they were last applied at
};

} // namespace kv
