// Korvid platform: platform changes, key edges and gamepad edges as plain-data events.
// They count as input and are recorded with it.
#pragma once
#include "korvid/platform_hid/input.h"

namespace kv {

struct WindowResized { u32 width, height; };
struct FocusChanged  { bool focused; };
struct CloseRequested { u8 unused; };
// The window stopped being seen, or is seen again (KR21): emitted when it changes, and
// readable between as HostWindow::visible(). As well as each OS can tell: minimized
// everywhere; covered, on another workspace or behind a locked screen where the OS or the
// compositor says so (a suspended Wayland surface, macOS's occlusion). Where it cannot be
// told the window is shown. On a phone the loop stops while the app is in the background:
// the hidden and shown events arrive together when it comes back.
enum class WindowHidden : u8 { None, Minimized, Occluded };
struct WindowVisibility { bool visible; WindowHidden why; };

struct KeyDown { Key key; u8 device; bool repeat; };
struct KeyUp   { Key key; u8 device; };

struct MouseMoved      { f32 x, y, dx, dy; };     // framebuffer pixels, origin top-left
struct MouseButtonDown { MouseButton button; f32 x, y; };
struct MouseButtonUp   { MouseButton button; f32 x, y; };
struct MouseWheel      { f32 dx, dy; };           // dy positive away from the user

// Text from the OS's text input (layouts, dead keys, an input method's commit), UTF-8, while
// a text field asks for it (platform_text_input). A long commit comes as several, split at
// code points. Events, so a reader in the Frame cycle sees each once however frames and
// ticks fall; InputState::text has the same for a tick's reader.
constexpr u32 KV_TEXT_TYPED = 32;
struct TextTyped { char text[KV_TEXT_TYPED]; };
// What an input method is composing, before it commits: drawn by the field at its caret and
// not yet part of it. `caret` and `selection` are byte offsets into `text` (selection 0: none).
// An empty text ends the composition (committed as TextTyped, or abandoned).
constexpr u32 KV_TEXT_COMPOSING = 64;
struct TextComposing { char text[KV_TEXT_COMPOSING]; u32 caret; u32 selection; };

struct GamepadConnected    { u8 pad; };           // index into InputState::pads
struct GamepadDisconnected { u8 pad; };
struct PadButtonDown { PadButton button; u8 pad; };
struct PadButtonUp   { PadButton button; u8 pad; };

} // namespace kv
