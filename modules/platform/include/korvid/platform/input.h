// Korvid platform: the input layer. Keyboard and gamepads; a further device type adds its
// own event structs and state block without changing this API. Key codes are
// engine-defined and mapped from SDL scancodes in the SDL3 source file; gamepad buttons
// and axes follow SDL3's standard layout (a Playstation or Xbox pad looks the same here).
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
    LShift, RShift, LCtrl, RCtrl, LAlt, RAlt,
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

constexpr u32 KV_MAX_GAMEPADS   = 4;
constexpr f32 KV_PAD_DEAD_ZONE  = 0.15f; // stick axes inside it read 0

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

// Keys held and pads as they are.
struct InputState {
    u64          bits[((u32)Key::Count + 63) / 64] = {};
    GamepadState pads[KV_MAX_GAMEPADS];

    bool down(Key k) const {
        u32 i = (u32)k;
        return (bits[i >> 6] >> (i & 63)) & 1u;
    }
    void set(Key k, bool on) {
        u32 i = (u32)k;
        u64 m = (u64)1 << (i & 63);
        if (on) bits[i >> 6] |= m; else bits[i >> 6] &= ~m;
    }
    // Releases every key and pad button, zeroes the axes; pads stay connected.
    void clear() {
        for (u64& b : bits) b = 0;
        for (GamepadState& p : pads) { p.buttons = 0; for (f32& a : p.axes) a = 0.0f; }
    }
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

    void sample() { sampled = raw; }
};

// Scancode mapping lives in the SDL3 source file; both directions, for tests and tooling.
Key key_from_scancode(u32 sdl_scancode);
u32 scancode_from_key(Key k);

// Gamepad events from the SDL queue. The window pump hands over what it does not handle;
// returns false if the event was not a gamepad's. Opens and closes pads on arrival and
// removal, updates raw, and emits the gamepad events on the bus.
bool input_gamepad_event(const void* sdl_event, EventBus& bus, Input& input);
// Closes every open pad. Module shutdown.
void input_gamepads_close(Input& input);

} // namespace kv
