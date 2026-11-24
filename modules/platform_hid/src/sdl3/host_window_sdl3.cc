#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/log.h"
#include "korvid/core/utf8.h"
#include <string.h>
#include "../hid_backend.h"
#include "../sdl3_pads/sdl_pads.h"
#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// macOS's live resize (below): AppKit's, so not on iOS, where a window is the screen.
#if defined(__APPLE__) && TARGET_OS_OSX
#define KV_LIVE_RESIZE_TIMER 1
#include <CoreFoundation/CoreFoundation.h>
#else
#define KV_LIVE_RESIZE_TIMER 0
#endif

namespace kv {

#if KV_LIVE_RESIZE_TIMER
// A live resize runs AppKit's event-tracking loop on the main thread, inside SDL_PollEvent,
// until the mouse is released: pump does not return, and no frame runs. SDL sends live-resize
// expose events from inside it, but only while the size changes, not while the mouse holds
// still. A run-loop timer in the event-tracking mode alone fires only there, steadily, and
// never during an ordinary pump (the default mode): the window's on_modal hook runs from it.
// The mode's name is AppKit's NSEventTrackingRunLoopMode, as a CFString.
namespace {
constexpr CFTimeInterval kModalInterval = 0.004;

void modal_tick(CFRunLoopTimerRef, void* info) {
    HostWindow* w = (HostWindow*)info;
    if (w->modal_fn_) w->modal_fn_(w->modal_ctx_);
}

void* modal_timer_start(HostWindow* w) {
    CFRunLoopTimerContext ctx = {0, w, nullptr, nullptr, nullptr};
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kModalInterval,
                                               kModalInterval, 0, 0, modal_tick, &ctx);
    if (t) CFRunLoopAddTimer(CFRunLoopGetMain(), t, CFSTR("NSEventTrackingRunLoopMode"));
    return t;
}

void modal_timer_stop(void* timer) {
    if (!timer) return;
    CFRunLoopTimerInvalidate((CFRunLoopTimerRef)timer);
    CFRelease((CFRunLoopTimerRef)timer);
}
} // namespace
#endif

bool hid_video_up() {
    if (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        KV_ERROR("hid", "SDL video unavailable: %s", SDL_GetError());
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) KV_WARN("input", "SDL gamepads unavailable: %s", SDL_GetError());
    KV_INFO("hid", "video driver '%s'", SDL_GetCurrentVideoDriver());
    return true;
}

void hid_video_down() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool hid_backend_startup() {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    // Headless until a window opens: video and gamepads come up with the first window
    // (HostWindow::open), so tests run where there is no display.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        KV_ERROR("hid", "SDL events unavailable: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("hid", "SDL %d.%d.%d, headless until a window opens",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver));
    return true;
}

void hid_backend_shutdown() {
    hid_video_down();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    if (SDL_WasInit(0) == 0) SDL_Quit(); // the last SDL user out (platform_audio may still hold audio)
}

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) return false; // the module starts headless
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (desc.surface == WindowSurface::Vulkan) flags |= SDL_WINDOW_VULKAN; // also loads the Vulkan loader
    if (desc.surface == WindowSurface::Metal) flags |= SDL_WINDOW_METAL;   // SDL_Metal_CreateView needs it
    if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS;
#if defined(__APPLE__)
    // Retina: without this the backing store is in window points and the system scales it
    // up, so a 2x display shows a quarter of the pixels. With it the framebuffer is in
    // pixels, which is what width() and height() promise, and pointers scale by
    // pixel_scale(). (Linux at a Wayland scale renders at window size today; the flag
    // would change that and every measurement made there, so it is not set there yet.)
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    SDL_Window* w = SDL_CreateWindow(desc.title, (int)desc.width, (int)desc.height, flags);
    if (!w) {
        KV_ERROR("window", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    native_ = w;
    surface_    = desc.surface;
    vsync_      = desc.vsync;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    width_   = (u32)pw;
    height_  = (u32)ph;
    focused_ = (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0;
#if KV_LIVE_RESIZE_TIMER
    modal_timer_ = modal_timer_start(this);
#endif
    KV_INFO("window", "opened '%s' %ux%u px", desc.title, width_, height_);
    return true;
}

void HostWindow::close() {
    if (!native_) return;
#if KV_LIVE_RESIZE_TIMER
    modal_timer_stop(modal_timer_);
    modal_timer_ = nullptr;
#endif
    SDL_DestroyWindow((SDL_Window*)native_);
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    hidden_  = 0;
    hidden_sent_ = WindowHidden::None;
    KV_INFO("window", "closed");
}

void HostWindow::present_blank() {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w || surface_ != WindowSurface::Software) return;
    SDL_Surface* s = SDL_GetWindowSurface(w);
    if (!s) {
        KV_WARN("window", "SDL_GetWindowSurface failed: %s", SDL_GetError());
        return;
    }
    SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGB(s, 24, 24, 28));
    SDL_UpdateWindowSurface(w);
}

