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
//
// A transparent window (slice 13, KR39) is a popup with no frame. Its picture's alpha is
// composited by the DWM, which is always on: a D3D12 window has no redirection bitmap
// (WS_EX_NOREDIRECTIONBITMAP) and the renderer presents through DirectComposition; a
// software window is a layered one, its frame given with UpdateLayeredWindow. Its shape is
// the window's region (SetWindowRgn): outside it a click reaches what is behind, and
// nothing is drawn. Grips answer WM_NCHITTEST and bounds WM_GETMINMAXINFO, on the window's
// thread, from copies kept under the lock.
#include "korvid/platform_hid/host_window.h"
#include "korvid/core/time.h"
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

enum class WinMsgType : u8 { Close, Size, Focus, Key, Move, Button, Wheel, Char, Compose, Minimized, Moved };

// What the window thread saw, in the order it saw it.
struct WinMsg {
    WinMsgType type;
    bool       on;      // Focus: focused; Key, Button: down; Minimized: minimized
    bool       repeat;  // Key: auto-repeat
    u8         button;  // Button: a MouseButton
    u32        a, b;    // Size: width, height in pixels; Key: scancode; Char: a code point; Move, Button: the
                        // pointer on the desktop then (GetMessagePos), as i32s;
                        // Compose: its UTF-8 in the texts queue, at a, b bytes long
    u32        c, d;    // Compose: the caret and the selection's length, in bytes
    f32        x, y;    // Move, Button: position in pixels; Wheel: dx, dy in notches; Moved: the client area's top-left on the desktop
    f64        at;      // time_now() as the window thread queued it: input_delay()'s start
    u8         win;     // the window it came from: its HostWindow's id
};

constexpr UINT WM_KV_DESTROY  = WM_APP + 1; // close() asks the window thread to destroy it
constexpr UINT WM_KV_CURSOR   = WM_APP + 2; // cursor(): the shape changed, show it now
constexpr UINT WM_KV_IME_AREA = WM_APP + 3; // text_area(): place the input method's windows
constexpr UINT WM_KV_IME_ON   = WM_APP + 4; // text_input(): wParam 1 gives the window its IME, 0 takes it away
constexpr UINT WM_KV_SHAPE    = WM_APP + 5; // shape(): lParam an HRGN (null: none), the window's from then
constexpr UINT WM_KV_BOUNDS   = WM_APP + 6; // bounds(): hold the size it has now to them

struct Win32Window {
    HWND          hwnd = nullptr;
    // A software window's pixels (software_frame), B, G, R, X top-down, the engine thread's;
    // present_software() blits them to the window's DC.
    u8*           soft = nullptr;
    u32           soft_w = 0, soft_h = 0;
    usize         soft_bytes = 0;
    // A layered window's (transparent and software): its pixels are a DIB section's, B, G,
    // R, A premultiplied, selected into soft_dc for UpdateLayeredWindow; soft is its bits.
    HBITMAP       soft_dib = nullptr;
    HDC           soft_dc  = nullptr;
    bool          layered  = false;
    bool          transparent = false; // a popup with no frame, its alpha composited
    u8            index = 0;        // its HostWindow's id, and its place in g_wins
    OsThread      thread;           // the window's own: it makes the window and runs its messages
    bool          ready  = false;   // under g_q.mu
    bool          failed = false;
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
    // The grips and bounds as the window's thread answers the OS with them (under mu): the
    // HostWindow's copies, given again by hid_window_grips and _bounds.
    WindowGripRect grips[KV_WINDOW_GRIPS] = {};
    u32           grip_count = 0;
    u32           bounds[4] = {};
};

Win32Window g_wins[KV_WINDOWS]; // by HostWindow id
// The window's own, on its thread: what its window procedure and the IME and cursor code
// act on. Set before the window is made, so the first messages find it.
thread_local Win32Window* t_win = nullptr;

// What every window shares: one queue, so the main window's pump sees their messages in the
// order they came, and one pair of events for the loop's wait.
struct Win32Shared {
    OsMutex       mu;               // guards the queue and texts, each window's ready, failed, area, grips and bounds
    OsCond        cv;               // open() waits on it for its window to exist
    bool          up = false;       // mu and cv made: at the first open, kept for the process
    Array<WinMsg> queue;            // the window threads append
    Array<WinMsg> taken;            // pump's side of the swap, the engine thread's alone
    Array<char>   texts;            // compositions' UTF-8, which Compose messages point into
    Array<char>   texts_taken;
    // The loop's wait (KR26). `queued` (manual reset) is set as a window thread appends to the
    // queue and reset as pump swaps it out, both under mu, so a wait ends early exactly when
    // something came since the last pump. `woken` (auto reset) is wake()'s. Made at the first
    // open and kept for the process: a wake from another thread never finds them closed.
    HANDLE        queued = nullptr;
    HANDLE        woken  = nullptr;
};
Win32Shared g_q;

