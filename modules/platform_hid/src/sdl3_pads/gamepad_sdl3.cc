// Gamepads from SDL3, the same on every platform: SDL's standard layout in PadButton order,
// pads opened on arrival into the first free slot of four, sticks with a dead zone and
// triggers in [0, 1].
#include "korvid/platform_hid/events.h"
#include "sdl_pads.h"
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

bool sdl_pads_up() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) return true;
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    // SDL has no window of its own here to know whether the game has focus, so it reports
    // pads always; the window's backend decides what focus means (raw input clears on loss).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        KV_WARN("input", "SDL gamepads unavailable: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("input", "gamepads from SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver));
    return true;
}

void sdl_pads_down() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (SDL_WasInit(0) == 0) SDL_Quit(); // the last SDL user out
}

void sdl_pads_pump(EventBus& bus, Input& input) {
    if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) input_gamepad_event(&e, bus, input);
}

} // namespace kv
