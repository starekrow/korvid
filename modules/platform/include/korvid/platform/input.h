// Korvid platform: the input layer. Keyboard is the first device; a second device
// type adds its own event structs and state block without changing this API.
// Key codes are engine-defined and mapped from SDL scancodes in the SDL3 source file.
#pragma once
#include "korvid/core/types.h"

namespace kv {

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

// Bit set of keys currently held.
struct InputState {
    u64 bits[((u32)Key::Count + 63) / 64] = {};

    bool down(Key k) const {
        u32 i = (u32)k;
        return (bits[i >> 6] >> (i & 63)) & 1u;
    }
    void set(Key k, bool on) {
        u32 i = (u32)k;
        u64 m = (u64)1 << (i & 63);
        if (on) bits[i >> 6] |= m; else bits[i >> 6] &= ~m;
    }
    void clear() { for (u64& b : bits) b = 0; }
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

} // namespace kv
