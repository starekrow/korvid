// Korvid platform_hid on Android: gamepads, from the same input queue as touches and keys.
#pragma once
#include "korvid/platform_hid/input.h"

struct AInputEvent;

namespace kv {

struct EventBus;

// A key or motion event from a gamepad or a joystick: the pad's buttons, sticks, triggers
// and hat. True if it was one (the caller is done with the event), false for anything else.
bool input_android_pad_event(const AInputEvent* ev, EventBus& bus, Input& input);

} // namespace kv
