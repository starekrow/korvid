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

**The window** (`host_window.h`). Opened on request (the frameloop module opens the main one)
and closed at module shutdown. It is a service, not an
entity. `platform_pump`, through the pump hook this module adds, turns OS events into bus
events (`WindowResized`, `FocusChanged`, `CloseRequested`, `WindowVisibility`, key, mouse
and gamepad edges) and updates the raw input state. **Whether it is seen**:
`WindowVisibility{visible, why}` when that changes, at most once a pump, and
`HostWindow::visible()` (or `platform_window_visible(e)`) between. Minimized on every
desktop; covered or away where the OS says so (SDL's occluded: a suspended Wayland
surface, macOS's occlusion); on a phone the background, whose loop waits in the pump, so
both events arrive when the app comes back. Where the OS cannot tell, it is seen.
**More than one** (slice 13, KR40): `platform_open_window` again, up to `KV_WINDOWS` (8) at
once; a phone refuses a second. Each has an id (`HostWindow::id()`); the first opened is the
main one, id 0, which `platform_window(e)` and the `platform_hid.window` resource are, and
which closes with the module; `platform_close_window` closes another. Every window event
and every mouse event says which window it is about (`window`), and `MouseState::window`
is the one the pointer was last in, whose pixels its x and y are: a move into another
window has no travel. The main window's pump takes every window's events, in the order
they came (SDL has one queue; on Win32 each window has its thread, and they share one
queue). `HostWindow::show(on)` hides a window and shows it again without closing it: its
size, shape and grips are kept, and `WindowVisibility` says `Hidden`. The keyboard and
typed text go to whichever window has the focus, unnamed: Krome keeps one focus across
them. The cursor and capture are the pointer's window's, the input method's area the
focused one's, the clipboard the main one's. The frame loop quits on the main window's
`CloseRequested` only. `window --two` shows it.

`HostWindow::minimize(on)` minimizes or restores it, for tests (Wayland restores nothing
a program asks to). A Vulkan window hands its native handle to the renderer; a software window can
only `present_blank`. **Waiting for it** (KR26): `HostWindow::wait(seconds)` blocks till the
OS has something for the next pump or `wake()` is called, taking nothing off the queue,
and the module gives it to `platform_wait` while the window is open. On SDL3 it is
`SDL_WaitEventTimeout` with no event to fill, in whole milliseconds rounded up; `wake()`
pushes one user event at most, which the pump takes. On Win32 the loop's thread waits on two
events: one the window's thread sets as it queues a message (and the pump resets as it takes
them), and `wake()`'s; pads read through SDL beside the window do not end a wait. Android
and iOS wait 4 ms at most: theirs are not written yet (docs/PRDs/Engine-Slice-12-PRD.md,
"Left open").
`input_delay()` is how long the last key or button pressed waited for the pump: from SDL's
stamp on SDL3, from the window thread's queueing of it on Win32.

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

## A window that is not a rectangle

A skin is its own outline, dragged by whatever part of itself it says. Three calls make a
window one (`docs/PRDs/Engine-Slice-13-PRD.md`, KR39 and KR41); `window --round` and
`window --grips` show them.

**Transparent** (`WindowDesc::transparent`). The window opens with an alpha channel and no
frame. Where the system composites, what is drawn clear shows what is behind:
`HostWindow::transparent()` says whether it does, and the open's log line says which it got.
A software frame then carries alpha in its fourth byte, premultiplied (`SoftwareFrame::alpha`);
a renderer is told through `RendererDesc::transparent`, and the frame loop's `--transparent`
does both and clears each frame to nothing.

**The shape** (`HostWindow::shape(alpha, w, h)`). A mask the program gives, a byte a pixel,
scaled to the window and cut at 128: under it the window takes no input, and a click goes
to the window behind. Where nothing composites, it also cuts the window out of the
picture, one bit a pixel. Kept across a resize, scaled to the new size; given again when
the picture changes. Drawn alpha and the shape are two things: a soft edge drawn at half
alpha is either in the shape or out of it.

