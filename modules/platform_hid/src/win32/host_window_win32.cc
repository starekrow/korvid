// The host window on Win32, on a thread of its own. Windows runs a modal loop of its own
// while the user drags or resizes a window, and the thread that owns the window is stuck
// in it until the mouse comes up. Here that is the window's thread, which only turns
// messages into a queue; the engine's thread drains the queue in pump() and keeps ticking
// and presenting through the drag. Per-monitor DPI aware: sizes in WindowDesc are logical
// (scaled by the display's DPI), the framebuffer and the pointer are in physical pixels,
// and a move to a display of another DPI resizes the window as Windows suggests. Alt and
// F10 do not open the window menu (another modal loop); Alt+F4 still asks to close.
// Gamepads come from SDL3 alone (src/sdl3_pads/): its gamepad subsystem, polled here.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/utf8.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/array.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "../hid_backend.h"
#include "../sdl3_pads/sdl_pads.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace kv {

namespace {

enum class WinMsgType : u8 { Close, Size, Focus, Key, Move, Button, Wheel, Char };

// What the window thread saw, in the order it saw it.
struct WinMsg {
    WinMsgType type;
    bool       on;      // Focus: focused; Key, Button: down
    bool       repeat;  // Key: auto-repeat
    u8         button;  // Button: a MouseButton
    u32        a, b;    // Size: width, height in pixels; Key: scancode; Char: a code point
    f32        x, y;    // Move, Button: position in pixels; Wheel: dx, dy in notches
};

constexpr UINT WM_KV_DESTROY = WM_APP + 1; // close() asks the window thread to destroy it

struct Win32Window {
    HWND          hwnd = nullptr;
    OsThread      thread;
    OsMutex       mu;               // guards queue, ready and failed
    OsCond        cv;               // open() waits on it for the window to exist
    bool          ready  = false;
    bool          failed = false;
    Array<WinMsg> queue;            // the window thread appends
    Array<WinMsg> taken;            // pump's side of the swap, the engine thread's alone
    WindowDesc    desc;
    wchar_t       title[256] = {};
    u32           width = 0, height = 0; // the client size at creation, before ready
    u32           dpi = 96;              // written by the window thread, read atomically
    u8            buttons = 0;           // the window thread's: mouse buttons held, for capture
};

Win32Window g_win; // one window per process

void post(const WinMsg& m) {
    os_mutex_lock(&g_win.mu);
    g_win.queue.push(m);
    os_mutex_unlock(&g_win.mu);
}

void post_button(HWND h, MouseButton b, bool down, LPARAM lp) {
    // The window keeps the mouse while a button is held, so a drag that leaves the window
    // still reports its moves and its release.
    u8 bit = (u8)(1u << (u32)b);
    if (down) { if (!g_win.buttons) SetCapture(h); g_win.buttons |= bit; }
    else { g_win.buttons &= (u8)~bit; if (!g_win.buttons) ReleaseCapture(); }
    WinMsg m{};
    m.type = WinMsgType::Button;
    m.on = down;
    m.button = (u8)b;
    m.x = (f32)GET_X_LPARAM(lp);
    m.y = (f32)GET_Y_LPARAM(lp);
    post(m);
}

// Set 1 with 0x100 for E0-prefixed keys, from a key message; asked of the layout when the
// message carries none (some virtual keys arrive without a scancode).
u32 scancode_of(WPARAM wp, LPARAM lp) {
    u32 sc = (u32)((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0x100u : 0u);
    if ((sc & 0xFF) == 0) {
        UINT x = MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_VSC_EX);
        sc = (x & 0xFF) | ((x & 0xFF00) == 0xE000 ? 0x100u : 0u);
    }
    return sc;
}

LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: { WinMsg m{}; m.type = WinMsgType::Close; post(m); return 0; } // the engine decides
    case WM_KV_DESTROY: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0; // a minimized window keeps its last size
        WinMsg m{};
        m.type = WinMsgType::Size;
        m.a = LOWORD(lp);
        m.b = HIWORD(lp);
        post(m);
        return 0;
    }
    case WM_DPICHANGED: {
        atomic_store(&g_win.dpi, (u32)HIWORD(wp), MO_RELEASE);
        const RECT* r = (const RECT*)lp; // the size Windows suggests at the new DPI
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        if (msg == WM_KILLFOCUS && g_win.buttons) { g_win.buttons = 0; ReleaseCapture(); }
        WinMsg m{};
        m.type = WinMsgType::Focus;
        m.on = msg == WM_SETFOCUS;
        post(m);
        return 0;
    }

    case WM_SYSKEYDOWN:
        if (wp == VK_F4) break; // Alt+F4: the system turns it into WM_CLOSE
        [[fallthrough]];
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        WinMsg m{};
        m.type = WinMsgType::Key;
        m.on = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        m.repeat = m.on && (lp & (1 << 30)) != 0;
        m.a = scancode_of(wp, lp);
        post(m);
        return 0; // handled: Alt and F10 open no menu
    }
    case WM_CHAR: {
        // UTF-16, a surrogate pair in two messages; control characters are keys, not text.
        static u32 high = 0;
        u32 u = (u32)wp;
        if (u >= 0xD800 && u < 0xDC00) { high = u; return 0; }
        if (u >= 0xDC00 && u < 0xE000) { u = high ? 0x10000 + ((high - 0xD800) << 10) + (u - 0xDC00) : 0xFFFD; high = 0; }
        if (u < 0x20 || u == 0x7F) return 0;
        WinMsg m{};
        m.type = WinMsgType::Char;
        m.a = u;
        post(m);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;

    case WM_MOUSEMOVE: {
        WinMsg m{};
        m.type = WinMsgType::Move;
        m.x = (f32)GET_X_LPARAM(lp);
        m.y = (f32)GET_Y_LPARAM(lp);
        post(m);
        return 0;
    }
    case WM_LBUTTONDOWN: post_button(h, MouseButton::Left, true, lp); return 0;
    case WM_LBUTTONUP:   post_button(h, MouseButton::Left, false, lp); return 0;
    case WM_MBUTTONDOWN: post_button(h, MouseButton::Middle, true, lp); return 0;
    case WM_MBUTTONUP:   post_button(h, MouseButton::Middle, false, lp); return 0;
    case WM_RBUTTONDOWN: post_button(h, MouseButton::Right, true, lp); return 0;
    case WM_RBUTTONUP:   post_button(h, MouseButton::Right, false, lp); return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        post_button(h, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2, msg == WM_XBUTTONDOWN, lp);
        return TRUE;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        WinMsg m{};
        m.type = WinMsgType::Wheel;
        f32 notches = (f32)GET_WHEEL_DELTA_WPARAM(wp) / (f32)WHEEL_DELTA;
        if (msg == WM_MOUSEWHEEL) m.y = notches; else m.x = notches;
        post(m);
        return 0;
    }

    // A renderer's window (Vulkan or D3D12) is the renderer's to paint; erasing it would
    // flash the background.
    case WM_ERASEBKGND:
        if (g_win.desc.surface != WindowSurface::Software) return 1;
        break;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The window's size for a client area of the desc's logical size at a DPI.
