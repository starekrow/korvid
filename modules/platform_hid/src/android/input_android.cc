// The only place Android key codes are known: AKEYCODE_* (android/keycodes.h), as a key
// event from a hardware keyboard (or the emulator's host keyboard) reports them. They name
// the key's meaning on the device's layout rather than its position; a phone's game is
// played by touch, and a keyboard there is a developer's. Back is Escape: every game
// already takes Escape as "close this, or go up a menu".
//
// Gamepads need no library: a pad's buttons are key events whose source is a gamepad
// (AKEYCODE_BUTTON_*), its sticks, triggers and hat motion events whose source is a joystick
// (AMOTION_EVENT_AXIS_*). A pad takes a slot the first time its device id is heard from.
// Android tells native code nothing when a pad goes away (that is InputManager's, in Java),
// so a pad stays connected, at rest, once seen. Written against the documentation and
// checked with `adb shell input gamepad keyevent`; the emulator passes no real pad through,
// so sticks and triggers wait for a device.
#include "korvid/platform_hid/input.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/event.h"
#include "korvid/core/log.h"
#include "android_pads.h"
#include <android/input.h>
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
    {Key::Delete, AKEYCODE_FORWARD_DEL}, {Key::Home, AKEYCODE_MOVE_HOME}, {Key::End, AKEYCODE_MOVE_END},
    {Key::PageUp, AKEYCODE_PAGE_UP}, {Key::PageDown, AKEYCODE_PAGE_DOWN},
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

namespace {

struct PadKey { PadButton button; i32 code; };

const PadKey g_pad_keys[] = {
    {PadButton::South, AKEYCODE_BUTTON_A}, {PadButton::East, AKEYCODE_BUTTON_B}, {PadButton::West, AKEYCODE_BUTTON_X}, {PadButton::North, AKEYCODE_BUTTON_Y},
    {PadButton::Back, AKEYCODE_BUTTON_SELECT}, {PadButton::Guide, AKEYCODE_BUTTON_MODE}, {PadButton::Start, AKEYCODE_BUTTON_START},
    {PadButton::LStick, AKEYCODE_BUTTON_THUMBL}, {PadButton::RStick, AKEYCODE_BUTTON_THUMBR},
    {PadButton::LShoulder, AKEYCODE_BUTTON_L1}, {PadButton::RShoulder, AKEYCODE_BUTTON_R1},
    {PadButton::DpadUp, AKEYCODE_DPAD_UP}, {PadButton::DpadDown, AKEYCODE_DPAD_DOWN},
    {PadButton::DpadLeft, AKEYCODE_DPAD_LEFT}, {PadButton::DpadRight, AKEYCODE_DPAD_RIGHT},
};

// The slot of a device id, taking a free one (and saying so) the first time it is heard from.
int pad_slot(EventBus& bus, InputState& s, i32 device) {
    u32 id = (u32)device + 1; // a device id may be 0
    for (u32 i = 0; i < KV_MAX_GAMEPADS; ++i) if (s.pads[i].connected && s.pads[i].id == id) return (int)i;
    for (u32 i = 0; i < KV_MAX_GAMEPADS; ++i) {
        if (s.pads[i].connected) continue;
        s.pads[i] = GamepadState{};
        s.pads[i].connected = true;
        s.pads[i].id = id;
        KV_INFO("input", "gamepad %u: Android input device %d", i, (int)device);
        bus.emit(GamepadConnected{(u8)i});
        return (int)i;
    }
    return -1;
}

void pad_button(EventBus& bus, GamepadState& p, u8 slot, PadButton b, bool down) {
    if (p.down(b) == down) return;
    p.set(b, down);
    if (down) bus.emit(PadButtonDown{b, slot});
    else      bus.emit(PadButtonUp{b, slot});
}

f32 stick(f32 v) { return v > -KV_PAD_DEAD_ZONE && v < KV_PAD_DEAD_ZONE ? 0.0f : v; }

} // namespace

bool input_android_pad_event(const AInputEvent* ev, EventBus& bus, Input& input) {
    i32 source = AInputEvent_getSource(ev);
    i32 type = AInputEvent_getType(ev);
    InputState& s = input.raw;
    if (type == AINPUT_EVENT_TYPE_KEY && (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) {
        i32 code = AKeyEvent_getKeyCode(ev);
        i32 action = AKeyEvent_getAction(ev);
        for (const PadKey& k : g_pad_keys) {
            if (k.code != code) continue;
            int slot = pad_slot(bus, s, AInputEvent_getDeviceId(ev));
            if (slot >= 0 && (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP))
                pad_button(bus, s.pads[slot], (u8)slot, k.button, action == AKEY_EVENT_ACTION_DOWN);
            return true;
        }
        return false; // a key the pad has that is not a pad button (Back on some): the keyboard's table
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        if ((AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK) != AMOTION_EVENT_ACTION_MOVE) return true;
        int slot = pad_slot(bus, s, AInputEvent_getDeviceId(ev));
        if (slot < 0) return true;
        GamepadState& p = s.pads[slot];
        auto axis = [&](i32 a) { return AMotionEvent_getAxisValue(ev, a, 0); };
        p.axes[(u32)PadAxis::LeftX]  = stick(axis(AMOTION_EVENT_AXIS_X));
        p.axes[(u32)PadAxis::LeftY]  = stick(axis(AMOTION_EVENT_AXIS_Y));
        p.axes[(u32)PadAxis::RightX] = stick(axis(AMOTION_EVENT_AXIS_Z));
        p.axes[(u32)PadAxis::RightY] = stick(axis(AMOTION_EVENT_AXIS_RZ));
        // A trigger is LTRIGGER on some pads and BRAKE on others (GAS for the right): the larger.
        f32 lt = axis(AMOTION_EVENT_AXIS_LTRIGGER), lb = axis(AMOTION_EVENT_AXIS_BRAKE);
        f32 rt = axis(AMOTION_EVENT_AXIS_RTRIGGER), rg = axis(AMOTION_EVENT_AXIS_GAS);
        p.axes[(u32)PadAxis::LeftTrigger]  = lt > lb ? lt : lb;
        p.axes[(u32)PadAxis::RightTrigger] = rt > rg ? rt : rg;
        // The hat is two axes of -1, 0 or 1: the four dpad buttons.
        f32 hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        pad_button(bus, p, (u8)slot, PadButton::DpadLeft,  hx < -0.5f);
        pad_button(bus, p, (u8)slot, PadButton::DpadRight, hx > 0.5f);
        pad_button(bus, p, (u8)slot, PadButton::DpadUp,    hy < -0.5f);
        pad_button(bus, p, (u8)slot, PadButton::DpadDown,  hy > 0.5f);
        return true;
    }
    return false;
}

void input_gamepads_close(Input& input) {
    for (GamepadState& p : input.raw.pads) p = GamepadState{};
}

} // namespace kv
