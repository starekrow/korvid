// The host window on Win32, on a thread of its own. Windows runs a modal loop of its own
// while the user drags or resizes a window, and the thread that owns the window is stuck
// in it until the mouse comes up. Here that is the window's thread, which only turns
// messages into a queue; the engine's thread drains the queue in pump() and keeps ticking
// and presenting through the drag. Per-monitor DPI aware: sizes in WindowDesc are logical
// (scaled by the display's DPI), the framebuffer and the pointer are in physical pixels,
// and a move to a display of another DPI resizes the window as Windows suggests. Alt and
// F10 do not open the window menu (another modal loop); Alt+F4 still asks to close.
// Gamepads come from SDL3 alone (src/sdl3_pads/): its gamepad subsystem, polled here.
//
// Text: typed characters are WM_CHAR. An input method's composition is the IMM messages:
// the system's composition window is turned off (the text field draws the composition
// inline, from TextComposing), the candidate list is kept and placed by text_area, and the
// result string arrives as typed characters. The IME context, the cursor and the window
// belong to the window's thread, so the engine's thread asks for them with a message; the
// clipboard is opened from the engine's thread itself.
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
#include <imm.h>
#include <string.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "imm32.lib") // delay-loaded (toolchain-windows): opened at the first composition

namespace kv {

namespace {

enum class WinMsgType : u8 { Close, Size, Focus, Key, Move, Button, Wheel, Char, Compose };

// What the window thread saw, in the order it saw it.
struct WinMsg {
    WinMsgType type;
    bool       on;      // Focus: focused; Key, Button: down
    bool       repeat;  // Key: auto-repeat
    u8         button;  // Button: a MouseButton
    u32        a, b;    // Size: width, height in pixels; Key: scancode; Char: a code point;
                        // Compose: its UTF-8 in the texts queue, at a, b bytes long
    u32        c, d;    // Compose: the caret and the selection's length, in bytes
    f32        x, y;    // Move, Button: position in pixels; Wheel: dx, dy in notches
};

constexpr UINT WM_KV_DESTROY  = WM_APP + 1; // close() asks the window thread to destroy it
constexpr UINT WM_KV_CURSOR   = WM_APP + 2; // cursor(): the shape changed, show it now
constexpr UINT WM_KV_IME_AREA = WM_APP + 3; // text_area(): place the input method's windows
constexpr UINT WM_KV_IME_ON   = WM_APP + 4; // text_input(): wParam 1 gives the window its IME, 0 takes it away

struct Win32Window {
    HWND          hwnd = nullptr;
    OsThread      thread;
    OsMutex       mu;               // guards queue, texts, ready, failed and the area
    OsCond        cv;               // open() waits on it for the window to exist
    bool          ready  = false;
    bool          failed = false;
    Array<WinMsg> queue;            // the window thread appends
    Array<WinMsg> taken;            // pump's side of the swap, the engine thread's alone
    Array<char>   texts;            // compositions' UTF-8, which Compose messages point into
    Array<char>   texts_taken;
    WindowDesc    desc;
    wchar_t       title[256] = {};
    u32           width = 0, height = 0; // the client size at creation, before ready
    u32           dpi = 96;              // written by the window thread, read atomically
    u8            buttons = 0;           // the window thread's: mouse buttons held, for capture
    u32           cursor = 0;            // a CursorShape: the engine's thread writes, the window's reads
    RECT          area = {};             // the text field's caret area, client pixels (under mu)
    i32           area_caret = 0;        // the caret's x within it
    bool          area_set = false;
    bool          composing = false;     // the window thread's: a composition was posted and not ended
};

Win32Window g_win; // one window per process

void post(const WinMsg& m) {
    os_mutex_lock(&g_win.mu);
    g_win.queue.push(m);
    os_mutex_unlock(&g_win.mu);
}

// ---- the input method -------------------------------------------------------------------------------

// A composition's text (UTF-16, n units) to the engine: its UTF-8, the caret and the
// selection (the clause being converted) at their byte offsets. Empty: it ended.
void post_compose(const wchar_t* s, int n, int caret, int sel_from, int sel_len) {
    auto bytes = [&](int units) { return units > 0 ? WideCharToMultiByte(CP_UTF8, 0, s, units, nullptr, 0, nullptr, nullptr) : 0; };
    int len = bytes(n);
    WinMsg m{};
    m.type = WinMsgType::Compose;
    os_mutex_lock(&g_win.mu);
    m.a = g_win.texts.len;
    m.b = (u32)len;
    if (len) WideCharToMultiByte(CP_UTF8, 0, s, n, g_win.texts.push_n((u32)len), len, nullptr, nullptr);
    m.c = (u32)bytes(sel_len > 0 ? sel_from : caret);
    m.d = sel_len > 0 ? (u32)bytes(sel_from + sel_len) - m.c : 0;
    g_win.queue.push(m);
    os_mutex_unlock(&g_win.mu);
    g_win.composing = n > 0;
}

void post_char(u32 u) {
    WinMsg m{};
    m.type = WinMsgType::Char;
    m.a = u;
    post(m);
}

// The IME's composition string, its caret and the clause it is converting (the target
// attributes), posted; the result string, if one came, posted first as typed characters.
void ime_composition(HWND h, LPARAM lp) {
    HIMC imc = ImmGetContext(h);
    if (!imc) return;
    wchar_t text[256];
    if (lp & GCS_RESULTSTR) {
        LONG got = ImmGetCompositionStringW(imc, GCS_RESULTSTR, text, sizeof text);
        int n = got > 0 ? (int)(got / sizeof(wchar_t)) : 0;
        if (g_win.composing) post_compose(L"", 0, 0, 0, 0); // what was composing is replaced by the result
        for (int i = 0; i < n; ++i) {
            u32 u = text[i];
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < n) { u = 0x10000 + ((u - 0xD800) << 10) + ((u32)text[i + 1] - 0xDC00); ++i; }
            if (u >= 0x20 && u != 0x7F) post_char(u);
        }
    }
    if (lp & GCS_COMPSTR) {
        LONG got = ImmGetCompositionStringW(imc, GCS_COMPSTR, text, sizeof text);
        int n = got > 0 ? (int)(got / sizeof(wchar_t)) : 0;
        int caret = (lp & GCS_CURSORPOS) ? (int)(ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0) & 0xFFFF) : n;
        if (caret > n) caret = n;
        int sel_from = 0, sel_len = 0;
        u8 attr[256];
        LONG na = (lp & GCS_COMPATTR) ? ImmGetCompositionStringW(imc, GCS_COMPATTR, attr, sizeof attr) : 0;
        for (int i = 0; i < na && i < n; ++i) {
            bool target = attr[i] == ATTR_TARGET_CONVERTED || attr[i] == ATTR_TARGET_NOTCONVERTED;
            if (target && !sel_len) sel_from = i;
            if (target) ++sel_len;
            else if (sel_len) break;
        }
        if (n || g_win.composing) post_compose(text, n, caret, sel_from, sel_len);
    }
    ImmReleaseContext(h, imc);
}