// From a window's thread: a message from its window.
void post(const WinMsg& msg) {
    WinMsg m = msg;
    m.at = time_now();
    m.win = t_win->index;
    os_mutex_lock(&g_q.mu);
    g_q.queue.push(m);
    SetEvent(g_q.queued);
    os_mutex_unlock(&g_q.mu);
}

// ---- the input method -------------------------------------------------------------------------------

// A composition's text (UTF-16, n units) to the engine: its UTF-8, the caret and the
// selection (the clause being converted) at their byte offsets. Empty: it ended.
void post_compose(const wchar_t* s, int n, int caret, int sel_from, int sel_len) {
    Win32Window& W = *t_win;
    auto bytes = [&](int units) { return units > 0 ? WideCharToMultiByte(CP_UTF8, 0, s, units, nullptr, 0, nullptr, nullptr) : 0; };
    int len = bytes(n);
    WinMsg m{};
    m.type = WinMsgType::Compose;
    os_mutex_lock(&g_q.mu);
    m.a = g_q.texts.len;
    m.b = (u32)len;
    if (len) WideCharToMultiByte(CP_UTF8, 0, s, n, g_q.texts.push_n((u32)len), len, nullptr, nullptr);
    m.c = (u32)bytes(sel_len > 0 ? sel_from : caret);
    m.d = sel_len > 0 ? (u32)bytes(sel_from + sel_len) - m.c : 0;
    g_q.queue.push(m);
    SetEvent(g_q.queued);
    os_mutex_unlock(&g_q.mu);
    W.composing = n > 0;
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
    Win32Window& W = *t_win;
    HIMC imc = ImmGetContext(h);
    if (!imc) return;
    wchar_t text[256];
    if (lp & GCS_RESULTSTR) {
        LONG got = ImmGetCompositionStringW(imc, GCS_RESULTSTR, text, sizeof text);
        int n = got > 0 ? (int)(got / sizeof(wchar_t)) : 0;
        if (W.composing) post_compose(L"", 0, 0, 0, 0); // what was composing is replaced by the result
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
        if (n || W.composing) post_compose(text, n, caret, sel_from, sel_len);
    }
    ImmReleaseContext(h, imc);
}

// The candidate list beside the caret, kept off the text field's line; the composition
// window (not shown) at the caret, which some IMEs place their candidates by.
void ime_place(HWND h) {
    Win32Window& W = *t_win;
    os_mutex_lock(&g_q.mu);
    bool set = W.area_set;
    RECT r = W.area;
    LONG caret = W.area_caret;
    os_mutex_unlock(&g_q.mu);
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
    Win32Window& W = *t_win;
    static const LPCWSTR ids[(u32)CursorShape::Count] = {IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZEALL, IDC_SIZEWE, IDC_SIZENS, IDC_NO};
    static HCURSOR made[(u32)CursorShape::Count] = {};
    u32 i = atomic_load(&W.cursor, MO_ACQUIRE);
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
    Win32Window& W = *t_win;
    // The window keeps the mouse while a button is held, so a drag that leaves the window
    // still reports its moves and its release.
    u8 bit = (u8)(1u << (u32)b);
    if (down) { if (!W.buttons) SetCapture(h); W.buttons |= bit; }
    else { W.buttons &= (u8)~bit; if (!W.buttons) ReleaseCapture(); }
    WinMsg m{};
    m.type = WinMsgType::Button;
    m.on = down;
    m.button = (u8)b;
    m.x = (f32)GET_X_LPARAM(lp);
    m.y = (f32)GET_Y_LPARAM(lp);
    DWORD at = GetMessagePos(); // the cursor on the desktop when the message was made
    m.a = (u32)GET_X_LPARAM(at);
    m.b = (u32)GET_Y_LPARAM(at);
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

// ---- grips and bounds ------------------------------------------------------------------------------

// What the window's copy of the grips says is under a client pixel: the last that holds it.
WindowGrip grip_at_point(f32 x, f32 y) {
    Win32Window& W = *t_win;
    WindowGrip g = WindowGrip::None;
    os_mutex_lock(&g_q.mu);
    for (u32 i = W.grip_count; i-- > 0;) {
        const rect2& r = W.grips[i].rect;
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) { g = W.grips[i].grip; break; }
    }
    os_mutex_unlock(&g_q.mu);
    return g;
}

