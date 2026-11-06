// Korvid platform: platform changes and key edges as plain-data events.
// They count as input and are recorded with it.
#pragma once
#include "korvid/platform/input.h"

namespace kv {

struct WindowResized { u32 width, height; };
struct FocusChanged  { bool focused; };
struct CloseRequested { u8 unused; };

struct KeyDown { Key key; u8 device; bool repeat; };
struct KeyUp   { Key key; u8 device; };

} // namespace kv