bool HostWindow::software_frame(SoftwareFrame* out) {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w || surface_ != WindowSurface::Software) return false;
    SDL_Surface* s = SDL_GetWindowSurface(w);
    static bool told = false;
    if (!s) {
        if (!told) KV_WARN("window", "SDL_GetWindowSurface failed: %s", SDL_GetError());
        told = true;
        return false;
    }
    bool rgba;
    switch (s->format) {
    case SDL_PIXELFORMAT_XRGB8888: case SDL_PIXELFORMAT_ARGB8888: rgba = false; break; // B, G, R, X in memory
    case SDL_PIXELFORMAT_XBGR8888: case SDL_PIXELFORMAT_ABGR8888: rgba = true; break;  // R, G, B, X
    default:
        if (!told) KV_WARN("window", "the window's surface is %s, not 32 bits a pixel: nothing drawn", SDL_GetPixelFormatName(s->format));
        told = true;
        return false;
    }
    // A new surface (the first, or one a resize made) starts with vsync off.
    if (software_vsync_ && software_vsynced_ != s) {
        SDL_SetWindowSurfaceVSync(w, 1);
        software_vsynced_ = s;
    }
    if (SDL_MUSTLOCK(s) && !SDL_LockSurface(s)) return false;
    software_ = s;
    *out = SoftwareFrame{(u8*)s->pixels, (u32)s->w, (u32)s->h, (u32)s->pitch, rgba};
    return true;
}

void HostWindow::present_software() {
    SDL_Window* w = (SDL_Window*)native_;
    SDL_Surface* s = (SDL_Surface*)software_;
    if (!w || !s) return;
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    software_ = nullptr;
    SDL_UpdateWindowSurface(w);
}

bool HostWindow::software_vsync(bool on) {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w || surface_ != WindowSurface::Software) return false;
    software_vsync_ = on;
    software_vsynced_ = nullptr;
    if (!SDL_GetWindowSurface(w)) return false;
    if (!SDL_SetWindowSurfaceVSync(w, on ? 1 : SDL_WINDOW_SURFACE_VSYNC_DISABLED)) {
        KV_INFO("window", "the window's surface has no vsync (%s): the frame loop paces it", SDL_GetError());
        software_vsync_ = false;
        return false;
    }
    software_vsynced_ = SDL_GetWindowSurface(w);
    return on;
}

void HostWindow::text_input(bool on) {
    if (!native_) return;
    if (on) SDL_StartTextInput((SDL_Window*)native_);
    else SDL_StopTextInput((SDL_Window*)native_);
}

void HostWindow::text_area(rect2 r, f32 caret) {
    if (!native_) return;
    // SDL wants window units; the rect is framebuffer pixels.
    f32 k = pixel_scale();
    SDL_Rect rr{(int)(r.x / k), (int)(r.y / k), (int)(r.w / k), (int)(r.h / k)};
    SDL_SetTextInputArea((SDL_Window*)native_, &rr, (int)(caret / k));
}

bool HostWindow::clipboard_get(Array<char>& out) {
    if (!native_ || !SDL_HasClipboardText()) return false;
    char* t = SDL_GetClipboardText();
    if (!t) return false;
    usize n = strlen(t);
    if (n) memcpy(out.push_n((u32)n), t, n);
    SDL_free(t);
    return n > 0;
}

bool HostWindow::clipboard_set(const char* utf8) { return native_ && SDL_SetClipboardText(utf8 ? utf8 : ""); }

void HostWindow::minimize(bool on) {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w) return;
    if (on) SDL_MinimizeWindow(w);
    else SDL_RestoreWindow(w);
}

