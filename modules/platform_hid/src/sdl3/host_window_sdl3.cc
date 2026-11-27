#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/log.h"
#include "korvid/core/atomic.h"
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

// platform_wake's event (KR26): pushed to end SDL_WaitEventTimeout, and taken by the pump.
u32 g_wake_event = 0;

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
    g_wake_event = SDL_RegisterEvents(1);
    if (!g_wake_event) KV_WARN("hid", "no SDL event for a wake: a wait ends only by the window or the time");
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

namespace {

// Whether what a transparent window draws clear shows what is behind it. Wayland and macOS
// composite every window. X11 composites when a compositing manager holds the screen's
// _NET_WM_CM_Sn selection; without one an ARGB window shows its clear parts black. Xlib is
// SDL's, opened by name as SDL opens it, so the engine links no X library of its own.
bool sdl_composited(SDL_Window* w, bool x11_alpha) {
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_TRANSPARENT)) return false;
    if (strcmp(SDL_GetCurrentVideoDriver(), "x11") != 0) return true;
#if defined(__APPLE__)
    return true;
#else
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    void* dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    i64 screen = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_SCREEN_NUMBER, 0);
    static SDL_SharedObject* x11 = SDL_LoadObject("libX11.so.6");
    if (!dpy || !x11) return true; // cannot ask: take SDL's word, an ARGB visual
    using InternAtom = unsigned long (*)(void*, const char*, int);
    using SelectionOwner = unsigned long (*)(void*, unsigned long);
    auto intern = (InternAtom)SDL_LoadFunction(x11, "XInternAtom");
    auto owner  = (SelectionOwner)SDL_LoadFunction(x11, "XGetSelectionOwner");
    if (!intern || !owner) return true;
    char name[32];
    SDL_snprintf(name, sizeof name, "_NET_WM_CM_S%d", (int)screen);
    // And the window has an alpha visual: SDL gives one to a window it draws (its software
    // surface, GL), not to a Vulkan window, unless asked (x11_alpha_visual), and does not
    // say which it gave.
    return x11_alpha && owner(dpy, intern(dpy, name, 0)) != 0;
#endif
}

// A 32-bit TrueColor visual's id on X11, for SDL_HINT_VIDEO_X11_WINDOW_VISUALID, so a
// transparent Vulkan window has alpha to present: from a connection of its own, since
// SDL's display is not to be had before a window. 0 where there is none or no Xlib.
unsigned long x11_alpha_visual() {
#if defined(__APPLE__)
    return 0;
#else
    static SDL_SharedObject* x11 = SDL_LoadObject("libX11.so.6");
    if (!x11) return 0;
    struct VisualInfo { void* visual; unsigned long visualid; int screen, depth, c_class; unsigned long masks[3]; int colormap_size, bits_per_rgb; };
    using Open = void* (*)(const char*);
    using Close = int (*)(void*);
    using Screen = int (*)(void*);
    using Match = int (*)(void*, int, int, int, VisualInfo*);
    auto open_d  = (Open)SDL_LoadFunction(x11, "XOpenDisplay");
    auto close_d = (Close)SDL_LoadFunction(x11, "XCloseDisplay");
    auto screen  = (Screen)SDL_LoadFunction(x11, "XDefaultScreen");
    auto match   = (Match)SDL_LoadFunction(x11, "XMatchVisualInfo");
    if (!open_d || !close_d || !screen || !match) return 0;
    void* dpy = open_d(nullptr);
    if (!dpy) return 0;
    VisualInfo vi{};
    constexpr int kTrueColor = 4; // X.h's
    unsigned long id = match(dpy, screen(dpy), 32, kTrueColor, &vi) ? vi.visualid : 0;
    close_d(dpy);
    return id;
#endif
}