SIZE outer_size(DWORD style, UINT dpi) {
    RECT r = {0, 0, MulDiv((int)g_win.desc.width, (int)dpi, 96), MulDiv((int)g_win.desc.height, (int)dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

// The window thread: creates the window, says so, then runs its messages until close().
void window_main(void*) {
    Win32Window& w = g_win;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof wc;
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = window_proc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 28)); // what a software window shows
        wc.lpszClassName = L"korvid";
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    DWORD style = w.desc.borderless ? (WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX) : WS_OVERLAPPEDWINDOW;
    UINT dpi = GetDpiForSystem();
    SIZE size = outer_size(style, dpi);
    HWND h = registered ? CreateWindowExW(0, L"korvid", w.title, style, CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy,
                                          nullptr, nullptr, inst, nullptr)
                        : nullptr;
    DWORD err = h ? 0 : GetLastError();
    if (h) {
        // The display it landed on may differ from the system's DPI: size it for its own.
        UINT here = GetDpiForWindow(h);
        if (here && here != dpi) {
            size = outer_size(style, here);
            SetWindowPos(h, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            dpi = here;
        }
        atomic_store(&w.dpi, (u32)dpi, MO_RELEASE);
        RECT c;
        GetClientRect(h, &c);
        w.width  = (u32)c.right;
        w.height = (u32)c.bottom;
    }
    os_mutex_lock(&w.mu);
    w.hwnd   = h;
    w.failed = !h;
    w.ready  = true;
    os_cond_broadcast(&w.cv);
    os_mutex_unlock(&w.mu);
    if (!h) {
        KV_ERROR("window", "CreateWindowExW failed (error %lu)", (unsigned long)err);
        mem_thread_exit();
        return;
    }
    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    mem_thread_exit();
}

} // namespace

bool hid_backend_startup() {
    KV_INFO("hid", "Win32 windows and input, SDL3 gamepads; headless until a window opens");
    return true;
}

void hid_backend_shutdown() { hid_video_down(); }

// Per-monitor DPI awareness, for the whole process, before the first window. Fails
// harmlessly if a manifest already chose.
bool hid_video_up() {
    static bool up = false;
    if (!up) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        sdl_pads_up(); // without pads the window still works
        up = true;
    }
    return true;
}

