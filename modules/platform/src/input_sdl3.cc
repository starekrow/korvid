// The only place SDL scancodes are known. Both directions, table-driven.
#include "korvid/platform/input.h"
#include <SDL3/SDL_scancode.h>

namespace kv {

struct KeyMap { Key key; SDL_Scancode sc; };

static const KeyMap g_map[] = {
    {Key::A, SDL_SCANCODE_A}, {Key::B, SDL_SCANCODE_B}, {Key::C, SDL_SCANCODE_C},
    {Key::D, SDL_SCANCODE_D}, {Key::E, SDL_SCANCODE_E}, {Key::F, SDL_SCANCODE_F},
    {Key::G, SDL_SCANCODE_G}, {Key::H, SDL_SCANCODE_H}, {Key::I, SDL_SCANCODE_I},
    {Key::J, SDL_SCANCODE_J}, {Key::K, SDL_SCANCODE_K}, {Key::L, SDL_SCANCODE_L},
    {Key::M, SDL_SCANCODE_M}, {Key::N, SDL_SCANCODE_N}, {Key::O, SDL_SCANCODE_O},
    {Key::P, SDL_SCANCODE_P}, {Key::Q, SDL_SCANCODE_Q}, {Key::R, SDL_SCANCODE_R},
    {Key::S, SDL_SCANCODE_S}, {Key::T, SDL_SCANCODE_T}, {Key::U, SDL_SCANCODE_U},
    {Key::V, SDL_SCANCODE_V}, {Key::W, SDL_SCANCODE_W}, {Key::X, SDL_SCANCODE_X},
    {Key::Y, SDL_SCANCODE_Y}, {Key::Z, SDL_SCANCODE_Z},
    {Key::Num0, SDL_SCANCODE_0}, {Key::Num1, SDL_SCANCODE_1}, {Key::Num2, SDL_SCANCODE_2},
    {Key::Num3, SDL_SCANCODE_3}, {Key::Num4, SDL_SCANCODE_4}, {Key::Num5, SDL_SCANCODE_5},
    {Key::Num6, SDL_SCANCODE_6}, {Key::Num7, SDL_SCANCODE_7}, {Key::Num8, SDL_SCANCODE_8},
    {Key::Num9, SDL_SCANCODE_9},
    {Key::F1, SDL_SCANCODE_F1}, {Key::F2, SDL_SCANCODE_F2}, {Key::F3, SDL_SCANCODE_F3},
    {Key::F4, SDL_SCANCODE_F4}, {Key::F5, SDL_SCANCODE_F5}, {Key::F6, SDL_SCANCODE_F6},
    {Key::F7, SDL_SCANCODE_F7}, {Key::F8, SDL_SCANCODE_F8}, {Key::F9, SDL_SCANCODE_F9},
    {Key::F10, SDL_SCANCODE_F10}, {Key::F11, SDL_SCANCODE_F11}, {Key::F12, SDL_SCANCODE_F12},
    {Key::Escape, SDL_SCANCODE_ESCAPE}, {Key::Space, SDL_SCANCODE_SPACE},
    {Key::Enter, SDL_SCANCODE_RETURN}, {Key::Tab, SDL_SCANCODE_TAB},
    {Key::Backspace, SDL_SCANCODE_BACKSPACE},
    {Key::Left, SDL_SCANCODE_LEFT}, {Key::Right, SDL_SCANCODE_RIGHT},
    {Key::Up, SDL_SCANCODE_UP}, {Key::Down, SDL_SCANCODE_DOWN},
    {Key::LShift, SDL_SCANCODE_LSHIFT}, {Key::RShift, SDL_SCANCODE_RSHIFT},
    {Key::LCtrl, SDL_SCANCODE_LCTRL}, {Key::RCtrl, SDL_SCANCODE_RCTRL},
    {Key::LAlt, SDL_SCANCODE_LALT}, {Key::RAlt, SDL_SCANCODE_RALT},
};

static_assert(KV_COUNTOF(g_map) == (u32)Key::Count - 1, "every Key except Unknown has a scancode");

Key key_from_scancode(u32 sc) {
    // A dense table built on first use keeps lookup O(1) for the hot path.
    static Key table[SDL_SCANCODE_COUNT];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < SDL_SCANCODE_COUNT; ++i) table[i] = Key::Unknown;
        for (const KeyMap& m : g_map) table[m.sc] = m.key;
        built = true;
    }
    return sc < SDL_SCANCODE_COUNT ? table[sc] : Key::Unknown;
}

u32 scancode_from_key(Key k) {
    for (const KeyMap& m : g_map)
        if (m.key == k) return (u32)m.sc;
    return SDL_SCANCODE_UNKNOWN;
}

} // namespace kv