// X11 with no compositor: SDL gives a transparent window's shape as its input shape alone,
// and the clear parts show black. The same shape as the window's bounding shape cuts them
// out, one bit a pixel (`shape` null: the whole rect again). Through SDL's Xlib and Xext,
// opened by name. Nothing on another driver, or with a compositor.
void sdl_x11_bounding(SDL_Window* w, const SDL_Surface* shape) {
#if defined(__APPLE__)
    (void)w; (void)shape;
#else
    if (strcmp(SDL_GetCurrentVideoDriver(), "x11") != 0) return;
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    void* dpy = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    unsigned long win = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    static SDL_SharedObject* x11 = SDL_LoadObject("libX11.so.6");
    static SDL_SharedObject* xext = SDL_LoadObject("libXext.so.6");
    if (!dpy || !win || !x11 || !xext) {
        static bool told = false;
        if (!told) KV_INFO("window", "no Xext to cut the window's shape out: its clear parts show black");
        told = true;
        return;
    }
    using CreateBitmap = unsigned long (*)(void*, unsigned long, const char*, unsigned, unsigned);
    using FreePixmap = int (*)(void*, unsigned long);
    using CombineMask = void (*)(void*, unsigned long, int, int, int, unsigned long, int);
    auto create  = (CreateBitmap)SDL_LoadFunction(x11, "XCreateBitmapFromData");
    auto release = (FreePixmap)SDL_LoadFunction(x11, "XFreePixmap");
    auto combine = (CombineMask)SDL_LoadFunction(xext, "XShapeCombineMask");
    if (!create || !release || !combine) return;
    constexpr int kShapeBounding = 0, kShapeSet = 0; // X11/extensions/shape.h's
    if (!shape) { combine(dpy, win, kShapeBounding, 0, 0, 0, kShapeSet); return; }
    // A bitmap's rows are whole bytes, least significant bit first.
    u32 stride = ((u32)shape->w + 7) / 8;
    Array<u8> bits;
    u8* b = bits.push_n(stride * (u32)shape->h);
    memset(b, 0, (usize)stride * (u32)shape->h);
    for (int y = 0; y < shape->h; ++y) {
        const u32* row = (const u32*)((const u8*)shape->pixels + (usize)y * (usize)shape->pitch);
        for (int x = 0; x < shape->w; ++x)
            if (row[x] >> 24) b[(usize)y * stride + (u32)x / 8] |= (u8)(1u << (x & 7));
    }
    unsigned long pm = create(dpy, win, (const char*)b, (unsigned)shape->w, (unsigned)shape->h);
    bits.free();
    if (!pm) return;
    combine(dpy, win, kShapeBounding, 0, 0, pm, kShapeSet);
    release(dpy, pm);
#endif
}

// Wayland: SDL keeps a window's shape for its own renderer's alpha and gives the compositor
// no input region, so a click on a clear part still lands on the window. The engine sets
// the surface's input region itself, from the shape's runs (window units, as the surface's
// coordinates are), through SDL's libwayland-client opened by name, and the next present
// commits it. A compositor of its own is bound once on SDL's display, on a queue of its
// own, so nothing of SDL's is dispatched here. `shape` null: input everywhere again.
// The protocol's numbers are wayland.xml's; the calls are libwayland-client's ABI, written
// out so no Wayland header is needed.
struct WlLib {
    void* (*marshal)(void*, u32, const void*, u32, u32, ...) = nullptr; // wl_proxy_marshal_flags
    u32   (*version)(void*) = nullptr;                                   // wl_proxy_get_version
    void* (*wrapper)(void*) = nullptr;                                   // wl_proxy_create_wrapper
    void  (*wrapper_destroy)(void*) = nullptr;
    void  (*set_queue)(void*, void*) = nullptr;
    int   (*add_listener)(void*, void (**)(void), void*) = nullptr;
    void* (*create_queue)(void*) = nullptr;                              // wl_display_create_queue
    int   (*roundtrip_queue)(void*, void*) = nullptr;
    const void* registry_iface = nullptr;   // wl_registry_interface
    const void* compositor_iface = nullptr; // wl_compositor_interface
    const void* region_iface = nullptr;     // wl_region_interface
    void* queue = nullptr;
    void* compositor = nullptr;
    bool  tried = false;
};
WlLib g_wl;

