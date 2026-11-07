// Korvid platform: platform changes, key edges and gamepad edges as plain-data events.
// They count as input and are recorded with it.
#pragma once
#include "korvid/platform/input.h"

namespace kv {

struct WindowResized { u32 width, height; };
struct FocusChanged  { bool focused; };
struct CloseRequested { u8 unused; };

struct KeyDown { Key key; u8 device; bool repeat; };
struct KeyUp   { Key key; u8 device; };

struct GamepadConnected    { u8 pad; };           // index into InputState::pads
struct GamepadDisconnected { u8 pad; };
struct PadButtonDown { PadButton button; u8 pad; };
struct PadButtonUp   { PadButton button; u8 pad; };

} // namespace kv
