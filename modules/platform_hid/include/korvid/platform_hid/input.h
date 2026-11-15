// Korvid platform: the input layer. Keyboard and gamepads; a further device type adds its
// own event structs and state block without changing this API. Key codes are
// engine-defined and mapped from the backend's scancodes (src/sdl3/, src/win32/); gamepad
// buttons and axes follow SDL3's standard layout (a Playstation or Xbox pad looks the same
// here).
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct EventBus;

enum class Key : u16 {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Space, Enter, Tab, Backspace,
    Left, Right, Up, Down,
    LShift, RShift, LCtrl, RCtrl, LAlt, RAlt, // Alt is Option on a Mac
    LGui, RGui, // Command on a Mac, the Windows key on a PC (SDL's name); appended, so the numbers above hold
    Delete, Home, End, PageUp, PageDown, // a text field's (Krome slice 3); appended likewise
    Count
};

const char* key_name(Key k);

// Gamepad buttons, by position: South is the bottom face button (A on an Xbox pad, Cross
// on a Playstation pad), East the right one, and so on.
enum class PadButton : u8 {
    South, East, West, North,
    Back, Guide, Start,
    LStick, RStick, LShoulder, RShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};
enum class PadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

const char* pad_button_name(PadButton b);
const char* pad_axis_name(PadAxis a);

// Mouse buttons, as bits of MouseState::buttons.
enum class MouseButton : u8 { Left, Middle, Right, X1, X2, Count };
const char* mouse_button_name(MouseButton b);

constexpr u32 KV_MAX_GAMEPADS   = 4;
constexpr f32 KV_PAD_DEAD_ZONE  = 0.15f; // stick axes inside it read 0

// The pointer, in framebuffer pixels with the origin at the window's top-left (what the OS
// reports). Screen-space sprites use the window's center with +y up; mouse_screen converts.
// `wheel` accumulates the scroll since the last sample (positive away from the user).
struct MouseState {
    f32 x = 0.0f, y = 0.0f;
    f32 wheel = 0.0f;
    u8  buttons = 0;
    bool down(MouseButton b) const { return (buttons >> (u32)b) & 1u; }
    void set(MouseButton b, bool on) { u8 m = (u8)(1u << (u32)b); if (on) buttons |= m; else buttons &= (u8)~m; }
};
// Window pixels (origin top-left, +y down) to screen space (origin at the center, +y up).
inline void mouse_screen(f32 x, f32 y, u32 width, u32 height, f32* sx, f32* sy) {
    *sx = x - (f32)width * 0.5f;
    *sy = (f32)height * 0.5f - y;
}

// One pad. Sticks are in [-1, 1] with +y down as SDL reports it, triggers in [0, 1].
struct GamepadState {
    u32   buttons   = 0; // bit per PadButton
    f32   axes[(u32)PadAxis::Count] = {};
    bool  connected = false;
    u32   id        = 0; // the platform's instance id, for events and tools
    void* native    = nullptr; // the SDL_Gamepad*, the platform's own

    bool down(PadButton b) const { return (buttons >> (u32)b) & 1u; }
    void set(PadButton b, bool on) { u32 m = 1u << (u32)b; if (on) buttons |= m; else buttons &= ~m; }
    f32  axis(PadAxis a) const { return axes[(u32)a]; }
};

// A finger on a touch screen, in framebuffer pixels with the origin at the window's
// top-left (as MouseState). `id` is nonzero while the slot is in use and the same for the
// touch's whole life. A touch that has ended stays one pump with `ended` set, at the place
// it left, so a tap that began and ended between two pumps is still seen once; the next
// pump drops it.
constexpr u32 KV_MAX_TOUCHES = 10;
struct TouchPoint {
    u32  id    = 0;
    f32  x = 0.0f, y = 0.0f;
    bool ended = false;
};

constexpr u32 KV_INPUT_TEXT_MAX = 64; // bytes of typed text one tick carries, the NUL included

// Keys held and pads as they are, and what was typed: UTF-8 text from the OS's text input
// (layouts, dead keys and input methods applied) while a text field asks for it
// (platform_text_input), and the Backspaces among it, auto-repeat included.
struct InputState {
    u64          bits[((u32)Key::Count + 63) / 64] = {};
    GamepadState pads[KV_MAX_GAMEPADS];
    MouseState   mouse;
    TouchPoint   touches[KV_MAX_TOUCHES];
    char         text[KV_INPUT_TEXT_MAX] = {};
    u32          text_len   = 0;
    u32          backspaces = 0;

    // Appends typed text, whole code points only, as far as it fits.
    void type(const char* utf8);

    bool down(Key k) const {
        u32 i = (u32)k;
        return (bits[i >> 6] >> (i & 63)) & 1u;
    }
    void set(Key k, bool on) {
        u32 i = (u32)k;
        u64 m = (u64)1 << (i & 63);
        if (on) bits[i >> 6] |= m; else bits[i >> 6] &= ~m;
    }
    // Releases every key, pad and mouse button, zeroes the axes; pads stay connected and
    // the pointer stays where it was.
    void clear() {
        for (u64& b : bits) b = 0;
        for (GamepadState& p : pads) { p.buttons = 0; for (f32& a : p.axes) a = 0.0f; }
        mouse.buttons = 0;
        mouse.wheel   = 0.0f;
        for (TouchPoint& t : touches) t = TouchPoint{};
        text[0] = 0; text_len = 0; backspaces = 0;
    }
    // Touches: the slot of `id`, or a free one for a new touch (null when all are taken).
    TouchPoint* touch(u32 id) {
        for (TouchPoint& t : touches) if (t.id == id) return &t;
        return nullptr;
    }
    TouchPoint* touch_begin(u32 id, f32 x, f32 y) {
        TouchPoint* t = touch(id);
        if (!t) t = touch(0);
        if (t) *t = TouchPoint{id, x, y, false};
        return t;
    }
    // A pump's start: the touches that ended in the last one are gone.
    void touches_settle() { for (TouchPoint& t : touches) if (t.ended) t = TouchPoint{}; }
    // The first connected pad, or null.
    const GamepadState* pad() const {
        for (const GamepadState& p : pads) if (p.connected) return &p;
        return nullptr;
    }
};

// raw is updated by the window pump at render rate and stays available to
// render-rate code. sampled is copied once per tick for the simulation.
struct Input {
    InputState raw;
    InputState sampled;

    // The wheel and the typing are deltas: the tick that samples them takes them.
    void sample() { sampled = raw; raw.mouse.wheel = 0.0f; raw.text[0] = 0; raw.text_len = 0; raw.backspaces = 0; }
};

// The backend's scancodes (SDL's, or Windows' set-1 codes with 0x100 for the E0-prefixed
// keys) to keys and back. Both directions, for tests and tooling; 0 is no key.
Key key_from_scancode(u32 scancode);
u32 scancode_from_key(Key k);

// Closes every open pad. Module shutdown.
void input_gamepads_close(Input& input);

} // namespace kv