void wl_global(void* data, void* registry, u32 name, const char* iface, u32 version) {
    WlLib& l = *(WlLib*)data;
    if (l.compositor || strcmp(iface, "wl_compositor") != 0) return;
    u32 v = version < 4 ? version : 4;
    // wl_registry.bind (opcode 0), new_id untyped: name, the interface's name, version, the id.
    l.compositor = l.marshal(registry, 0, l.compositor_iface, v, 0, name, "wl_compositor", v, nullptr);
}
void wl_global_remove(void*, void*, u32) {}

void* wl_compositor_for(void* display) {
    WlLib& l = g_wl;
    if (l.tried) return l.compositor;
    l.tried = true;
    SDL_SharedObject* so = SDL_LoadObject("libwayland-client.so.0");
    if (!so) return nullptr;
    l.marshal         = (decltype(l.marshal))SDL_LoadFunction(so, "wl_proxy_marshal_flags");
    l.version         = (decltype(l.version))SDL_LoadFunction(so, "wl_proxy_get_version");
    l.wrapper         = (decltype(l.wrapper))SDL_LoadFunction(so, "wl_proxy_create_wrapper");
    l.wrapper_destroy = (decltype(l.wrapper_destroy))SDL_LoadFunction(so, "wl_proxy_wrapper_destroy");
    l.set_queue       = (decltype(l.set_queue))SDL_LoadFunction(so, "wl_proxy_set_queue");
    l.add_listener    = (decltype(l.add_listener))SDL_LoadFunction(so, "wl_proxy_add_listener");
    l.create_queue    = (decltype(l.create_queue))SDL_LoadFunction(so, "wl_display_create_queue");
    l.roundtrip_queue = (decltype(l.roundtrip_queue))SDL_LoadFunction(so, "wl_display_roundtrip_queue");
    l.registry_iface  = (const void*)SDL_LoadFunction(so, "wl_registry_interface");
    l.compositor_iface = (const void*)SDL_LoadFunction(so, "wl_compositor_interface");
    l.region_iface    = (const void*)SDL_LoadFunction(so, "wl_region_interface");
    if (!l.marshal || !l.version || !l.wrapper || !l.wrapper_destroy || !l.set_queue || !l.add_listener || !l.create_queue ||
        !l.roundtrip_queue || !l.registry_iface || !l.compositor_iface || !l.region_iface) return nullptr;
    l.queue = l.create_queue(display);
    void* d = l.wrapper(display);
    if (!l.queue || !d) return nullptr;
    l.set_queue(d, l.queue);
    void* registry = l.marshal(d, 1, l.registry_iface, l.version(d), 0, nullptr); // wl_display.get_registry
    l.wrapper_destroy(d);
    if (!registry) return nullptr;
    static void (*listener[2])(void) = {(void (*)(void))wl_global, (void (*)(void))wl_global_remove};
    l.add_listener(registry, listener, &l);
    l.roundtrip_queue(display, l.queue);
    // The registry stays: destroying it is a request a compositor of this age may refuse.
    return l.compositor;
}

void sdl_wayland_input(SDL_Window* w, const SDL_Surface* shape) {
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") != 0) return;
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    void* surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    void* compositor = display && surface ? wl_compositor_for(display) : nullptr;
    if (!compositor) {
        static bool told = false;
        if (!told) KV_WARN("window", "no Wayland compositor to give an input region to: clicks on the clear parts reach the window");
        told = true;
        return;
    }
    WlLib& l = g_wl;
    constexpr u32 kSetInputRegion = 5, kCreateRegion = 1, kRegionAdd = 1, kRegionDestroy = 0, kMarshalDestroy = 1;
    if (!shape) { l.marshal(surface, kSetInputRegion, nullptr, l.version(surface), 0, nullptr); return; }
    void* region = l.marshal(compositor, kCreateRegion, l.region_iface, l.version(compositor), 0, nullptr);
    if (!region) return;
    // Each row's runs of input, and a run as tall as the rows below it that have the same
    // runs: a disc is a few hundred rects.
    auto in = [&](int x, int y) { return (((const u32*)((const u8*)shape->pixels + (usize)y * (usize)shape->pitch))[x] >> 24) != 0; };
    auto same_row = [&](int a, int b) {
        for (int x = 0; x < shape->w; ++x) if (in(x, a) != in(x, b)) return false;
        return true;
    };
    for (int y = 0; y < shape->h;) {
        int y1 = y + 1;
        while (y1 < shape->h && same_row(y, y1)) ++y1;
        for (int x = 0; x < shape->w;) {
            if (!in(x, y)) { ++x; continue; }
            int x1 = x + 1;
            while (x1 < shape->w && in(x1, y)) ++x1;
            l.marshal(region, kRegionAdd, nullptr, l.version(region), 0, x, y, x1 - x, y1 - y);
            x = x1;
        }
        y = y1;
    }
    l.marshal(surface, kSetInputRegion, nullptr, l.version(surface), 0, region);
    l.marshal(region, kRegionDestroy, nullptr, l.version(region), kMarshalDestroy);
}

