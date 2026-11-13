// The only place Android key codes are known: AKEYCODE_* (android/keycodes.h), as a key
// event from a hardware keyboard (or the emulator's host keyboard) reports them. They name
// the key's meaning on the device's layout rather than its position; a phone's game is
// played by touch, and a keyboard there is a developer's. Back is Escape: every game
// already takes Escape as "close this, or go up a menu". No gamepads yet (milestone 6).
#include "korvid/platform_hid/input.h"
#include <android/keycodes.h>

namespace kv {

namespace {

struct KeyMap { Key key; u16 code; };

const KeyMap g_map[] = {
    {Key::A, AKEYCODE_A}, {Key::B, AKEYCODE_B}, {Key::C, AKEYCODE_C}, {Key::D, AKEYCODE_D}, {Key::E, AKEYCODE_E},
    {Key::F, AKEYCODE_F}, {Key::G, AKEYCODE_G}, {Key::H, AKEYCODE_H}, {Key::I, AKEYCODE_I}, {Key::J, AKEYCODE_J},
    {Key::K, AKEYCODE_K}, {Key::L, AKEYCODE_L}, {Key::M, AKEYCODE_M}, {Key::N, AKEYCODE_N}, {Key::O, AKEYCODE_O},
    {Key::P, AKEYCODE_P}, {Key::Q, AKEYCODE_Q}, {Key::R, AKEYCODE_R}, {Key::S, AKEYCODE_S}, {Key::T, AKEYCODE_T},
    {Key::U, AKEYCODE_U}, {Key::V, AKEYCODE_V}, {Key::W, AKEYCODE_W}, {Key::X, AKEYCODE_X}, {Key::Y, AKEYCODE_Y},
    {Key::Z, AKEYCODE_Z},
    {Key::Num0, AKEYCODE_0}, {Key::Num1, AKEYCODE_1}, {Key::Num2, AKEYCODE_2}, {Key::Num3, AKEYCODE_3}, {Key::Num4, AKEYCODE_4},
    {Key::Num5, AKEYCODE_5}, {Key::Num6, AKEYCODE_6}, {Key::Num7, AKEYCODE_7}, {Key::Num8, AKEYCODE_8}, {Key::Num9, AKEYCODE_9},
    {Key::F1, AKEYCODE_F1}, {Key::F2, AKEYCODE_F2}, {Key::F3, AKEYCODE_F3}, {Key::F4, AKEYCODE_F4}, {Key::F5, AKEYCODE_F5},
    {Key::F6, AKEYCODE_F6}, {Key::F7, AKEYCODE_F7}, {Key::F8, AKEYCODE_F8}, {Key::F9, AKEYCODE_F9}, {Key::F10, AKEYCODE_F10},
    {Key::F11, AKEYCODE_F11}, {Key::F12, AKEYCODE_F12},
    {Key::Escape, AKEYCODE_ESCAPE}, {Key::Space, AKEYCODE_SPACE}, {Key::Enter, AKEYCODE_ENTER}, {Key::Tab, AKEYCODE_TAB},
    {Key::Backspace, AKEYCODE_DEL},
    {Key::Left, AKEYCODE_DPAD_LEFT}, {Key::Right, AKEYCODE_DPAD_RIGHT}, {Key::Up, AKEYCODE_DPAD_UP}, {Key::Down, AKEYCODE_DPAD_DOWN},
    {Key::LShift, AKEYCODE_SHIFT_LEFT}, {Key::RShift, AKEYCODE_SHIFT_RIGHT}, {Key::LCtrl, AKEYCODE_CTRL_LEFT}, {Key::RCtrl, AKEYCODE_CTRL_RIGHT},
    {Key::LAlt, AKEYCODE_ALT_LEFT}, {Key::RAlt, AKEYCODE_ALT_RIGHT},
    {Key::LGui, AKEYCODE_META_LEFT}, {Key::RGui, AKEYCODE_META_RIGHT},
};

static_assert(KV_COUNTOF(g_map) == (u32)Key::Count - 1, "every Key except Unknown has a key code");

constexpr u32 k_codes = 320;

} // namespace

Key key_from_scancode(u32 sc) {
    static Key table[k_codes];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < k_codes; ++i) table[i] = Key::Unknown;
        for (const KeyMap& m : g_map) table[m.code] = m.key;
        table[AKEYCODE_BACK] = Key::Escape; // the Back button and gesture
        table[AKEYCODE_NUMPAD_ENTER] = Key::Enter;
        built = true;
    }
    return sc < k_codes ? table[sc] : Key::Unknown;
}

u32 scancode_from_key(Key k) {
    for (const KeyMap& m : g_map)
        if (m.key == k) return m.code;
    return 0;
}

void input_gamepads_close(Input&) {}

} // namespace kv