**Grips and bounds** (`HostWindow::grips(list, count)`, `bounds(min_w, min_h, max_w, max_h)`).
Rects in framebuffer pixels, each a `WindowGrip`: `Move`, an edge or a corner, or `None`,
a rect that is the program's over a grip under it. When the OS asks what is under a point,
the last rect that holds it answers, and the OS does the move or the resize itself, with
its own snapping and tiling; the program sees no button for it. A borderless window with
grips owns its frame, so the GNOME Wayland resize stall (below) does not reach it. Bounds
hold any resize, in pixels, 0 for none, and follow a change of scale. Krome gives both
from a screen's `grip=` nodes and its root's size range
([krome](../krome/README.md#behaviour-worth-knowing)).

**What each platform does**, as found on SDL 3.4.2 and Windows 11:

| platform | alpha | input through the clear parts | grips, bounds |
|---|---|---|---|
| Wayland (GNOME) | always composited; Vulkan presents premultiplied | the surface's input region, set by the engine: SDL's window shape gives Wayland none | SDL's hit test (`xdg_toplevel` move and resize); `set_min_size`, `set_max_size` |
| X11, with a compositor | an ARGB visual, which the engine asks SDL for (SDL gives a Vulkan window none of its own); Vulkan presents premultiplied | the Shape extension's input shape, through SDL | SDL's hit test (`_NET_WM_MOVERESIZE`); the WM's size hints |
| X11, without | none: the window is opaque, and the shape is also its bounding shape, cut one bit a pixel and hard edged | the same shape | the same, where a window manager runs |
| Win32 | always composited (the DWM): a popup with no frame. D3D12 presents premultiplied through DirectComposition (the window has no redirection bitmap); a software window is layered, its frame given with `UpdateLayeredWindow`. A Vulkan window (not bound on Windows by default) is opaque, and its shape cuts it out | the window's region (`SetWindowRgn`). A layered window's picture is cut by it too, one bit a pixel, so a soft edge is drawn inside it, and a click passes where its alpha is 0; whether the region cuts a D3D12 window's picture is not tried | `WM_NCHITTEST` from a copy the window's thread keeps; `WM_GETMINMAXINFO` |
| macOS | through SDL; not tried | through SDL; not tried | through SDL; not tried |
| Android, iOS | not offered | | |

The X11 rows were measured on a rootful Xwayland (no compositor) and on GNOME's Xwayland
(composited): the window's depth, its bounding and input shapes and its pixels, and, on the
rootful one, a click on a clear corner reaching a window behind. The Wayland row was read
from the protocol (`WAYLAND_DEBUG=1`) and seen on GNOME: the clear corners, a click through
them, a move by a grip and a resize held to its bounds. On GNOME's Xwayland a move and a
resize by a grip were seen too, the resize stopped at both bounds; with no compositor,
grips need a window manager, and are not tried. On X11 the engine reaches Xlib and Xext, and on Wayland
libwayland-client, through SDL's own libraries, opened by name: it links none. The Win32 row was
measured on Windows 11 at 150% scale: `WindowFromPoint` and the screen's pixels at a clear
corner (the desktop) and inside (the program), for `window --round` (layered) and `rocks
--transparent` (D3D12); `window --grips` moved by its band and resized by its handle with
synthesized mouse input, stopped at exactly 160 and 720 pixels a side.

## Quick reference

Header `korvid/platform_hid/host_window.h`:

| name | kind | what |
|---|---|---|
| `WindowDesc` | record | `title`, `width`, `height`, `vsync` (a preference the renderer reads), `surface` (`Software`, `Vulkan`, `Metal`, `D3D12`), `borderless`, `transparent` |
| `HostWindow` | record | `open(desc)`, `close()`, `is_open()`, `id()`, `show(on)`, `width()`, `height()`, `vsync()`, `focused()`, `visible()`, `hidden_why()`, `minimize(on)`, `pump(bus, input)`, `wait(seconds)`, `wake()`, `input_delay()`, `present_blank()`, `native_handle()`; `transparent()`, `shape(alpha, w, h)`, `grips(list, count)`, `grip_at(x, y)`, `bounds(min_w, min_h, max_w, max_h)` |
| `KV_WINDOWS` | constant | 8: the most windows open at once |
| `WindowGrip`, `WindowGripRect`, `KV_WINDOW_GRIPS` | enum, record, constant | `Move`, `N` ... `NW` by compass point, `None` (the program's); a rect in framebuffer pixels and its grip; 64 kept |

Header `korvid/platform_hid/input.h`:

| name | kind | what |
|---|---|---|
| `Key` | enum | letters, digits, F keys, `Escape`, `Space`, `Enter`, `Tab`, `Backspace`, arrows, modifiers; `key_name` |
| `PadButton`, `PadAxis` | enums | the standard layout; `pad_button_name`, `pad_axis_name` |
| `GamepadState` | record | `buttons` (bits), `axes`, `connected`, `id`; `down(b)`, `axis(a)` |
| `MouseButton`, `MouseState` | enum, record | `x`, `y` in framebuffer pixels (origin top-left) of `window`, the one the pointer was last in, `wheel` (the scroll since the last sample), `buttons`; `down(b)`; `mouse_button_name` |
| `mouse_screen(x, y, w, h, &sx, &sy)` | function | window pixels to screen space: center origin, +y up, what screen-space sprites use |
| `InputState` | record | `down(key)`, `set(key, on)`, `clear()`, `pads[KV_MAX_GAMEPADS]`, `pad()` (the first connected), `mouse` |
| `Input` | record | `raw`, `sampled`, `sample()` |
| `key_from_scancode`, `scancode_from_key` | functions | the SDL mapping, both ways |
| `KV_MAX_GAMEPADS`, `KV_PAD_DEAD_ZONE` | constants | 4; 0.15 |

Header `korvid/platform_hid/events.h`: `WindowResized{width, height, window}`, `FocusChanged{focused, window}`,
`CloseRequested{window}`, `WindowVisibility{visible, why, window}` (`WindowHidden`: `None`, `Minimized`, `Occluded`, `Hidden`), `KeyDown{key, device, repeat}`, `KeyUp{key, device}`,
`MouseMoved{x, y, dx, dy, window}`, `MouseButtonDown{button, x, y, window}`, `MouseButtonUp{button, x, y, window}`,
`MouseWheel{dx, dy, window}`, `GamepadConnected{pad}`, `GamepadDisconnected{pad}`,
`PadButtonDown{button, pad}`, `PadButtonUp{button, pad}`.

Header `korvid/platform_hid/actions.h`: `InputActionDesc`, `ActionKind`, `InputActions`,
`input_actions(e, descs)`, `input_actions_get(e)`, `input_action_override(e, name, down,
axis)`; queries by id (`InputActions::find(name)`): `action_down`, `action_pressed`,
`action_released`, `action_axis`, `action_repeat`.

Header `korvid/platform_hid/module.h`:

| name | kind | what |
|---|---|---|
| `platform_hid_module` | `ModuleDesc` | the record; kormake puts it in the list |
| `HidState` | record | `input`, `windows` (by id, `[0]` the main one), `actions`: the module's state, for code outside the schedule |
| `platform_open_window(engine, desc)`, `platform_window(engine)` | functions | a window, the main one first; null with `KV_WINDOWS` open, a second on a phone, or refused. The main window |
| `platform_close_window(engine, window)`, `platform_window_by_id(engine, id)` | functions | closes a window other than the main one; the open window with an event's id |
| `platform_window_visible(engine)` | function | whether the main window is seen; true with no window |
| `platform_video_up(engine)` | function | SDL video and gamepads, once; false without a video driver |
| `platform_input(engine)` | function | the input, for code outside the schedule |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `WindowResized`, `FocusChanged`, `CloseRequested`, `WindowVisibility`, `KeyDown`, `KeyUp`, `MouseMoved`, `MouseButtonDown`, `MouseButtonUp`, `MouseWheel`, `GamepadConnected`, `GamepadDisconnected`, `PadButtonDown`, `PadButtonUp` | event types | no schemas; none carries an entity |
| `input.state`, `platform_hid.window` | resources, published | `Input` and `HostWindow`; read them through `ctx.resource<const T>` |
| `input.actions` | resource, published | the action map and its state this tick |
| `input.sample` | system, phase Input, `Always` | writes `input.state` and `input.actions`: raw becomes sampled and the actions follow, on paused ticks too |
| a pump hook | `platform_pump_add` | the window's events, once per frame |
| a waiter | `platform_set_waiter` | `platform_wait` waits on the window while it is open |

## Behaviour worth knowing

- **Sampled input is per tick, raw is per frame.** A system sees `sampled`; render-rate
  code may read `raw`. A key pressed and released between two ticks is seen only as its
  `KeyDown` and `KeyUp` events.
- **Mouse resizing on GNOME Wayland stalls the pump** for the whole drag: SDL's libdecor
  repaints the frame inside every configure. Keyboard resizes and borderless windows are
  fine, and a borderless window resized by its own grips is too; `host_window.h` has the
  diagnosis.
- **A shape asks for a transparent window.** `shape` on a window opened without
  `WindowDesc::transparent` warns once and keeps the rectangle: SDL shapes only those, and
  on Win32 only a transparent window is a popup whose region is its client area.
- **The module starts headless.** Startup brings up SDL's event subsystem only; video
  and gamepads come up with the first window, or with `platform_video_up(e)` for code
  that needs a video driver before one (the Vulkan renderer loads its loader through
  it). A test that opens no window needs no display. `SDL_VIDEODRIVER=dummy` gives a
  window-opening test a driver without a display.
- **SDL shuts down with the last user.** This module and `platform_audio` each release
  their own SDL subsystems; whichever shuts down last calls `SDL_Quit`.

## Not in this module

Touch, drawing in more than one window (slice 13's KR40, its later milestones), placing a window and the display's bounds
(KR42), controller rumble.

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