// SDL's hit test asks in window units; the grips are framebuffer pixels.
SDL_HitTestResult sdl_hit_test(SDL_Window*, const SDL_Point* p, void* data) {
    const HostWindow& w = *(const HostWindow*)data;
    f32 k = w.pixel_scale();
    switch (w.grip_at((f32)p->x * k, (f32)p->y * k)) {
    case WindowGrip::Move: return SDL_HITTEST_DRAGGABLE;
    case WindowGrip::N:    return SDL_HITTEST_RESIZE_TOP;
    case WindowGrip::NE:   return SDL_HITTEST_RESIZE_TOPRIGHT;
    case WindowGrip::E:    return SDL_HITTEST_RESIZE_RIGHT;
    case WindowGrip::SE:   return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    case WindowGrip::S:    return SDL_HITTEST_RESIZE_BOTTOM;
    case WindowGrip::SW:   return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    case WindowGrip::W:    return SDL_HITTEST_RESIZE_LEFT;
    case WindowGrip::NW:   return SDL_HITTEST_RESIZE_TOPLEFT;
    case WindowGrip::None: break;
    }
    return SDL_HITTEST_NORMAL;
}

} // namespace

// The mask scaled to the window's size in window units (what SDL's shape is in), nearest
// neighbour, and cut at a half: SDL's backends each read its alpha with their own threshold
// (X11's shape is one bit), so it gets 0 or 255 and nothing between.
bool hid_window_shape(HostWindow& hw) {
    SDL_Window* w = (SDL_Window*)hw.native_;
    if (!w) return false;
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_TRANSPARENT)) {
        static bool told = false;
        if (!told) KV_WARN("window", "a shape on a window not opened transparent (WindowDesc::transparent): it stays a rectangle");
        told = true;
        return false;
    }
    if (!hw.shape_.len) {
        if (!hw.transparent_) sdl_x11_bounding(w, nullptr);
        sdl_wayland_input(w, nullptr);
        return SDL_SetWindowShape(w, nullptr);
    }
    int ww = 0, wh = 0;
    if (!SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return false;
    SDL_Surface* s = SDL_CreateSurface(ww, wh, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    for (int y = 0; y < wh; ++y) {
        u32* row = (u32*)((u8*)s->pixels + (usize)y * (usize)s->pitch);
        const u8* src = hw.shape_.data + (usize)((u64)y * hw.shape_h_ / (u32)wh) * hw.shape_w_;
        for (int x = 0; x < ww; ++x) row[x] = src[(u64)x * hw.shape_w_ / (u32)ww] >= 128 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    }
    bool ok = SDL_SetWindowShape(w, s);
    if (ok && !hw.transparent_) sdl_x11_bounding(w, s);
    if (ok) sdl_wayland_input(w, s);
    SDL_DestroySurface(s);
    if (!ok) {
        static bool told = false;
        if (!told) KV_WARN("window", "SDL_SetWindowShape: %s", SDL_GetError());
        told = true;
    }
    return ok;
}

bool hid_window_grips(HostWindow& hw) {
    SDL_Window* w = (SDL_Window*)hw.native_;
    if (!w) return false;
    if (!SDL_SetWindowHitTest(w, hw.grip_count_ ? sdl_hit_test : nullptr, &hw)) {
        static bool told = false;
        if (!told) KV_WARN("window", "SDL_SetWindowHitTest: %s", SDL_GetError());
        told = true;
        return false;
    }
    return true;
}

// SDL's sizes are window units: a minimum rounds up, a maximum down.
void hid_window_bounds(HostWindow& hw) {
    SDL_Window* w = (SDL_Window*)hw.native_;
    if (!w) return;
    f32 k = hw.pixel_scale();
    hw.bounds_scale_ = k;
    auto up   = [&](u32 px) { return px ? (int)((f32)px / k + 0.999f) : 0; };
    auto down = [&](u32 px) { return px ? (int)((f32)px / k) : 0; };
    SDL_SetWindowMinimumSize(w, up(hw.bounds_[0]), up(hw.bounds_[1]));
    SDL_SetWindowMaximumSize(w, down(hw.bounds_[2]), down(hw.bounds_[3]));
}

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) return false; // the module starts headless
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (desc.surface == WindowSurface::Vulkan) flags |= SDL_WINDOW_VULKAN; // also loads the Vulkan loader
    if (desc.surface == WindowSurface::Metal) flags |= SDL_WINDOW_METAL;   // SDL_Metal_CreateView needs it
    if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS;
    if (desc.transparent) flags |= SDL_WINDOW_TRANSPARENT | SDL_WINDOW_BORDERLESS;
