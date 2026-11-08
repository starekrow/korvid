// The only place SDL scancodes are known. Both directions, table-driven.
#include "korvid/platform_hid/input.h"
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

// ---- gamepads -------------------------------------------------------------------------------
#include "korvid/platform_hid/events.h"
#include "korvid/core/log.h"
#include <SDL3/SDL.h>

namespace kv {

namespace {

// SDL's standard layout in PadButton order.
static const SDL_GamepadButton g_pad_buttons[(u32)PadButton::Count] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};
static const SDL_GamepadAxis g_pad_axes[(u32)PadAxis::Count] = {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

int pad_button_index(u8 sdl_button) {
    for (u32 i = 0; i < (u32)PadButton::Count; ++i) if (g_pad_buttons[i] == (SDL_GamepadButton)sdl_button) return (int)i;
    return -1;
}
int pad_axis_index(u8 sdl_axis) {
    for (u32 i = 0; i < (u32)PadAxis::Count; ++i) if (g_pad_axes[i] == (SDL_GamepadAxis)sdl_axis) return (int)i;
    return -1;
}
int pad_slot_of(const InputState& s, u32 id) {
    for (u32 i = 0; i < KV_MAX_GAMEPADS; ++i) if (s.pads[i].connected && s.pads[i].id == id) return (int)i;
    return -1;
}

f32 axis_value(i16 raw, bool trigger) {
    f32 v = raw < 0 ? (f32)raw / 32768.0f : (f32)raw / 32767.0f;
    if (trigger) return v < 0.0f ? 0.0f : v;
    return v > -KV_PAD_DEAD_ZONE && v < KV_PAD_DEAD_ZONE ? 0.0f : v;
}

} // namespace

bool input_gamepad_event(const void* sdl_event, EventBus& bus, Input& input) {
    const SDL_Event& e = *(const SDL_Event*)sdl_event;
    InputState& s = input.raw;
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        if (pad_slot_of(s, (u32)e.gdevice.which) >= 0) return true; // already open
        int slot = -1;
        for (u32 i = 0; i < KV_MAX_GAMEPADS && slot < 0; ++i) if (!s.pads[i].connected) slot = (int)i;
        if (slot < 0) { KV_WARN("input", "gamepad %u ignored: %u pads are open", (u32)e.gdevice.which, KV_MAX_GAMEPADS); return true; }
        SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which);
        if (!pad) { KV_WARN("input", "SDL_OpenGamepad: %s", SDL_GetError()); return true; }
        GamepadState& p = s.pads[slot];
        p = GamepadState{};
        p.connected = true;
        p.id        = (u32)e.gdevice.which;
        p.native    = pad;
        const char* name = SDL_GetGamepadName(pad);
        KV_INFO("input", "gamepad %d: '%s'", slot, name ? name : "?");
        bus.emit(GamepadConnected{(u8)slot});
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int slot = pad_slot_of(s, (u32)e.gdevice.which);
        if (slot < 0) return true;
        GamepadState& p = s.pads[slot];
        if (p.native) SDL_CloseGamepad((SDL_Gamepad*)p.native);
        p = GamepadState{};
        input.sampled.pads[slot].connected = false; // the sim sees it gone at its next sample anyway
        KV_INFO("input", "gamepad %d removed", slot);
        bus.emit(GamepadDisconnected{(u8)slot});
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int slot = pad_slot_of(s, (u32)e.gbutton.which);
        int b = pad_button_index(e.gbutton.button);
        if (slot < 0 || b < 0) return true;
        bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        s.pads[slot].set((PadButton)b, down);
        if (down) bus.emit(PadButtonDown{(PadButton)b, (u8)slot});
        else      bus.emit(PadButtonUp{(PadButton)b, (u8)slot});
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int slot = pad_slot_of(s, (u32)e.gaxis.which);
        int a = pad_axis_index(e.gaxis.axis);
        if (slot < 0 || a < 0) return true;
        s.pads[slot].axes[a] = axis_value(e.gaxis.value, a >= (int)PadAxis::LeftTrigger);
        return true;
    }
    default:
        return false;
    }
}

void input_gamepads_close(Input& input) {
    for (GamepadState& p : input.raw.pads) {
        if (p.native) SDL_CloseGamepad((SDL_Gamepad*)p.native);
        p = GamepadState{};
    }
    for (GamepadState& p : input.sampled.pads) p = GamepadState{};
}

} // namespace kv