void hid_video_down() { sdl_pads_down(); }

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) return false;
    Win32Window& w = g_win;
    w.desc = desc;
    w.desc.title = nullptr; // the wide copy below is what the window thread reads
    const char* title = desc.title ? desc.title : "korvid";
    if (!MultiByteToWideChar(CP_UTF8, 0, title, -1, w.title, (int)KV_COUNTOF(w.title))) {
        w.title[KV_COUNTOF(w.title) - 1] = 0; // too long: what fitted
    }
    w.hwnd = nullptr;
    w.ready = w.failed = false;
    w.buttons = 0;
    w.width = w.height = 0;
    w.queue.clear();
    w.taken.clear();
    os_mutex_init(&w.mu);
    os_cond_init(&w.cv);
    w.thread = OsThread();
    if (!os_thread_start(&w.thread, window_main, nullptr)) {
        KV_ERROR("window", "could not start the window thread");
        os_cond_destroy(&w.cv);
        os_mutex_destroy(&w.mu);
        return false;
    }
    os_mutex_lock(&w.mu);
    while (!w.ready) os_cond_wait(&w.cv, &w.mu);
    bool ok = !w.failed;
    os_mutex_unlock(&w.mu);
    if (!ok) {
        os_thread_join(&w.thread);
        os_cond_destroy(&w.cv);
        os_mutex_destroy(&w.mu);
        return false;
    }
    native_  = w.hwnd;
    surface_ = desc.surface;
    vsync_   = desc.vsync;
    width_   = w.width;
    height_  = w.height;
    focused_ = false; // the first pump reports it
    KV_INFO("window", "opened '%s' %ux%u px at %u dpi", title, width_, height_, atomic_load(&w.dpi, MO_ACQUIRE));
    return true;
}

// Windows sends WM_CHAR whenever the window has the keyboard; nothing to switch.
void HostWindow::text_input(bool) {}

// Not yet on this host (Krome slice 3): the input method's area is not told, the clipboard is
// the platform module's own (inside this process only), the cursor keeps its shape.
void HostWindow::text_area(rect2, f32) {}
bool HostWindow::clipboard_get(Array<char>&) { return false; }
bool HostWindow::clipboard_set(const char*) { return false; }
void HostWindow::cursor(CursorShape) {}


void HostWindow::close() {
    if (!native_) return;
    Win32Window& w = g_win;
    PostMessageW(w.hwnd, WM_KV_DESTROY, 0, 0);
    os_thread_join(&w.thread);
    os_cond_destroy(&w.cv);
    os_mutex_destroy(&w.mu);
    w.queue.free();
    w.taken.free();
    w.hwnd = nullptr;
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    KV_INFO("window", "closed");
}

// The window class's brush paints a software window; nothing to do here.
void HostWindow::present_blank() {}

// Physical pixels per logical unit: the window's DPI over 96.
f32 HostWindow::pixel_scale() const {
    if (!native_) return 1.0f;
    return (f32)atomic_load(&g_win.dpi, MO_ACQUIRE) / 96.0f;
}

void HostWindow::pump(EventBus& bus, Input& input) {
    if (!native_) return;
    Win32Window& w = g_win;
    os_mutex_lock(&w.mu);
    Array<WinMsg> swap = w.queue;
    w.queue = w.taken;
    w.taken = swap;
    os_mutex_unlock(&w.mu);
    MouseState& mouse = input.raw.mouse;
    for (const WinMsg& m : w.taken) {
        switch (m.type) {
        case WinMsgType::Close:
            bus.emit(CloseRequested{0});
            break;
        case WinMsgType::Size:
            if ((m.a != width_ || m.b != height_) && m.a && m.b) {
                width_  = m.a;
                height_ = m.b;
                bus.emit(WindowResized{m.a, m.b});
            }
            break;
        case WinMsgType::Focus:
            if (m.on == focused_) break;
            focused_ = m.on;
            if (!m.on) input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{m.on});
            break;
        case WinMsgType::Char: {
            char utf8[5] = {};
            utf8_encode(m.a, utf8);
            input_typed(bus, input, utf8);
            break;
        }
        case WinMsgType::Key: {
            Key k = key_from_scancode(m.a);
            if (k == Key::Backspace && m.on) ++input.raw.backspaces; // repeats too
            if (k == Key::Unknown) break;
            input.raw.set(k, m.on);
            if (m.on) bus.emit(KeyDown{k, 0, m.repeat});
            else      bus.emit(KeyUp{k, 0});
            break;
        }
        case WinMsgType::Move:
            if (m.x == mouse.x && m.y == mouse.y) break;
            bus.emit(MouseMoved{m.x, m.y, m.x - mouse.x, m.y - mouse.y});
            mouse.x = m.x;
            mouse.y = m.y;
            break;
        case WinMsgType::Button: {
            MouseButton b = (MouseButton)m.button;
            mouse.x = m.x;
            mouse.y = m.y;
            mouse.set(b, m.on);
            if (m.on) bus.emit(MouseButtonDown{b, m.x, m.y});
            else      bus.emit(MouseButtonUp{b, m.x, m.y});
            break;
        }
        case WinMsgType::Wheel:
            mouse.wheel += m.y;
            bus.emit(MouseWheel{m.x, m.y});
            break;
        }
    }
    w.taken.clear();
    sdl_pads_pump(bus, input);
}

// pump never asks the OS for anything here: it takes what the window's thread queued.
void HostWindow::drain(EventBus& bus, Input& input) { pump(bus, input); }

// The window's thread takes the system's modal move and size loops, so the frame loop is
// never held in one and this is never called.
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
