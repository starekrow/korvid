// The only place Windows scancodes are known: set 1, as WM_KEYDOWN reports them in bits
// 16-23 of lParam, with 0x100 added for the E0-prefixed keys (bit 24, "extended"). They
// name physical positions, so W A S D are where they are on any layout, as with SDL.
// Gamepads come from SDL3, in src/sdl3_pads/.
#include "korvid/platform_hid/input.h"

namespace kv {

namespace {

struct KeyMap { Key key; u16 sc; };

const KeyMap g_map[] = {
    {Key::A, 0x1E}, {Key::B, 0x30}, {Key::C, 0x2E}, {Key::D, 0x20}, {Key::E, 0x12}, {Key::F, 0x21},
    {Key::G, 0x22}, {Key::H, 0x23}, {Key::I, 0x17}, {Key::J, 0x24}, {Key::K, 0x25}, {Key::L, 0x26},
    {Key::M, 0x32}, {Key::N, 0x31}, {Key::O, 0x18}, {Key::P, 0x19}, {Key::Q, 0x10}, {Key::R, 0x13},
    {Key::S, 0x1F}, {Key::T, 0x14}, {Key::U, 0x16}, {Key::V, 0x2F}, {Key::W, 0x11}, {Key::X, 0x2D},
    {Key::Y, 0x15}, {Key::Z, 0x2C},
    {Key::Num0, 0x0B}, {Key::Num1, 0x02}, {Key::Num2, 0x03}, {Key::Num3, 0x04}, {Key::Num4, 0x05},
    {Key::Num5, 0x06}, {Key::Num6, 0x07}, {Key::Num7, 0x08}, {Key::Num8, 0x09}, {Key::Num9, 0x0A},
    {Key::F1, 0x3B}, {Key::F2, 0x3C}, {Key::F3, 0x3D}, {Key::F4, 0x3E}, {Key::F5, 0x3F}, {Key::F6, 0x40},
    {Key::F7, 0x41}, {Key::F8, 0x42}, {Key::F9, 0x43}, {Key::F10, 0x44}, {Key::F11, 0x57}, {Key::F12, 0x58},
    {Key::Escape, 0x01}, {Key::Space, 0x39}, {Key::Enter, 0x1C}, {Key::Tab, 0x0F}, {Key::Backspace, 0x0E},
    // The arrow keys are E0-prefixed; without the prefix the same codes are the keypad's.
    {Key::Left, 0x14B}, {Key::Right, 0x14D}, {Key::Up, 0x148}, {Key::Down, 0x150},
    {Key::LShift, 0x2A}, {Key::RShift, 0x36}, {Key::LCtrl, 0x1D}, {Key::RCtrl, 0x11D},
    {Key::LAlt, 0x38}, {Key::RAlt, 0x138},
    {Key::LGui, 0x15B}, {Key::RGui, 0x15C}, // the Windows keys: E0 5B, E0 5C
    {Key::Delete, 0x153}, {Key::Home, 0x147}, {Key::End, 0x14F}, {Key::PageUp, 0x149}, {Key::PageDown, 0x151}, // E0-prefixed
};

static_assert(KV_COUNTOF(g_map) == (u32)Key::Count - 1, "every Key except Unknown has a scancode");

constexpr u32 k_codes = 0x200; // set 1 with and without the E0 prefix

} // namespace

Key key_from_scancode(u32 sc) {
    static Key table[k_codes];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < k_codes; ++i) table[i] = Key::Unknown;
        for (const KeyMap& m : g_map) table[m.sc] = m.key;
        built = true;
    }
    return sc < k_codes ? table[sc] : Key::Unknown;
}

u32 scancode_from_key(Key k) {
    for (const KeyMap& m : g_map)
        if (m.key == k) return m.sc;
    return 0;
}

} // namespace kv