#if defined(__APPLE__)
    // Retina: without this the backing store is in window points and the system scales it
    // up, so a 2x display shows a quarter of the pixels. With it the framebuffer is in
    // pixels, which is what width() and height() promise, and pointers scale by
    // pixel_scale(). (Linux at a Wayland scale renders at window size today; the flag
    // would change that and every measurement made there, so it is not set there yet.)
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    bool x11_visual = false;
    if (desc.transparent && strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        if (unsigned long id = x11_alpha_visual()) {
            char hex[24];
            SDL_snprintf(hex, sizeof hex, "0x%lx", id);
            x11_visual = SDL_SetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID, hex);
        }
    }
    SDL_Window* w = SDL_CreateWindow(desc.title, (int)desc.width, (int)desc.height, flags);
    if (x11_visual) SDL_ResetHint(SDL_HINT_VIDEO_X11_WINDOW_VISUALID);
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
    transparent_ = desc.transparent && sdl_composited(w, x11_visual);
#if KV_LIVE_RESIZE_TIMER
    modal_timer_ = modal_timer_start(this);
#endif
    KV_INFO("window", "opened '%s' %ux%u px%s", desc.title, width_, height_,
            !desc.transparent ? "" : transparent_ ? ", transparent" : ", opaque: nothing composites its alpha; a shape cuts it out");
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
    hid_window_parts_reset(*this);
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
    case SDL_PIXELFORMAT_XRGB8888: case SDL_PIXELFORMAT_ARGB8888: rgba = false; break; // B, G, R, X (or A) in memory
    case SDL_PIXELFORMAT_XBGR8888: case SDL_PIXELFORMAT_ABGR8888: rgba = true; break;  // R, G, B, X (or A)
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
    *out = SoftwareFrame{(u8*)s->pixels, (u32)s->w, (u32)s->h, (u32)s->pitch, rgba, false, transparent_ && SDL_ISPIXELFORMAT_ALPHA(s->format)};
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

// The window an event is for: the open HostWindow whose SDL window has the event's window
// id. The main one for an event that names none (a quit, a pad, the wake); null for one
// whose window is already closed.
HostWindow* event_window(HostWindow& main, const SDL_Event& e) {
    SDL_WindowID id = 0;
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST) id = e.window.windowID;
    else switch (e.type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: id = e.key.windowID; break;
    case SDL_EVENT_TEXT_INPUT:    id = e.text.windowID; break;
    case SDL_EVENT_TEXT_EDITING:  id = e.edit.windowID; break;
    case SDL_EVENT_MOUSE_MOTION:  id = e.motion.windowID; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: id = e.button.windowID; break;
    case SDL_EVENT_MOUSE_WHEEL:   id = e.wheel.windowID; break;
    default: break;
    }
    if (!id) return &main;
    SDL_Window* sw = SDL_GetWindowFromID(id);
    for (u32 i = 0; sw && i < KV_WINDOWS; ++i)
        if (HostWindow* h = hid_window(i); h && h->native_ == sw) return h;
    return nullptr;
}