LRESULT hit_of(WindowGrip g) {
    switch (g) {
    case WindowGrip::Move: return HTCAPTION;
    case WindowGrip::N:    return HTTOP;
    case WindowGrip::NE:   return HTTOPRIGHT;
    case WindowGrip::E:    return HTRIGHT;
    case WindowGrip::SE:   return HTBOTTOMRIGHT;
    case WindowGrip::S:    return HTBOTTOM;
    case WindowGrip::SW:   return HTBOTTOMLEFT;
    case WindowGrip::W:    return HTLEFT;
    case WindowGrip::NW:   return HTTOPLEFT;
    case WindowGrip::None: break;
    }
    return HTCLIENT;
}

// How much bigger the window is than its client area: its frame, none for a popup.
SIZE frame_extra(HWND h) {
    RECT w, c;
    GetWindowRect(h, &w);
    GetClientRect(h, &c);
    return SIZE{(w.right - w.left) - c.right, (w.bottom - w.top) - c.bottom};
}

LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Window& W = *t_win;
    switch (msg) {
    case WM_CLOSE: { WinMsg m{}; m.type = WinMsgType::Close; post(m); return 0; } // the engine decides
    case WM_KV_DESTROY: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;

    case WM_SIZE: {
        // A minimized window keeps its last size, and is not seen (KR21); any other size
        // is a window shown again.
        WinMsg v{};
        v.type = WinMsgType::Minimized;
        v.on   = wp == SIZE_MINIMIZED;
        post(v);
        if (wp == SIZE_MINIMIZED) return 0;
        WinMsg m{};
        m.type = WinMsgType::Size;
        m.a = LOWORD(lp);
        m.b = HIWORD(lp);
        post(m);
        return 0;
    }
    case WM_MOVE: { // the client area's top-left on the desktop (KR42)
        WinMsg m{};
        m.type = WinMsgType::Moved;
        m.x = (f32)GET_X_LPARAM(lp);
        m.y = (f32)GET_Y_LPARAM(lp);
        post(m);
        return 0;
    }
    case WM_DPICHANGED: {
        atomic_store(&W.dpi, (u32)HIWORD(wp), MO_RELEASE);
        const RECT* r = (const RECT*)lp; // the size Windows suggests at the new DPI
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        if (msg == WM_KILLFOCUS && W.buttons) { W.buttons = 0; ReleaseCapture(); }
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
        if (W.composing) post_compose(L"", 0, 0, 0, 0);
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
        DWORD at = GetMessagePos();
        m.a = (u32)GET_X_LPARAM(at);
        m.b = (u32)GET_Y_LPARAM(at);
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

    // What a point is for (KR41): the last grip that holds it, when there is a list. A
    // grip's press is the OS's (a WM_NCLBUTTONDOWN), so the engine sees no button for it.
    case WM_NCHITTEST: {
        os_mutex_lock(&g_q.mu);
        u32 n = W.grip_count;
        os_mutex_unlock(&g_q.mu);
        if (!n) break;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &p);
        RECT c;
        GetClientRect(h, &c);
        if (!PtInRect(&c, p)) break; // a frame's own edges, where there is one
        return hit_of(grip_at_point((f32)p.x + 0.5f, (f32)p.y + 0.5f));
    }
    case WM_NCCALCSIZE:
        if (W.transparent) return 0; // the client area is the whole window, at creation too: no frame drawn
        break;
    case WM_GETMINMAXINFO: {
        os_mutex_lock(&g_q.mu);
        u32 b[4] = {W.bounds[0], W.bounds[1], W.bounds[2], W.bounds[3]};
        os_mutex_unlock(&g_q.mu);
        if (!(b[0] | b[1] | b[2] | b[3])) break;
        SIZE f = frame_extra(h);
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        if (b[0]) mm->ptMinTrackSize.x = (LONG)b[0] + f.cx;
        if (b[1]) mm->ptMinTrackSize.y = (LONG)b[1] + f.cy;
        if (b[2]) mm->ptMaxTrackSize.x = (LONG)b[2] + f.cx;
        if (b[3]) mm->ptMaxTrackSize.y = (LONG)b[3] + f.cy;
        return 0;
    }
    case WM_KV_SHAPE:
        // The system owns the region from here, and frees the one it replaces.
        if (!SetWindowRgn(h, (HRGN)lp, TRUE) && lp) DeleteObject((HRGN)lp);
        return 0;
    case WM_KV_BOUNDS: {
        // A size outside the new bounds is brought inside them: the OS holds a resize to
        // them, and SetWindowPos asks WM_GETMINMAXINFO too.
        os_mutex_lock(&g_q.mu);
        u32 b[4] = {W.bounds[0], W.bounds[1], W.bounds[2], W.bounds[3]};
        os_mutex_unlock(&g_q.mu);
        RECT c;
        GetClientRect(h, &c);
        LONG w = c.right, ht = c.bottom;
        auto hold = [](LONG v, u32 lo, u32 hi) { if (lo && v < (LONG)lo) v = (LONG)lo; if (hi && v > (LONG)hi) v = (LONG)hi; return v; };
        LONG nw = hold(w, b[0], b[2]), nh = hold(ht, b[1], b[3]);
        if (nw == w && nh == ht) return 0;
        SIZE f = frame_extra(h);
        SetWindowPos(h, nullptr, 0, 0, nw + f.cx, nh + f.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    // A renderer's window (Vulkan or D3D12) is the renderer's to paint; erasing it would
    // flash the background.
    case WM_ERASEBKGND:
        if (W.desc.surface != WindowSurface::Software) return 1;
        break;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The window's size for a client area of the desc's logical size at a DPI.
SIZE outer_size(DWORD style, UINT dpi) {
    Win32Window& W = *t_win;
    RECT r = {0, 0, MulDiv((int)W.desc.width, (int)dpi, 96), MulDiv((int)W.desc.height, (int)dpi, 96)};
    if (!W.transparent) AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi); // a transparent window's frame is its client area
    return SIZE{r.right - r.left, r.bottom - r.top};
}

// A window's thread: creates the window, says so, then runs its messages until close().
void window_main(void* arg) {
    Win32Window& w = *(Win32Window*)arg;
    t_win = &w;
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
    // A transparent window is a popup with no frame at all: what is not drawn is the
    // desktop's, and grips are what move and resize it. WS_THICKFRAME is what lets an edge's
    // hit resize it; WM_NCCALCSIZE then gives the frame's room to the client area. WS_SYSMENU
    // and WS_MINIMIZEBOX keep the taskbar's menu and its minimize.
    DWORD style = w.desc.transparent ? (WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX)
                : w.desc.borderless  ? (WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX) : WS_OVERLAPPEDWINDOW;
    // A D3D12 transparent window has no redirection bitmap: DirectComposition gives the DWM
    // its picture, with alpha. A software one is layered: UpdateLayeredWindow does. A Vulkan
    // one keeps its bitmap, which some drivers present through, and is opaque.
    DWORD ex = !w.desc.transparent ? 0 : w.layered ? WS_EX_LAYERED
             : w.desc.surface == WindowSurface::D3D12 ? WS_EX_NOREDIRECTIONBITMAP : 0;
    UINT dpi = GetDpiForSystem();
    SIZE size = outer_size(style, dpi);
    HWND h = registered ? CreateWindowExW(ex, L"korvid", w.title, style, CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy,
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
    os_mutex_lock(&g_q.mu);
    w.hwnd   = h;
    w.failed = !h;
    w.ready  = true;
    os_cond_broadcast(&g_q.cv);
    os_mutex_unlock(&g_q.mu);
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
    Win32Window& w = g_wins[id_];
    w.index = id_;
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
    w.cursor = 0;
    w.area_set = false;
    w.composing = false;
    w.transparent = desc.transparent;
    w.layered = desc.transparent && desc.surface == WindowSurface::Software;
    w.grip_count = 0;
    for (u32& b : w.bounds) b = 0;
    if (!g_q.queued) g_q.queued = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_q.woken) g_q.woken = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_q.queued || !g_q.woken) {
        KV_ERROR("window", "CreateEventW failed (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    if (!g_q.up) {
        os_mutex_init(&g_q.mu);
        os_cond_init(&g_q.cv);
        g_q.up = true;
    }
    w.thread = OsThread();
    if (!os_thread_start(&w.thread, window_main, &w)) {
        KV_ERROR("window", "could not start the window thread");
        return false;
    }
    os_mutex_lock(&g_q.mu);
    while (!w.ready) os_cond_wait(&g_q.cv, &g_q.mu);
    bool ok = !w.failed;
    os_mutex_unlock(&g_q.mu);
    if (!ok) {
        os_thread_join(&w.thread);
        return false;
    }
    native_  = w.hwnd;
    surface_ = desc.surface;
    vsync_   = desc.vsync;
    width_   = w.width;
    height_  = w.height;
    focused_ = false; // the first pump reports it
    // The DWM composites every window; what is drawn with alpha is a layered window's and
    // DirectComposition's. Vulkan on Windows presents opaque: its shape cuts it out.
    transparent_ = desc.transparent && desc.surface != WindowSurface::Vulkan;
    KV_INFO("window", "opened '%s' %ux%u px at %u dpi%s", title, width_, height_, atomic_load(&w.dpi, MO_ACQUIRE),
            !desc.transparent ? "" : w.layered ? ", transparent (layered)" : transparent_ ? ", transparent (no redirection bitmap)"
                                                : ", opaque: Vulkan presents no alpha here; a shape cuts it out");
    return true;
}

// Windows sends WM_CHAR whenever the window has the keyboard, so typed text needs nothing
// switched on. The input method does: with text input off, the window has no IME context,
// so composing keys are keys (a game's WASD is not an IME's first syllable). A window
// starts with its IME, as Windows makes it.
void HostWindow::text_input(bool on) {
    Win32Window& W = g_wins[id_];
    if (!native_) return;
    PostMessageW(W.hwnd, WM_KV_IME_ON, on ? 1 : 0, 0);
}

// The caret's area in framebuffer pixels, which on this host are the client area's.
void HostWindow::text_area(rect2 r, f32 caret) {
    Win32Window& W = g_wins[id_];
    if (!native_) return;
    os_mutex_lock(&g_q.mu);
    W.area = RECT{(LONG)r.x, (LONG)r.y, (LONG)(r.x + r.w), (LONG)(r.y + r.h)};
    W.area_caret = (i32)caret;
    W.area_set = true;
    os_mutex_unlock(&g_q.mu);
    PostMessageW(W.hwnd, WM_KV_IME_AREA, 0, 0);
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
    Win32Window& W = g_wins[id_];
    if (!native_ || !IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard(W.hwnd)) return false;
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
    Win32Window& W = g_wins[id_];
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
    bool ok = open_clipboard(W.hwnd);
    if (ok) {
        EmptyClipboard();
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr; // the system's from here
        CloseClipboard();
    }
    if (!ok) GlobalFree(mem);
    return ok;
}

void HostWindow::minimize(bool on) {
    Win32Window& W = g_wins[id_];
    if (native_) ShowWindowAsync(W.hwnd, on ? SW_MINIMIZE : SW_RESTORE); // the window thread's to do
}

void HostWindow::cursor(CursorShape shape) {
    Win32Window& W = g_wins[id_];
    if (!native_ || (u32)shape >= (u32)CursorShape::Count) return;
    if (atomic_exchange(&W.cursor, (u32)shape, MO_ACQ_REL) != (u32)shape) PostMessageW(W.hwnd, WM_KV_CURSOR, 0, 0);
}

// The window keeps the mouse whenever a button is held (post_button), which is what a drag
// asks for.
bool HostWindow::capture_mouse(bool) { return native_ != nullptr; }


void HostWindow::close() {
    if (!native_) return;
    Win32Window& w = g_wins[id_];
    PostMessageW(w.hwnd, WM_KV_DESTROY, 0, 0);
    os_thread_join(&w.thread);
    // What it queued and no pump took is dropped: its id may be another window's by then.
    bool others = false;
    os_mutex_lock(&g_q.mu);
    u32 k = 0;
    for (const WinMsg& m : g_q.queue) if (m.win != w.index) g_q.queue.data[k++] = m;
    g_q.queue.len = k;
    os_mutex_unlock(&g_q.mu);
    for (const Win32Window& o : g_wins) others = others || (&o != &w && o.hwnd);
    if (!others) { // the last: the queue's memory goes back
        g_q.queue.free();
        g_q.taken.free();
        g_q.texts.free();
        g_q.texts_taken.free();
    }
    if (w.soft_dib) {
        DeleteObject(w.soft_dib); // its bits were soft
        w.soft = nullptr;
    }
    if (w.soft_dc) DeleteDC(w.soft_dc);
    w.soft_dib = nullptr;
    w.soft_dc = nullptr;
    if (w.soft) free_in(mem_general(), w.soft, w.soft_bytes);
    w.soft = nullptr;
    w.soft_bytes = 0;
    w.grip_count = 0;
    w.hwnd = nullptr;
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    hidden_  = 0;
    hidden_sent_ = WindowHidden::None;
    hid_window_parts_reset(*this);
    if (id_) KV_INFO("window", "window %u closed", id_);
    else KV_INFO("window", "closed");
}

// The window class's brush paints a software window; nothing to do here.
void HostWindow::present_blank() {}

bool HostWindow::software_frame(SoftwareFrame* out) {
    Win32Window& W = g_wins[id_];
    HWND h = (HWND)native_;
    if (!h || surface_ != WindowSurface::Software) return false;
    RECT rc;
    if (!GetClientRect(h, &rc)) return false;
    u32 w = (u32)(rc.right - rc.left), hh = (u32)(rc.bottom - rc.top);
    if (!w || !hh) return false;
    usize bytes = (usize)w * hh * 4;
    if (W.layered) {
        // A DIB section the size of the window, made again when the size changes: what
        // UpdateLayeredWindow reads.
        if (!W.soft_dib || w != W.soft_w || hh != W.soft_h) {
            if (!W.soft_dc) W.soft_dc = CreateCompatibleDC(nullptr);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)w;
            bi.bmiHeader.biHeight = -(LONG)hh; // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP dib = W.soft_dc ? CreateDIBSection(W.soft_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
            if (!dib) return false;
            SelectObject(W.soft_dc, dib);
            if (W.soft_dib) DeleteObject(W.soft_dib);
            W.soft_dib = dib;
            W.soft = (u8*)bits;
        }
        W.soft_w = w;
        W.soft_h = hh;
        software_ = W.soft;
        *out = SoftwareFrame{W.soft, w, hh, w * 4, false, false, true};
        return true;
    }
    if (bytes > W.soft_bytes) {
        if (W.soft) free_in(mem_general(), W.soft, W.soft_bytes);
        W.soft = (u8*)alloc_in(mem_general(), bytes, 16);
        W.soft_bytes = W.soft ? bytes : 0;
        if (!W.soft) return false;
    }
    W.soft_w = w;
    W.soft_h = hh;
    software_ = W.soft;
    *out = SoftwareFrame{W.soft, w, hh, w * 4, false};
    return true;
}

// A DIB blitted with SetDIBitsToDevice: nothing waits for the display; the frame loop paces.
bool HostWindow::software_vsync(bool) { return false; }

void HostWindow::present_software() {
    Win32Window& W = g_wins[id_];
    HWND h = (HWND)native_;
    if (!h || !software_) return;
    software_ = nullptr;
    if (W.layered) {
        // Its premultiplied alpha composited by the DWM; a pixel of alpha 0 also passes the
        // mouse through. The size given is the window's: a frame drawn at a size the window
        // has since left (a drag under way) is not shown, since showing it would undo the drag.
        RECT c;
        if (!GetClientRect(h, &c) || (u32)c.right != W.soft_w || (u32)c.bottom != W.soft_h) return;
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT at = {0, 0};
        SIZE size = {(LONG)W.soft_w, (LONG)W.soft_h};
        if (!UpdateLayeredWindow(h, nullptr, nullptr, &size, W.soft_dc, &at, 0, &bf, ULW_ALPHA)) {
            static bool told = false;
            if (!told) KV_WARN("window", "UpdateLayeredWindow failed (error %lu)", (unsigned long)GetLastError());
            told = true;
        }
        return;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)W.soft_w;
    bi.bmiHeader.biHeight = -(LONG)W.soft_h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(h); // GDI on another thread's window: allowed with its DC
    if (!dc) return;
    SetDIBitsToDevice(dc, 0, 0, W.soft_w, W.soft_h, 0, 0, 0, W.soft_h, W.soft, &bi, DIB_RGB_COLORS);
    ReleaseDC(h, dc);
}

// Physical pixels per logical unit: the window's DPI over 96.
f32 HostWindow::pixel_scale() const {
    Win32Window& W = g_wins[id_];
    if (!native_) return 1.0f;
    return (f32)atomic_load(&W.dpi, MO_ACQUIRE) / 96.0f;
}

void HostWindow::pump(EventBus& bus, Input& input) {
    if (!native_) return;
    os_mutex_lock(&g_q.mu);
    Array<WinMsg> swap = g_q.queue;
    g_q.queue = g_q.taken;
    g_q.taken = swap;
    Array<char> swap_texts = g_q.texts;
    g_q.texts = g_q.texts_taken;
    g_q.texts_taken = swap_texts;
    ResetEvent(g_q.queued); // what it was set for is taken
    os_mutex_unlock(&g_q.mu);
    // A wake the platform already took (its flag is set before the event) must not end the
    // next wait as well; one after this sets the flag again, which the next wait reads first.
    ResetEvent(g_q.woken);
    MouseState& mouse = input.raw.mouse;
    for (const WinMsg& m : g_q.taken) {
        HostWindow* hw = hid_window(m.win); // every window's messages come here, the main one's pump
        if (!hw || !hw->is_open()) continue;
        HostWindow& w = *hw;
        u8 id = w.id_;
        // How long a press waited for the pump, from the window thread's queueing of it: the
        // part the loop's wait decides (the OS's own delivery before it is not counted here).
        if ((m.type == WinMsgType::Key || m.type == WinMsgType::Button) && m.on) input_delay_ = time_now() - m.at;
        switch (m.type) {
        case WinMsgType::Close:
            bus.emit(CloseRequested{id});
            break;
        case WinMsgType::Size:
            if ((m.a != w.width_ || m.b != w.height_) && m.a && m.b) {
                w.width_  = m.a;
                w.height_ = m.b;
                if (w.shape_.len) hid_window_shape(w); // the mask scaled to the new size
                bus.emit(WindowResized{m.a, m.b, id});
            }
            break;
        case WinMsgType::Minimized:
            w.set_hidden(bus, HiddenMinimized, m.on);
            break;
        case WinMsgType::Moved:
            bus.emit(WindowMoved{(i32)m.x, (i32)m.y, id});
            break;
        case WinMsgType::Focus:
            if (m.on == w.focused_) break;
            w.focused_ = m.on;
            if (!m.on) input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{m.on, id});
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
            const char* src = g_q.texts_taken.data + m.a;
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
            mouse.desktop_x = (f32)(i32)m.a;
            mouse.desktop_y = (f32)(i32)m.b;
            mouse.desktop = true;
            if (mouse.window != id) { // into another window: its pixels, so no travel
                mouse.window = id;
                bus.emit(MouseMoved{m.x, m.y, 0.0f, 0.0f, id});
            } else {
                if (m.x == mouse.x && m.y == mouse.y) break;
                bus.emit(MouseMoved{m.x, m.y, m.x - mouse.x, m.y - mouse.y, id});
            }
            mouse.x = m.x;
            mouse.y = m.y;
            break;
        case WinMsgType::Button: {
            MouseButton b = (MouseButton)m.button;
            mouse.window = id;
            mouse.x = m.x;
            mouse.y = m.y;
            mouse.desktop_x = (f32)(i32)m.a;
            mouse.desktop_y = (f32)(i32)m.b;
            mouse.desktop = true;
            mouse.set(b, m.on);
            if (m.on) bus.emit(MouseButtonDown{b, m.x, m.y, id});
            else      bus.emit(MouseButtonUp{b, m.x, m.y, id});
            break;
        }
        case WinMsgType::Wheel:
            mouse.wheel += m.y;
            bus.emit(MouseWheel{m.x, m.y, id});
            break;
        }
    }
    g_q.taken.clear();
    g_q.texts_taken.clear();
    sdl_pads_pump(bus, input);
}