void HostWindow::cursor(CursorShape shape) {
    if (!native_ || (u32)shape >= (u32)CursorShape::Count) return;
    static SDL_Cursor* made[(u32)CursorShape::Count] = {};
    static const SDL_SystemCursor which[] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_POINTER, SDL_SYSTEM_CURSOR_MOVE,
                                             SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NS_RESIZE, SDL_SYSTEM_CURSOR_NOT_ALLOWED};
    SDL_Cursor*& c = made[(u32)shape];
    if (!c) c = SDL_CreateSystemCursor(which[(u32)shape]);
    if (c) SDL_SetCursor(c);
}

// A drag that leaves the window: SDL counts motion outside a window that holds no capture
// as the pointer leaving, and lets go of its buttons, so a press is cut at the edge. Where
// SDL cannot capture (Wayland), the pointer is confined to the window instead while the
// button is down: the drag goes on, the cursor held at the edge, until the release.
bool HostWindow::capture_mouse(bool on) {
    if (!native_) return false;
    static int capture = -1; // -1 not tried; 1 SDL captures; 0 it cannot: confine instead
    if (capture != 0) {
        bool ok = SDL_CaptureMouse(on);
        if (capture < 0 && on) {
            capture = ok ? 1 : 0;
            if (!ok) KV_INFO("input", "mouse capture: %s; a press confines the pointer to the window instead", SDL_GetError());
        }
        if (capture == 1) return ok;
    }
    return SDL_SetWindowMouseGrab((SDL_Window*)native_, on);
}

// Framebuffer pixels per window unit (2 on a scaled desktop); 1 when SDL has no answer.
f32 HostWindow::pixel_scale() const {
    int ww = 0, wh = 0;
    if (!native_ || !SDL_GetWindowSize((SDL_Window*)native_, &ww, &wh) || ww <= 0) return 1.0f;
    return (f32)width_ / (f32)ww;
}

namespace {

// KV_TRACE_MOUSE=1 in the environment: what SDL says of the pointer at the window's edge
// (entering and leaving, buttons, motion outside it), for a drag that stops there.
bool trace_mouse() {
    static int on = -1;
    if (on < 0) { const char* v = SDL_getenv("KV_TRACE_MOUSE"); on = v && *v && *v != '0'; }
    return on != 0;
}

// One OS event into bus events and raw input: pump's and drain's.
void translate(HostWindow& w, const SDL_Event& e, EventBus& bus, Input& input) {
        if (trace_mouse()) {
            switch (e.type) {
            case SDL_EVENT_WINDOW_MOUSE_ENTER: KV_INFO("input", "trace: the pointer entered the window"); break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: KV_INFO("input", "trace: the pointer left the window"); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: KV_INFO("input", "trace: the window lost the focus"); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                KV_INFO("input", "trace: button %u %s at (%.0f, %.0f)", e.button.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up", e.button.x, e.button.y);
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                int ww = 0, wh = 0;
                SDL_GetWindowSize((SDL_Window*)w.native_, &ww, &wh);
                if (e.motion.x < 0 || e.motion.y < 0 || e.motion.x >= ww || e.motion.y >= wh)
                    KV_INFO("input", "trace: motion outside the window at (%.0f, %.0f), buttons %x", e.motion.x, e.motion.y, e.motion.state);
                else if (e.motion.x == 0 || e.motion.y == 0 || e.motion.x == ww - 1 || e.motion.y == wh - 1)
                    KV_INFO("input", "trace: motion at the window's edge (%.0f, %.0f), buttons %x", e.motion.x, e.motion.y, e.motion.state);
                break;
            }
            default: break;
            }
        }
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            bus.emit(CloseRequested{0});
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            u32 nw = (u32)e.window.data1, nh = (u32)e.window.data2;
            if (nw != w.width_ || nh != w.height_) {
                w.width_  = nw;
                w.height_ = nh;
                bus.emit(WindowResized{nw, nh});
            }
            break;
        }