// One OS event into bus events and raw input: pump's and drain's, always on the main
// window, whose queue is every window's.
void translate(HostWindow& main, const SDL_Event& e, EventBus& bus, Input& input) {
        HostWindow* to = event_window(main, e);
        if (!to) return;
        HostWindow& w = *to;
        u8 id = w.id_;
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
        if (g_wake_event && e.type == g_wake_event) { atomic_store(&main.wake_queued_, 0u, MO_RELEASE); return; }
        // How long a press waited for the pump, from SDL's stamp (the OS's, on its clock).
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            u64 now = SDL_GetTicksNS();
            main.input_delay_ = e.common.timestamp && e.common.timestamp <= now ? (f64)(now - e.common.timestamp) * 1e-9 : 0.0;
        }
        switch (e.type) {
        case SDL_EVENT_QUIT:
            bus.emit(CloseRequested{0});
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            bus.emit(CloseRequested{id});
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            u32 nw = (u32)e.window.data1, nh = (u32)e.window.data2;
            if (nw != w.width_ || nh != w.height_) {
                w.width_  = nw;
                w.height_ = nh;
                if (w.shape_.len) hid_window_shape(w); // the mask, scaled to the new size
                // Bounds are pixels, SDL's are window units: a new scale changes them.
                if ((w.bounds_[0] | w.bounds_[1] | w.bounds_[2] | w.bounds_[3]) && w.pixel_scale() != w.bounds_scale_) hid_window_bounds(w);
                bus.emit(WindowResized{nw, nh, id});
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
            bus.emit(FocusChanged{true, id});
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            w.focused_ = false;
            input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{false, id});
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
            bool into = m.window != id; // into another window: its pixels, so no travel
            bus.emit(MouseMoved{nx, ny, into ? 0.0f : e.motion.xrel * s, into ? 0.0f : e.motion.yrel * s, id});
            m.window = id;
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
                m.window = id;
                m.x = e.button.x * s; m.y = e.button.y * s;
                bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                m.set(b, down);
                if (down) bus.emit(MouseButtonDown{b, m.x, m.y, id});
                else      bus.emit(MouseButtonUp{b, m.x, m.y, id});
            }
        done:
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            input.raw.mouse.wheel += e.wheel.y;
            bus.emit(MouseWheel{e.wheel.x, e.wheel.y, id});
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

// SDL's wait with no event to fill leaves what came queued for the pump. It takes whole
// milliseconds: rounded up, so a wait does not end just short of the time it was given.
bool HostWindow::wait(f64 seconds) {
    if (seconds <= 0.0) return false;
    f64 ms = seconds * 1000.0 + 0.999;
    return SDL_WaitEventTimeout(nullptr, ms >= 2.0e9 ? 2000000000 : (Sint32)ms);
}

// One wake queued at most: the next pump takes it, and a wake after that queues another.
void HostWindow::wake() {
    if (!g_wake_event || atomic_exchange(&wake_queued_, 1u, MO_ACQ_REL)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_wake_event;
    if (!SDL_PushEvent(&e)) atomic_store(&wake_queued_, 0u, MO_RELEASE);
}

void HostWindow::on_modal(void (*fn)(void* ctx), void* ctx) {
    modal_fn_  = fn;
    modal_ctx_ = ctx;
}

bool hid_backend_holds_unfocused() { return false; }
u32  hid_backend_windows() { return KV_WINDOWS; }

// SDL's hide and show; SDL then says WINDOW_HIDDEN and _SHOWN, which mark it unmapped too.
void HostWindow::show(bool on) {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w) return;
    if (on) SDL_ShowWindow(w); else SDL_HideWindow(w);
    hidden_ = on ? (u8)(hidden_ & ~HiddenByProgram) : (u8)(hidden_ | HiddenByProgram);
}
bool HostWindow::touch_screen() const { return false; }

int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx) {
    return run(argc, argv, ctx);
}

} // namespace kv