// The candidate list beside the caret, kept off the text field's line; the composition
// window (not shown) at the caret, which some IMEs place their candidates by.
void ime_place(HWND h) {
    os_mutex_lock(&g_win.mu);
    bool set = g_win.area_set;
    RECT r = g_win.area;
    LONG caret = g_win.area_caret;
    os_mutex_unlock(&g_win.mu);
    if (!set) return;
    HIMC imc = ImmGetContext(h);
    if (!imc) return;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = POINT{r.left + caret, r.top};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE;
    cand.ptCurrentPos = POINT{r.left + caret, r.bottom};
    cand.rcArea = r;
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(h, imc);
}

// ---- the cursor ------------------------------------------------------------------------------------

void show_cursor() {
    static const LPCWSTR ids[(u32)CursorShape::Count] = {IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, IDC_NO};
    static HCURSOR made[(u32)CursorShape::Count] = {};
    u32 i = atomic_load(&g_win.cursor, MO_ACQUIRE);
    if (i >= (u32)CursorShape::Count) i = 0;
    if (!made[i]) made[i] = LoadCursorW(nullptr, ids[i]);
    SetCursor(made[i]);
}

// Whether the pointer is over the window's client area now.
bool pointer_inside(HWND h) {
    POINT p;
    if (!GetCursorPos(&p) || WindowFromPoint(p) != h) return false;
    ScreenToClient(h, &p);
    RECT c;
    GetClientRect(h, &c);
    return PtInRect(&c, p) != 0;
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
        post_char(u);
        return 0;
    }

    // The input method. The system's composition window stays hidden: the field draws the
    // composition. Its result comes through ime_composition, not as WM_IME_CHAR.
    case WM_IME_SETCONTEXT: return DefWindowProcW(h, msg, wp, lp & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);
    case WM_IME_STARTCOMPOSITION: ime_place(h); return 0;
    case WM_IME_COMPOSITION: ime_composition(h, lp); return 0;
    case WM_IME_ENDCOMPOSITION:
        if (g_win.composing) post_compose(L"", 0, 0, 0, 0);
        return 0;
    case WM_KV_IME_AREA: ime_place(h); return 0;
    case WM_KV_IME_ON: ImmAssociateContextEx(h, nullptr, wp ? IACE_DEFAULT : 0); return 0;

    // The cursor: the engine's shape over the client area, the system's over the frame.
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { show_cursor(); return TRUE; }
        break;
    case WM_KV_CURSOR:
        if (pointer_inside(h)) show_cursor();
        return 0;
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
    w.texts.clear();
    w.texts_taken.clear();
    w.cursor = 0;
    w.area_set = false;
    w.composing = false;
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