// pump never asks the OS for anything here: it takes what the window's thread queued.
void HostWindow::drain(EventBus& bus, Input& input) { pump(bus, input); }

// The window's thread sets `queued` as it queues a message, so the loop's thread waits on
// that and on wake()'s event, never on a message queue of its own (it has none: the window's
// thread owns the window). Whole milliseconds, rounded up, as SDL's wait takes them. Pads
// read through SDL beside the window do not end a wait (slice 12, "Not woken").
bool HostWindow::wait(f64 seconds) {
    if (seconds <= 0.0 || !native_) return false;
    f64 ms = seconds * 1000.0 + 0.999;
    HANDLE both[2] = {g_q.queued, g_q.woken};
    DWORD r = WaitForMultipleObjects(2, both, FALSE, ms >= 4.0e9 ? INFINITE - 1 : (DWORD)ms);
    return r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1;
}
void HostWindow::wake() {
    if (g_q.woken) SetEvent(g_q.woken);
}

// The window's thread takes the system's modal move and size loops, so the frame loop is
// never held in one and this is never called.
void HostWindow::on_modal(void (*fn)(void* ctx), void* ctx) {
    modal_fn_  = fn;
    modal_ctx_ = ctx;
}

bool hid_backend_holds_unfocused() { return false; }
u32  hid_backend_windows() { return KV_WINDOWS; }
bool hid_backend_can_place() { return true; }

