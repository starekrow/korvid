# platform_hid

What a person at the machine touches and looks at: the window, the keyboard, the mouse,
gamepads, and the input action map over them. The window and keyboard backend is per
platform, picked by the toolchain's source directories: SDL3 on Linux and macOS
(`src/sdl3/`), native Win32 on Windows (`src/win32/`), UIKit on iOS (`src/ios/`), a
`NativeActivity` on Android (`src/android/`), behind `src/hid_backend.h`. Gamepads come from
SDL3 on the desktop (`src/sdl3_pads/`) and from Android's own input events there. Nothing outside this module and
`platform_audio` sees either, so a port adds a backend, not an API. The module starts
headless: the display and gamepads come up with the first window.

On Windows the window has a thread of its own, so the system's modal move and size loops
block that thread and never the frame loop; it is per-monitor DPI aware (`WindowDesc`
sizes are logical, the framebuffer and the pointer physical); Alt and F10 open no menu;
Alt+F4 asks to close. Gamepads use SDL's gamepad subsystem alone, polled by the window's pump.
`window` shows the frame loop's gaps while the window is dragged, on any backend.

## How it works

**The window** (`host_window.h`). One per process, opened on request by the frameloop
module and closed at module shutdown, never by the caller. It is a service, not an
entity. `platform_pump`, through the pump hook this module adds, turns OS events into bus
events (`WindowResized`, `FocusChanged`, `CloseRequested`, key, mouse and gamepad edges)
and updates the raw input state. A Vulkan window hands its native handle to the renderer; a software window can
only `present_blank`.

**Input** (`input.h`, `events.h`). Two copies of the state: `raw`, updated by the pump at
frame rate, and `sampled`, copied once per tick by the system `input.sample` (phase
Input) so the simulation sees one consistent picture per tick. Keys are engine codes
mapped from SDL scancodes; pads follow SDL's standard layout (South is A or Cross), with
a dead zone on the sticks and triggers in [0, 1]. Pads open when they arrive and close
when they leave; up to four. Losing focus releases every key and button so nothing stays
pressed.

**Actions.** Games bind what the player can do to devices as data and ask about actions,
never keys (`korvid/platform_hid/actions.h`): `input_actions(e, {{"move", ActionKind::Axis2,
"keys W S A D, stick left, dpad"}, {"confirm", ActionKind::Button, "key Enter, pad South"}})`
at startup; a system that reads the resource `input.actions` asks `action_pressed`,
`action_down`, `action_released`, `action_axis` (+y up, clamped to the unit circle, a stick
rescaled past its 0.2 dead zone, the strongest binding winning) and `action_repeat(a, id,
delay, interval)` in ticks for menus. They are computed in `input.sample` from the sampled
input on every tick, paused or not, so they are as deterministic as the input. A demo
drives them with `input_action_override(e, name, down, axis)`, which replaces the devices
for one sample; `input_action_override_exact` takes the axis as it is, unclamped (a
replay's, which a sample has clamped once already). The CLI's `press ACTION [TICKS] [X Y]` holds an action for that many sampled ticks (six
by default), and `type TEXT` types (`\n` ends a line), so a script over `--cli-stdin` can
work menus and fields as a player would.

## Using it

**Declare the dependency**: `deps = { "core", "platform", "platform_hid", ... }` in
`module.lua`. The frameloop module depends on it, so a game gets it for free; a tool that
opens no window leaves it out and links no SDL.

**Read input in a system.** Name the resource in the system's sets and reach it through
the context:

```cpp
void control(TickContext& ctx) {
    static const u32 k_input = ctx.resource_id("input.state");
    const Input* in = ctx.resource<const Input>(k_input);
    bool left = in->sampled.down(Key::Left);
    if (const GamepadState* pad = in->sampled.pad()) {        // the first connected pad, or null
        left |= pad->axis(PadAxis::LeftX) < -0.5f || pad->down(PadButton::DpadLeft);
    }
}
```

Key edges arrive as events too: `for (const KeyDown& k : ctx.events->read<KeyDown>())`.
The window is `ctx.resource<const HostWindow>("platform_hid.window")` for its size.

**Options the frame loop exposes**: `--borderless`, `--no-vsync`.

## Typed text

Keys are positions; text is what a person meant to type. `InputState::text` carries the
UTF-8 the OS produced since the last sample (SDL3's text input events on Linux, with
layouts, dead keys and input methods applied; `WM_CHAR` on Windows, surrogate pairs joined),
and `backspaces` the Backspaces among it, auto-repeat included. A text field asks for it with
`platform_text_input(e, true)` (counted, so two fields need two offs); SDL then shows an
input method's window where there is one. `input_text_override(e, "Rowan\n")` types into the
next sample, for demos and tests. `input_pointer_override(e, x, y, buttons, wheel)` puts the pointer somewhere with these
buttons held, as the mouse would, for the next sample: Krome's demo clicks through a form with it.

**As events too** (Krome slice 3). Each commit is a `TextTyped` (UTF-8, split at code
points into 31-byte pieces), and what an input method is composing, before it commits, a
`TextComposing` (its text, caret and selection in bytes; empty when it ends), so a reader in
the Frame cycle sees each once however frames and ticks fall. Every host types through
`input_typed`, which keeps `InputState::text` too. SDL3 turns `TEXT_EDITING` into
`TextComposing`, and Win32 its IME's `WM_IME_COMPOSITION` (the result string arrives as
typed characters). `input_key_override(e, key, down)` and `input_compose_override(e, text,
caret)` drive keys and an input method as the text override drives typing.