        // Seen or not (KR21). Wayland has no minimized state to report; GNOME and KDE say a
        // surface is suspended, which SDL gives as occluded.
        case SDL_EVENT_WINDOW_MINIMIZED: w.set_hidden(bus, HostWindow::HiddenMinimized, true); break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED: w.set_hidden(bus, HostWindow::HiddenMinimized, false); break;
        case SDL_EVENT_WINDOW_HIDDEN:    w.set_hidden(bus, HostWindow::HiddenUnmapped, true); break;
        case SDL_EVENT_WINDOW_SHOWN:     w.set_hidden(bus, HostWindow::HiddenUnmapped, false); break;
        case SDL_EVENT_WINDOW_OCCLUDED:  w.set_hidden(bus, HostWindow::HiddenOccluded, true); break;
        case SDL_EVENT_WINDOW_EXPOSED:   w.set_hidden(bus, HostWindow::HiddenOccluded, false); break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            w.focused_ = true;
            bus.emit(FocusChanged{true});
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            w.focused_ = false;
            input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{false});
            break;

        case SDL_EVENT_TEXT_INPUT:
            input_typed(bus, input, e.text.text);
            break;

        case SDL_EVENT_TEXT_EDITING: {
            // An input method composing: its text and where its caret is, in bytes.
            TextComposing tc{};
            const char* src = e.edit.text ? e.edit.text : "";
            usize sl = strlen(src);
            usize n = utf8_prefix(src, sl < sizeof tc.text - 1 ? sl : sizeof tc.text - 1);
            memcpy(tc.text, src, n);
            // SDL counts in code points: turn them into bytes of what was kept.
            auto bytes_at = [&](i32 cps) { const char* p = tc.text; for (i32 k = 0; k < cps && *p; ++k) utf8_next(p); return (u32)(p - tc.text); };
            tc.caret = bytes_at(e.edit.start < 0 ? 0 : e.edit.start);
            tc.selection = e.edit.length > 0 ? bytes_at(e.edit.start + e.edit.length) - tc.caret : 0;
            bus.emit(tc);
            break;
        }

        case SDL_EVENT_KEY_DOWN: {
            Key k = key_from_scancode((u32)e.key.scancode);
            if (k == Key::Backspace) ++input.raw.backspaces; // repeats too: holding it erases
            if (k == Key::Unknown) break;
            input.raw.set(k, true);
            bus.emit(KeyDown{k, 0, e.key.repeat});
            break;
        }

        case SDL_EVENT_KEY_UP: {
            Key k = key_from_scancode((u32)e.key.scancode);
            if (k == Key::Unknown) break;
            input.raw.set(k, false);
            bus.emit(KeyUp{k, 0});
            break;
        }

        // SDL reports the pointer in window units; the engine works in framebuffer pixels.
        case SDL_EVENT_MOUSE_MOTION: {
            f32 s = w.pixel_scale();
            MouseState& m = input.raw.mouse;
            f32 nx = e.motion.x * s, ny = e.motion.y * s;
            bus.emit(MouseMoved{nx, ny, e.motion.xrel * s, e.motion.yrel * s});
            m.x = nx; m.y = ny;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            MouseButton b;
            switch (e.button.button) {
            case SDL_BUTTON_LEFT:   b = MouseButton::Left; break;
            case SDL_BUTTON_MIDDLE: b = MouseButton::Middle; break;
            case SDL_BUTTON_RIGHT:  b = MouseButton::Right; break;
            case SDL_BUTTON_X1:     b = MouseButton::X1; break;
            case SDL_BUTTON_X2:     b = MouseButton::X2; break;
            default: goto done;
            }
            {
                f32 s = w.pixel_scale();
                MouseState& m = input.raw.mouse;
                m.x = e.button.x * s; m.y = e.button.y * s;
                bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                m.set(b, down);
                if (down) bus.emit(MouseButtonDown{b, m.x, m.y});
                else      bus.emit(MouseButtonUp{b, m.x, m.y});
            }
        done:
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            input.raw.mouse.wheel += e.wheel.y;
            bus.emit(MouseWheel{e.wheel.x, e.wheel.y});
            break;
        }

        default:
            input_gamepad_event(&e, bus, input); // pads arrive on the same queue
            break;
        }
}

} // namespace

void HostWindow::pump(EventBus& bus, Input& input) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) translate(*this, e, bus, input);
}

// SDL_PeepEvents takes from SDL's queue without pumping the OS for more.
void HostWindow::drain(EventBus& bus, Input& input) {
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) translate(*this, e, bus, input);
}

void HostWindow::on_modal(void (*fn)(void* ctx), void* ctx) {
    modal_fn_  = fn;
    modal_ctx_ = ctx;
}

bool hid_backend_holds_unfocused() { return false; }
bool HostWindow::touch_screen() const { return false; }

int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx) {
    return run(argc, argv, ctx);
}

} // namespace kv
