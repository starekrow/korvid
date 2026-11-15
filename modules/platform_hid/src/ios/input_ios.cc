// The only place iOS key codes are known: a hardware keyboard's USB HID usages (page 7), as
// UIKit's UIKey.keyCode reports them, which are the numbers SDL's scancodes use as well. They
// name physical positions, so W A S D are where they are on any layout. No gamepads yet:
// the GameController framework is slice 8's, with touch.
#include "korvid/platform_hid/input.h"

namespace kv {

namespace {

struct KeyMap { Key key; u16 usage; };

const KeyMap g_map[] = {
    {Key::A, 4}, {Key::B, 5}, {Key::C, 6}, {Key::D, 7}, {Key::E, 8}, {Key::F, 9}, {Key::G, 10},
    {Key::H, 11}, {Key::I, 12}, {Key::J, 13}, {Key::K, 14}, {Key::L, 15}, {Key::M, 16}, {Key::N, 17},
    {Key::O, 18}, {Key::P, 19}, {Key::Q, 20}, {Key::R, 21}, {Key::S, 22}, {Key::T, 23}, {Key::U, 24},
    {Key::V, 25}, {Key::W, 26}, {Key::X, 27}, {Key::Y, 28}, {Key::Z, 29},
    {Key::Num1, 30}, {Key::Num2, 31}, {Key::Num3, 32}, {Key::Num4, 33}, {Key::Num5, 34},
    {Key::Num6, 35}, {Key::Num7, 36}, {Key::Num8, 37}, {Key::Num9, 38}, {Key::Num0, 39},
    {Key::F1, 58}, {Key::F2, 59}, {Key::F3, 60}, {Key::F4, 61}, {Key::F5, 62}, {Key::F6, 63},
    {Key::F7, 64}, {Key::F8, 65}, {Key::F9, 66}, {Key::F10, 67}, {Key::F11, 68}, {Key::F12, 69},
    {Key::Escape, 41}, {Key::Space, 44}, {Key::Enter, 40}, {Key::Tab, 43}, {Key::Backspace, 42},
    {Key::Left, 80}, {Key::Right, 79}, {Key::Up, 82}, {Key::Down, 81},
    {Key::LShift, 225}, {Key::RShift, 229}, {Key::LCtrl, 224}, {Key::RCtrl, 228},
    {Key::LAlt, 226}, {Key::RAlt, 230},
    {Key::LGui, 227}, {Key::RGui, 231},
    {Key::Delete, 76}, {Key::Home, 74}, {Key::End, 77}, {Key::PageUp, 75}, {Key::PageDown, 78}, // HID usages
};

static_assert(KV_COUNTOF(g_map) == (u32)Key::Count - 1, "every Key except Unknown has a usage");

constexpr u32 k_usages = 256;

} // namespace

Key key_from_scancode(u32 sc) {
    static Key table[k_usages];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < k_usages; ++i) table[i] = Key::Unknown;
        for (const KeyMap& m : g_map) table[m.usage] = m.key;
        built = true;
    }
    return sc < k_usages ? table[sc] : Key::Unknown;
}

u32 scancode_from_key(Key k) {
    for (const KeyMap& m : g_map)
        if (m.key == k) return m.usage;
    return 0;
}

void input_gamepads_close(Input&) {}

} // namespace kv
