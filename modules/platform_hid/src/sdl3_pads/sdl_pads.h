// Korvid platform_hid: gamepads from SDL3, on every platform. Linux's SDL window pump hands
// this the events it does not handle; a backend with a window of its own (Win32) brings up
// SDL's gamepad subsystem alone and has its pump poll SDL's queue here.
#pragma once
#include "korvid/platform_hid/input.h"

namespace kv {

// Gamepad events from the SDL queue. Returns false if the event was not a gamepad's. Opens
// and closes pads on arrival and removal, updates raw, and emits the gamepad events.
bool input_gamepad_event(const void* sdl_event, EventBus& bus, Input& input);

// SDL's gamepad subsystem and nothing else of SDL, once; false (logged) if SDL refuses.
bool sdl_pads_up();
void sdl_pads_down();
// Every event in SDL's queue, gamepads handled: once per frame, from the window's pump.
void sdl_pads_pump(EventBus& bus, Input& input);

} // namespace kv
