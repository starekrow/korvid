// Korvid platform: platform changes, key edges and gamepad edges as plain-data events.
// They count as input and are recorded with it.
#pragma once
#include "korvid/platform_hid/input.h"

namespace kv {

struct WindowResized { u32 width, height; };
struct FocusChanged  { bool focused; };
struct CloseRequested { u8 unused; };

struct KeyDown { Key key; u8 device; bool repeat; };
struct KeyUp   { Key key; u8 device; };

struct MouseMoved      { f32 x, y, dx, dy; };     // framebuffer pixels, origin top-left
struct MouseButtonDown { MouseButton button; f32 x, y; };
struct MouseButtonUp   { MouseButton button; f32 x, y; };
struct MouseWheel      { f32 dx, dy; };           // dy positive away from the user

struct GamepadConnected    { u8 pad; };           // index into InputState::pads
struct GamepadDisconnected { u8 pad; };
struct PadButtonDown { PadButton button; u8 pad; };
struct PadButtonUp   { PadButton button; u8 pad; };

} // namespace kv