// Desktop pixels are physical pixels here (per-monitor DPI aware): the client area's origin
// on the desktop, asked of the window as it is now, from any thread.
bool HostWindow::position(i32* x, i32* y) const {
    if (!native_) return false;
    POINT p{0, 0};
    if (!ClientToScreen((HWND)native_, &p)) return false;
    *x = p.x;
    *y = p.y;
    return true;
}

// The window's own rect is placed so its client area lands at (x, y); the move is posted to
// the window's thread (SWP_ASYNCWINDOWPOS), which may be in a modal loop of its own.
bool HostWindow::move_to(i32 x, i32 y) {
    HWND h = (HWND)native_;
    if (!h) return false;
    RECT wr;
    POINT c{0, 0};
    if (!GetWindowRect(h, &wr) || !ClientToScreen(h, &c)) return false;
    return SetWindowPos(h, nullptr, x - (c.x - wr.left), y - (c.y - wr.top), 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != 0;
}

// The work area of the monitor the window is most on.
bool HostWindow::display_bounds(rect2* out) const {
    if (!native_) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow((HWND)native_, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& r = mi.rcWork;
    *out = rect2{(f32)r.left, (f32)r.top, (f32)(r.right - r.left), (f32)(r.bottom - r.top)};
    return true;
}

// The window's thread shows it or hides it; shown, it does not take the focus. Seen or not
// is the program's reason, given now and sent at the next pump.
void HostWindow::show(bool on) {
    if (!native_) return;
    ShowWindowAsync(g_wins[id_].hwnd, on ? SW_SHOWNA : SW_HIDE);
    hidden_ = on ? (u8)(hidden_ & ~HiddenByProgram) : (u8)(hidden_ | HiddenByProgram);
}
bool HostWindow::touch_screen() const { return false; }

int platform_app_main(int argc, char** argv, int (*run)(int argc, char** argv, void* ctx), void* ctx) {
    return run(argc, argv, ctx);
}

// The shape as the window's region: the mask scaled to the client area, nearest neighbour,
// cut at a half, one rect a run of a row. A popup's window and client areas are the same,
// so the region's coordinates are the client's. The region clips what the DWM shows too,
// one bit a pixel: a soft edge is drawn inside it and cut at it.
bool hid_window_shape(HostWindow& hw) {
    Win32Window& W = g_wins[hw.id_];
    if (!hw.native_) return false;
    if (!W.transparent) {
        static bool told = false;
        if (!told) KV_WARN("window", "a shape on a window not opened transparent (WindowDesc::transparent): it stays a rectangle");
        told = true;
        return false;
    }
    HRGN rgn = nullptr;
    if (hw.shape_.len) {
        u32 ww = hw.width_, wh = hw.height_;
        if (!ww || !wh) return false;
        Array<RECT> runs;
        for (u32 y = 0; y < wh; ++y) {
            const u8* src = hw.shape_.data + (usize)((u64)y * hw.shape_h_ / wh) * hw.shape_w_;
            u32 x = 0;
            while (x < ww) {
                while (x < ww && src[(u64)x * hw.shape_w_ / ww] < 128) ++x;
                u32 from = x;
                while (x < ww && src[(u64)x * hw.shape_w_ / ww] >= 128) ++x;
                if (x > from) runs.push(RECT{(LONG)from, (LONG)y, (LONG)x, (LONG)y + 1});
            }
        }
        usize bytes = sizeof(RGNDATAHEADER) + (usize)runs.len * sizeof(RECT);
        RGNDATA* rd = (RGNDATA*)alloc_in(mem_general(), bytes, alignof(RGNDATA));
        if (!rd) { runs.free(); return false; }
        rd->rdh.dwSize   = sizeof(RGNDATAHEADER);
        rd->rdh.iType    = RDH_RECTANGLES;
        rd->rdh.nCount   = runs.len;
        rd->rdh.nRgnSize = (DWORD)(runs.len * sizeof(RECT));
        rd->rdh.rcBound  = RECT{0, 0, (LONG)ww, (LONG)wh};
        if (runs.len) memcpy(rd->Buffer, runs.data, (usize)runs.len * sizeof(RECT));
        rgn = ExtCreateRegion(nullptr, (DWORD)bytes, rd);
        free_in(mem_general(), rd, bytes);
        u32 n = runs.len;
        runs.free();
        if (!rgn) {
            static bool told = false;
            if (!told) KV_WARN("window", "ExtCreateRegion failed for %u rects (error %lu)", n, (unsigned long)GetLastError());
            told = true;
            return false;
        }
    }
    // SetWindowRgn is the window thread's to call: from here it would wait on that thread.
    if (!PostMessageW(W.hwnd, WM_KV_SHAPE, 0, (LPARAM)rgn)) {
        if (rgn) DeleteObject(rgn);
        return false;
    }
    return true;
}

// Grips are client pixels here, which are framebuffer pixels: copied as given.
bool hid_window_grips(HostWindow& hw) {
    Win32Window& W = g_wins[hw.id_];
    if (!hw.native_) return false;
    os_mutex_lock(&g_q.mu);
    if (hw.grip_count_) memcpy(W.grips, hw.grips_, (usize)hw.grip_count_ * sizeof hw.grips_[0]);
    W.grip_count = hw.grip_count_;
    os_mutex_unlock(&g_q.mu);
    return true;
}

// The bounds are client pixels too; WM_GETMINMAXINFO adds the frame, where there is one.
void hid_window_bounds(HostWindow& hw) {
    Win32Window& W = g_wins[hw.id_];
    if (!hw.native_) return;
    os_mutex_lock(&g_q.mu);
    for (u32 i = 0; i < 4; ++i) W.bounds[i] = hw.bounds_[i];
    os_mutex_unlock(&g_q.mu);
    hw.bounds_scale_ = hw.pixel_scale();
    PostMessageW(W.hwnd, WM_KV_BOUNDS, 0, 0);
}

} // namespace kv