**What a text field asks of the platform.** `platform_text_area(e, rect, caret)` tells the
input method where the caret is (its candidates open there; tell it only when it moves: an
input method answers each telling with its state). `platform_clipboard_get` and `_set` read
and write the system clipboard, and keep the process's own, which is what a headless run or
a host not yet filled in gets back. `platform_cursor(e, CursorShape)` sets the pointer's
shape. SDL3 (Linux, macOS) and Win32 do all of it (Win32 reads and writes the clipboard's
CRLFs as LFs, and gives the window its IME only while text input is on); iOS and Android
have stubs.

## Quick reference

Header `korvid/platform_hid/host_window.h`:

| name | kind | what |
|---|---|---|
| `WindowDesc` | record | `title`, `width`, `height`, `vsync` (a preference the renderer reads), `surface` (`Software`, `Vulkan`), `borderless` |
| `HostWindow` | record | `open(desc)`, `close()`, `is_open()`, `width()`, `height()`, `vsync()`, `focused()`, `pump(bus, input)`, `present_blank()`, `native_handle()` |

Header `korvid/platform_hid/input.h`:

| name | kind | what |
|---|---|---|
| `Key` | enum | letters, digits, F keys, `Escape`, `Space`, `Enter`, `Tab`, `Backspace`, arrows, modifiers; `key_name` |
| `PadButton`, `PadAxis` | enums | the standard layout; `pad_button_name`, `pad_axis_name` |
| `GamepadState` | record | `buttons` (bits), `axes`, `connected`, `id`; `down(b)`, `axis(a)` |
| `MouseButton`, `MouseState` | enum, record | `x`, `y` in framebuffer pixels (origin top-left), `wheel` (the scroll since the last sample), `buttons`; `down(b)`; `mouse_button_name` |
| `mouse_screen(x, y, w, h, &sx, &sy)` | function | window pixels to screen space: center origin, +y up, what screen-space sprites use |
| `InputState` | record | `down(key)`, `set(key, on)`, `clear()`, `pads[KV_MAX_GAMEPADS]`, `pad()` (the first connected), `mouse` |
| `Input` | record | `raw`, `sampled`, `sample()` |
| `key_from_scancode`, `scancode_from_key` | functions | the SDL mapping, both ways |
| `KV_MAX_GAMEPADS`, `KV_PAD_DEAD_ZONE` | constants | 4; 0.15 |

Header `korvid/platform_hid/events.h`: `WindowResized{width, height}`, `FocusChanged{focused}`,
`CloseRequested`, `KeyDown{key, device, repeat}`, `KeyUp{key, device}`,
`MouseMoved{x, y, dx, dy}`, `MouseButtonDown{button, x, y}`, `MouseButtonUp{button, x, y}`,
`MouseWheel{dx, dy}`, `GamepadConnected{pad}`, `GamepadDisconnected{pad}`,
`PadButtonDown{button, pad}`, `PadButtonUp{button, pad}`.