// Windows sends WM_CHAR whenever the window has the keyboard, so typed text needs nothing
// switched on. The input method does: with text input off, the window has no IME context,
// so composing keys are keys (a game's WASD is not an IME's first syllable). A window
// starts with its IME, as Windows makes it.
void HostWindow::text_input(bool on) {
    if (!native_) return;
    PostMessageW(g_win.hwnd, WM_KV_IME_ON, on ? 1 : 0, 0);
}

// The caret's area in framebuffer pixels, which on this host are the client area's.
void HostWindow::text_area(rect2 r, f32 caret) {
    if (!native_) return;
    os_mutex_lock(&g_win.mu);
    g_win.area = RECT{(LONG)r.x, (LONG)r.y, (LONG)(r.x + r.w), (LONG)(r.y + r.h)};
    g_win.area_caret = (i32)caret;
    g_win.area_set = true;
    os_mutex_unlock(&g_win.mu);
    PostMessageW(g_win.hwnd, WM_KV_IME_AREA, 0, 0);
}

namespace {

// Another program may hold the clipboard for a moment.
bool open_clipboard(HWND h) {
    for (u32 i = 0; i < 10; ++i) {
        if (OpenClipboard(h)) return true;
        Sleep(1);
    }
    return false;
}

} // namespace

// CF_UNICODETEXT, its CRLFs read as the engine's LFs.
bool HostWindow::clipboard_get(Array<char>& out) {
    if (!native_ || !IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard(g_win.hwnd)) return false;
    bool got = false;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(d)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); // with the terminator
            if (n > 1) {
                u32 at = out.len;
                char* p = out.push_n((u32)n);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, p, n, nullptr, nullptr);
                u32 k = 0;
                for (u32 i = 0; i + 1 < (u32)n; ++i)
                    if (!(p[i] == '\r' && p[i + 1] == '\n')) p[k++] = p[i];
                out.len = at + k;
                got = k > 0;
            }
            GlobalUnlock(d);
        }
    }
    CloseClipboard();
    return got;
}

// The text as CF_UNICODETEXT, its LFs written as CRLFs, which Windows programs expect.
bool HostWindow::clipboard_set(const char* utf8) {
    if (!native_) return false;
    if (!utf8) utf8 = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0); // with the terminator
    if (n <= 0) return false;
    u32 lfs = 0;
    for (const char* c = utf8; *c; ++c) lfs += *c == '\n' && (c == utf8 || c[-1] != '\r');
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, ((usize)n + lfs) * sizeof(wchar_t));
    if (!mem) return false;
    wchar_t* dst = (wchar_t*)GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return false; }
    wchar_t* src = dst + lfs; // converted at the end, spread forward with the CRs
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, src, n);
    wchar_t* o = dst;
    for (int i = 0; i < n; ++i) {
        if (src[i] == L'\n' && (o == dst || o[-1] != L'\r')) *o++ = L'\r';
        *o++ = src[i];
    }
    GlobalUnlock(mem);
    bool ok = open_clipboard(g_win.hwnd);
    if (ok) {
        EmptyClipboard();
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr; // the system's from here
        CloseClipboard();
    }
    if (!ok) GlobalFree(mem);
    return ok;
}

void HostWindow::cursor(CursorShape shape) {
    if (!native_ || (u32)shape >= (u32)CursorShape::Count) return;
    if (atomic_exchange(&g_win.cursor, (u32)shape, MO_ACQ_REL) != (u32)shape) PostMessageW(g_win.hwnd, WM_KV_CURSOR, 0, 0);
}

// The window keeps the mouse whenever a button is held (post_button), which is what a drag
// asks for.
bool HostWindow::capture_mouse(bool) { return native_ != nullptr; }


void HostWindow::close() {
    if (!native_) return;
    Win32Window& w = g_win;
    PostMessageW(w.hwnd, WM_KV_DESTROY, 0, 0);
    os_thread_join(&w.thread);
    os_cond_destroy(&w.cv);
    os_mutex_destroy(&w.mu);
    w.queue.free();
    w.taken.free();
    w.texts.free();
    w.texts_taken.free();
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
    Array<char> swap_texts = w.texts;
    w.texts = w.texts_taken;
    w.texts_taken = swap_texts;
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
        case WinMsgType::Compose: {
            // As much of the composition as the event holds, at a code point's edge.
            TextComposing tc{};
            const char* src = w.texts_taken.data + m.a;
            usize n = utf8_prefix(src, m.b < sizeof tc.text - 1 ? m.b : sizeof tc.text - 1);
            memcpy(tc.text, src, n);
            tc.caret = m.c < n ? m.c : (u32)n;
            tc.selection = tc.caret + m.d <= n ? m.d : (u32)n - tc.caret;
            bus.emit(tc);
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
    w.texts_taken.clear();
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