Header `korvid/platform_hid/actions.h`: `InputActionDesc`, `ActionKind`, `InputActions`,
`input_actions(e, descs)`, `input_actions_get(e)`, `input_action_override(e, name, down,
axis)`; queries by id (`InputActions::find(name)`): `action_down`, `action_pressed`,
`action_released`, `action_axis`, `action_repeat`.

Header `korvid/platform_hid/module.h`:

| name | kind | what |
|---|---|---|
| `platform_hid_module` | `ModuleDesc` | the record; kormake puts it in the list |
| `HidState` | record | `input`, `window`, `actions`: the module's state, for code outside the schedule |
| `platform_open_window(engine, desc)`, `platform_window(engine)` | functions | the one window; null if open already or refused |
| `platform_video_up(engine)` | function | SDL video and gamepads, once; false without a video driver |
| `platform_input(engine)` | function | the input, for code outside the schedule |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `WindowResized`, `FocusChanged`, `CloseRequested`, `KeyDown`, `KeyUp`, `MouseMoved`, `MouseButtonDown`, `MouseButtonUp`, `MouseWheel`, `GamepadConnected`, `GamepadDisconnected`, `PadButtonDown`, `PadButtonUp` | event types | no schemas; none carries an entity |
| `input.state`, `platform_hid.window` | resources, published | `Input` and `HostWindow`; read them through `ctx.resource<const T>` |
| `input.actions` | resource, published | the action map and its state this tick |
| `input.sample` | system, phase Input, `Always` | writes `input.state` and `input.actions`: raw becomes sampled and the actions follow, on paused ticks too |
| a pump hook | `platform_pump_add` | the window's events, once per frame |

## Behaviour worth knowing

- **Sampled input is per tick, raw is per frame.** A system sees `sampled`; render-rate
  code may read `raw`. A key pressed and released between two ticks is seen only as its
  `KeyDown` and `KeyUp` events.
- **Mouse resizing on GNOME Wayland stalls the pump** for the whole drag: SDL's libdecor
  repaints the frame inside every configure. Keyboard resizes and borderless windows are
  fine; `host_window.h` has the diagnosis and the remedy.
- **The module starts headless.** Startup brings up SDL's event subsystem only; video
  and gamepads come up with the first window, or with `platform_video_up(e)` for code
  that needs a video driver before one (the Vulkan renderer loads its loader through
  it). A test that opens no window needs no display. `SDL_VIDEODRIVER=dummy` gives a
  window-opening test a driver without a display.
- **SDL shuts down with the last user.** This module and `platform_audio` each release
  their own SDL subsystems; whichever shuts down last calls `SDL_Quit`.

## Not in this module

Touch, more than one window, controller rumble.

## Android

`src/android/host_window_android.cc` is the app's entry as well as its window:
`ANativeActivity_onCreate` runs the game's `main` on a thread of its own, and the
activity's callbacks reach `pump` through a ring and an `ALooper`. Arguments are the
launching intent's string extra `args`; output goes to logcat (tag `korvid`); the working
directory is the app's external files directory.

- **The surface comes and goes.** Android destroys the window's surface whenever the app
  leaves the screen and makes another when it returns. `HostWindow::on_surface` (through
  `platform_surface_hook`) is called from inside `pump` with null before the old one goes
  and with the new handle after: the frame loop passes both to `renderer_set_surface`.
  While there is none, `pump` waits: no frame runs.
- **Focus holds the sim** (`hid_backend_holds_unfocused`), as on iOS: resumed, with window
  focus, with a surface.
- **The window is the screen.** The size asked for chooses landscape or portrait.
  `safe_insets` reports the cutout and the system bars' edges; the bars are hidden.
- **Keys** are `AKEYCODE_*` through `input_android.cc`; Back is Escape. **Touches** are
  `TouchPoint`s. **Gamepads** are key events from a gamepad source and motion events from
  a joystick source; a pad is never reported gone (Android tells only Java).
- **Typed text** comes through the package's one Java class,
  `src/android/java/org/korvid/KorvidActivity.java`, a `NativeActivity` with a one-pixel
  view that offers the input method a connection: what the on-screen keyboard commits (and
  what an input method finishes composing) reaches `InputState::text` as UTF-8, and a
  hardware keyboard's printable keys go on to Java unhandled while a text field wants
  typing, where the layout's character is known. `text_input` raises and lowers the
  keyboard. kormake compiles the class into the package when the host has `javac`; without
  it the activity is `NativeActivity` and `text_input` warns once.
