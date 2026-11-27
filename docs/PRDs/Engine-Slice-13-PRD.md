# Engine Slice 13 — Windows that are not rectangles, and more than one

Nov 23, 2026 · @starekrow · draft

## Goal and exit criteria

The third of three slices drafted from Blackwing's skin import note
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout, KR28 to KR42).
Krome's slices [7](Krome-Slice-7-PRD.md) and [8](Krome-Slice-8-PRD.md) draw a skin and
take its input. This one is the window it sits in: KR39 to KR42. Each keeps Blackwing's
ID.

A skin from Winamp, VLC, WMP or XMPlay is not in a system window. It is its own outline,
dragged by whatever part of itself it says, often in several pieces that open, close and
stick to each other. The engine today has one window with a frame the desktop draws, and
"more than one window" is in the not-built list of `platform_hid`, `frameloop`, Krome's
master PRD and `docs/design/cycles.md`.

Blackwing asks that where each of these is cut between the engine (`platform_hid`,
`frameloop`) and its own `blackwing_window` be part of the answer. The draft's cut: the
engine opens, shapes, shows, places and reports windows, and answers the OS's question of
what a point on one is for; which windows there are, what snaps to what and what follows
what are the program's.

Nothing in Blackwing waits on it; it has no date. It is the largest of the three, and
its second half (KR40) is most of that.

It is done when:

- A round skin shows the desktop at its corners, and a click there reaches the window
  behind, on each desktop platform that can do it. For each that cannot, the module page
  says what it does instead.
- A window with no frame is dragged by the part of its screen marked for it and resized
  by a marked corner between the bounds its screen gives, through the OS's own move and
  resize, on each desktop platform.
- A main window and a playlist window are up at once, each showing a Krome screen. One is
  hidden and shown again with its scroll where it was. Neither is presented while only the
  other changes.
- The keyboard goes to one node across both.
- A playlist window dragged within 10 pixels of the main one snaps to it and then follows
  it, where the platform lets a program place a window. Where it does not, the page says
  so and the windows move apart.
- A program that opens one window is the program it was: goldberg's window, `kromedemo`
  and every game's hashes and frames.

## What the code has

Checked Nov 23 on `main` (52f9963).

| ID | Where | What is there now |
|---|---|---|
| KR39 | `platform_hid` (`host_window.h`) | `WindowDesc::borderless`: SDL's borderless flag; on Win32 a popup with a thick frame, still resizable by the desktop. Nothing about alpha or shape |
| KR39 | the renderers | Vulkan asks for an opaque composite first (`vk_swapchain.cc`); D3D12's swapchain is `DXGI_ALPHA_MODE_IGNORE`. Each clears to a colour and presents an opaque frame |
| KR40 | `platform_hid/src/module.cc` | `platform_open_window` is "the one window; null if open already". The Win32 backend keeps its window in one global. `Input` is one state with one pointer |
| KR40 | `render` | `RendererDesc::native_window`: one. `renderer_ui_set(r, layer, list)` names no window |
| KR40 | `frameloop`, `krome` | The loop holds one `HostWindow*` and paces by it. `krome.paint` reads its size and scale from `platform_window(e)`. The keyboard already has one owner across every screen |
| KR41 | `platform_hid` (`host_window.h`) | `capture_mouse`, `cursor` (with `Move`, `ResizeEW`, `ResizeNS`). A `TODO(wayland)` already asks for a borderless window resized through `SDL_SetWindowHitTest`, for a different reason (libdecor's stall on GNOME) |
| KR42 | `platform_hid` | No position, no move, no display bounds. `minimize` is the only thing a program does to its window |

## A shaped window (KR39)

**`WindowDesc::transparent`.** The window is opened with an alpha channel and no frame.
The renderer clears it to nothing and presents with its alpha, premultiplied, where the
platform composites. Where it does not, the window is opaque and the module says so once.

**`HostWindow::shape(alpha, w, h)`.** A mask the program gives, one byte a pixel, scaled
to the window: where it is under a half the window takes no input and the click goes to
what is behind. Given again when the skin or the size changes; null for the whole rect.
The program gives the mask because it has one (Krome's, from slice 8's `mask-alpha`) and
reading the alpha back from a frame every time it changes does not pay.

Drawn alpha and the input shape are two things. A soft edge is drawn at half alpha and is
either in or out of the shape.

**What each platform does.** As found at milestone 1 (SDL 3.4.2; `platform_hid`'s page
keeps it current):

| platform | alpha | input through the clear parts |
|---|---|---|
| Wayland | composited always | the surface's input region, which the engine sets itself: SDL's window shape gives Wayland none (it keeps the shape for its own renderer's alpha) |
| X11, with a compositor | an ARGB visual, which the engine asks SDL for by id: SDL gives one to a software or GL window, not to a Vulkan one | the Shape extension's input shape, through SDL's window shape |
| X11, without | none: the shape is also the window's bounding shape, set by the engine (SDL sets the input shape alone), one bit a pixel, hard edged | the same shape |
| Win32 | composited always (the DWM). D3D12: a composition swapchain, premultiplied, shown through DirectComposition, on a window with no redirection bitmap. Software: a layered window | the window's region; a layered window also passes a click where its alpha is 0 |
| macOS | a layer that is not opaque | AppKit passes clicks where a clear window is clear |
| Android, iOS | not offered | |

Win32 was the row with real work in it and the reason this is not one milestone (milestone 3,
below, found it).

## Grips and bounds (KR41)

With no frame the program says what each part of the window is for.
**`HostWindow::grips(list)`**: rects in window units, each a `WindowGrip`: `Move`, or one
of the eight edges and corners. When the OS asks what is under a point (SDL's hit test,
`WM_NCHITTEST`) the backend answers from the list, and the OS does the move or the resize
itself: its snapping, its tiling, its keyboard, and on Wayland the only way there is. The
list is copied, since Win32's window has a thread of its own.

**`HostWindow::bounds(min_w, min_h, max_w, max_h)`**: what a resize is held to. 0 for no
bound.

**Krome.** A property `grip=` on a node (`move`, `n`, `ne`, `e`, `se`, `s`, `sw`, `w`,
`nw`). After layout Krome gives its window the rects of every node that has one, when
they changed. A node with a grip takes no press of its own: the OS has it. The root's
size range (`w=275..800 h=116..600`) becomes the window's bounds.

Grips are rects. A drag area that is a mask's shape (XMPlay's "Heading" index) is its
bounding rects until someone asks for better.

This also does what the `TODO(wayland)` in `host_window.h` asks: a borderless window with
the engine's own edges, where libdecor stalls a resize.

## Several windows (KR40)

**The platform.** `platform_open_window` can be called again, up to eight. Each
`HostWindow` has an id; the first is the main one, and `platform_window(e)` still returns
it. `WindowResized`, `FocusChanged`, `CloseRequested` and `WindowVisibility` carry the
id. The pointer is in one window at a time: `Input`'s mouse says which, in that window's
pixels. `HostWindow::show(on)` hides and shows without closing. A phone refuses a second.

**The renderer.** One renderer, one device, a surface for each window:
`renderer_surface_add(r, window)` and `_remove`. The world (sprites, post) is drawn in the
main window only. A UI list is set for a surface (`renderer_ui_set` gains the surface;
the form without it means the main one), and a surface whose lists did not change since
it was last presented is not drawn and not presented. The CPU renderer keeps a software
frame a window.

**The loop.** One Frame cycle, run when any window is to be drawn; `docs/design/cycles.md`
keeps "more than one Frame cycle" as not built. What makes "neither draws while the other
changes" true is the renderer's rule above, not a cycle a window. Under `OnDemand` and
`Decoupled` a frame is wanted when any shown window's lists changed; a hidden window is
skipped, and the loop draws nothing only when all are hidden. `Continuous` presents the
main window every frame and the others when they change.

**Krome.** A screen is opened in a window: `krome_open` takes one more argument, the
main window when left out. Size and scale are each window's own, so a screen dragged to a
display at another scale is laid out for it. Pointer events go to the window's screens.
The keyboard is one node across all of them, as it is across screens now, and follows the
OS's focus between windows. An overlay stays inside its screen's window: a dropdown at
the bottom of a short window flips up, as now, and does not leave it. Hiding a window
keeps its screens built, with their models and scrolls.

## Placing windows (KR42)

`HostWindow::position(&x, &y)`, `move_to(x, y)`, a `WindowMoved` event with the id, and
`platform_display_bounds(window)`: the usable rect of the display it is on.
`platform_can_place(e)` says whether any of it works.

Snapping and following are then the program's, as Blackwing asked. Where a program can
place a window (X11, Win32, macOS) it drags by its own hand, with `capture_mouse` and
`move_to`, snapping as it goes, and moves the followers when the leader's `WindowMoved`
comes. That is a second kind of move beside KR41's, and a node says which it wants:
`grip=move` is the OS's, `grip=drag` sends the screen the pointer's travel and leaves the
moving to it.

**Wayland** gives a client no position and takes none. There `platform_can_place` is
false, `grip=drag` is the OS's move, and nothing snaps or follows. Blackwing's note offers
"one window drawn as several" as the other answer; that is the program's to choose (one
transparent window with every panel in it is KR39 and no more), and the engine does not
do it for it.

## Out of scope

- **The world in a second window.** Sprites, cameras and post are the main window's.
- **A Frame cycle a window**, and windows on threads of their own.
- **Overlays that leave their window**: a menu wider than a narrow skin. It wants popup
  windows parented to another, which is its own protocol on each platform.
- **Drag and drop between windows, or from the desktop.**
- **Phones.** One window, opaque, placed by the OS.
- **The snapping itself**, and which windows a skin has: Blackwing's `blackwing_window`.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR39 | `WindowDesc::transparent`; the renderers present with alpha where the platform composites | P1 |
| KR39 | `HostWindow::shape`: input passes where the mask is clear | P1 |
| KR39 | `platform_hid`'s page says, for Wayland, X11 with and without a compositor, Win32 and macOS, what is done and what is not | P0 |
| KR41 | `HostWindow::grips`, `bounds`; Krome's `grip=` and the root's range | P1 |
| KR40 | A second and later window: ids on the window's events, the pointer's window, `show` | P1 |
| KR40 | A surface a window in every renderer; a UI list for a surface; a surface unchanged is not presented | P1 |
| KR40 | A Krome screen in a window; size and scale each window's; one keyboard focus | P1 |
| KR42 | `position`, `move_to`, `WindowMoved`, `platform_display_bounds`, `platform_can_place`; `grip=drag` | P2 |
| W1 | A program with one window runs as before: the demos' hashes, goldberg's window, render-gate | P0 |
| R14 | The module pages and quick references of `platform_hid`, `render`, `frameloop` and `krome` change with their headers; `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **Transparent and shaped, on SDL3** (KR39). `examples/window` with a round picture, on
   Wayland and on X11 with and without a compositor. The table above corrected from what
   is found.
2. **Grips and bounds** (KR41). The platform's half, then Krome's `grip=`. The same
   example dragged and resized by itself; the `TODO(wayland)` closed or restated.
3. **Win32 and macOS for the one window** (KR39, KR41). Written; verified where a machine
   is to hand. Win32's alpha is decided here.
4. **A second window in the platform** (KR40). Ids, events, the pointer, `show`; Win32's
   global made a list. No drawing yet: the second window is blank.
5. **Surfaces** (KR40). Vulkan first, then the CPU renderer; Metal and D3D12 written. A
   UI list a surface, and the count of presents a surface.
6. **The loop and Krome** (KR40). Screens in windows, pacing over several, hiding. Two
   Krome windows in `kromedemo --two`, with each one's presents counted while the other
   changes.
7. **Placing** (KR42). Position, moving, bounds; `grip=drag`. `examples/window` with two
   windows that snap and follow, on X11; on Wayland, what it does instead.

**Verification.** `kormake test` on linux-debug, linux-dev and linux-ship; `platform_hid`,
`frameloop` and `krome` under `--sanitize address,undefined` and `thread`; render-gate on
each backend after milestone 5; arcfire's and arcanum's demo hashes as recorded. Windows
and macOS are written and not verified unless a machine is to hand: the status says
which, by row of the platform table.

## For review

1. **Two slices, not one.** Milestones 1 to 3 (one window, shaped, dragged by itself) are
   useful alone and are what "all four formats" need first. Milestones 4 to 7 are a
   change to the renderer's and the platform's shape. They could be slices 13 and 14.
2. **UI only in the other windows.** It keeps the renderer's change to "a surface and its
   lists". A game with a second view of its world (an editor's) would want more, and
   would be asking for cameras a surface, which nothing asks for yet.
3. **One Frame cycle.** Krome paints only the layers that changed, so a cycle run for one
   window costs the other little. If it is measured to cost more than little (a screen
   whose `krome.pump` is heavy), a cycle a window is the next step and cycles.md's to
   design.
4. **The shape from a mask the program gives.** The alternative is the engine reading the
   presented frame's alpha, which needs no second source of truth and costs a readback on
   every backend whenever the picture changes.
5. **Win32's alpha.** Not known until milestone 3. If per-pixel alpha there needs a
   composition swapchain, it reaches into `renderer-d3d12` and into Vulkan on Windows
   separately; a shape cut one bit a pixel (a window region) may be all the first version
   does there.
6. **What SDL gives.** The draft counts on SDL3's transparent windows, window shape and
   hit test for Wayland, X11 and macOS. None is used in the tree today and none was tried
   for this draft.
7. **Before or after Krome's slices.** Milestones 1 and 2 need nothing from slices 7 and
   8 but a mask's bytes for `shape`, which a test can make. A skin needs all three.

## Status

**Milestone 1, transparent and shaped on SDL3 (KR39): built Nov 25, 2026.** As above, and:

- **The platform.** `WindowDesc::transparent` opens with `SDL_WINDOW_TRANSPARENT` and no
  frame; `HostWindow::transparent()` is whether it composites: on X11, whether a
  compositing manager holds `_NET_WM_CM_Sn` and the window got the alpha visual asked for.
  `HostWindow::shape` keeps a copy of the mask, cut at 128 and scaled to the window in
  window units, and gives it again after each resize. A software frame says when its
  fourth byte is alpha (`SoftwareFrame::alpha`). What SDL does not do, the engine does
  through SDL's own Xlib, Xext and libwayland-client, opened by name: it links none of
  them. The shared half is `src/window_parts.cc`; Win32, Android and iOS refuse.
- **The renderers.** `RendererDesc::transparent`; Vulkan presents
  `VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR` where the surface offers it, and its log line
  says so. The sprite and UI blends already kept a premultiplied target, so nothing else
  changed. The CPU renderer draws no alpha yet and says so; D3D12 and Metal are milestone 3.
  The frame loop's `--transparent` opens such a window and clears to nothing.
- **`examples/window --round`**: a disc with a soft edge in a 360 x 360 window, its shape
  from the same coverage.
- **Measured** with an Xlib probe: on a rootful Xwayland (no compositor) the window is
  opaque, its bounding and input shapes are the disc's 207 rects (98,676 px; a disc of
  radius 178 is 99,538), a click on a corner reaches a window behind and one inside reaches
  the program. On GNOME's Xwayland (composited) the window is depth 32, its corners alpha
  0 and the same input shape; XTest does not reach a rootless Xwayland's windows, so no
  click was tried there. On Wayland the protocol shows the same 207 rects in
  `set_input_region`. `kromedemo --transparent` presents premultiplied on Wayland and on
  composited X11, opaque without a compositor, with no validation errors.
- **Seen on Wayland** (GNOME, by @starekrow): the desktop shows at the disc's corners and
  a click there reaches the window behind; a click inside is the program's. macOS is not
  tried.

**Milestone 2, grips and bounds (KR41): built Nov 25, 2026.** As above, and:

- **Pixels, not window units.** Grips and bounds are framebuffer pixels, as the pointer and
  `text_area` are; the backend divides by the scale. Bounds are given again when the scale
  changes.
- **`WindowGrip::None`**: a rect that is the program's. The last rect holding a point
  answers, so a button inside a title bar is listed after it and stays the button's. Krome
  lists, after the grips, the nodes that take a press of their own and its open overlays
  (all of the window for a modal), and drops the ones over no grip. 64 rects at most.
- **SDL3**: `SDL_SetWindowHitTest` while the list is not empty; `SDL_SetWindowMinimumSize`
  and `MaximumSize`. On Wayland the protocol shows `set_min_size(160, 160)` and
  `set_max_size(720, 720)` for `window --grips`.
- **Krome**: `grip=` in the kernel, the `.kimb` check and Lua; given after a paint, when the
  list or the bounds changed; a press on a grip is the OS's. `krome_tests`' `grips_test`
  checks the list, the bounds at scales 1 and 2, the presses and an overlay.
- **The `TODO(wayland)`** in `host_window.h` is restated: a borderless window with grips
  owns its frame and does not stall; one with the desktop's frame still does.
- **Seen on Wayland** (GNOME, by @starekrow): `window --grips` moves by its band and resizes
  by its handle, held to its bounds. Through about 300 sizes of one drag the loop ran 112 to
  207 frames a second, its longest gap 21.7 ms: no stall.
- **Seen on X11** (GNOME's Xwayland, composited, by @starekrow): the band moves the window
  and the handle resizes it, stopped at exactly 160 and 720 pixels a side. Gaps rose to
  20 to 25 ms near 720 x 720: the example redraws the frame and the shape at every size,
  which costs with the area. X11 without a compositor needs a window manager for grips to
  do anything, and is not tried.


**Milestone 3, Win32 for the one window (KR39, KR41): built Nov 26, 2026.** macOS goes
through SDL, written at milestones 1 and 2; no Mac is to hand, so it is not tried. On Win32:

- **The window.** A transparent window is a popup with `WS_THICKFRAME` (which is what lets an
  edge's hit resize it) and no frame at all: `WM_NCCALCSIZE` gives the frame's room to the
  client area. `transparent()` is true wherever it was asked for: the DWM composites every
  window. A software one is layered (`WS_EX_LAYERED`), its frame a DIB section given with
  `UpdateLayeredWindow`, premultiplied; a frame drawn at a size the window has since left
  is not shown, since showing it would undo a drag. A renderer's has no redirection bitmap
  (`WS_EX_NOREDIRECTIONBITMAP`).
- **The shape** is the window's region, one rect a run of a row, built on the engine's
  thread and set on the window's (`SetWindowRgn` would wait on it otherwise), and again
  after each resize. **Grips** answer `WM_NCHITTEST` and **bounds** `WM_GETMINMAXINFO`, from
  copies the window's thread reads under the lock; a bound given while the window is past
  it brings it inside. Framebuffer pixels are client pixels here, so nothing is scaled.
- **D3D12** (review item 5's answer): a transparent window's swapchain is made for
  composition (premultiplied, stretch scaling, no tearing) and is the content of one
  DirectComposition visual, the root of a target for the window. It reaches only
  `d3d_swapchain.cc`. Vulkan is not bound on Windows and is not changed there. The CPU
  renderer writes opaque alpha, so a layered window shows black where it is clear, as on
  Linux, and says so.
- **Measured** on Windows 11 at 150% scale, with `WindowFromPoint` and the screen's pixels
  (`BitBlt` with `CAPTUREBLT`): `window --round` (layered) shows the desktop at a corner and
  takes no click there, and shows the disc (`#f29e2e`) and takes the click inside; `rocks
  --transparent` (D3D12) shows the desktop's own colour at a corner, with no shape, so the
  whole rect still takes clicks, as specified. `window --grips`, driven by synthesized mouse
  input: the band moved it by exactly (200, 100); the handle resized it, stopped at exactly
  720 and 160 pixels a side; gaps of 20 to 33 ms only while resizing, the example
  redrawing its picture and shape at each size. Ordinary and `--borderless` windows keep
  their styles. Not tried: whether the region cuts a D3D12 window's picture (no program
  here gives a D3D12 window a shape yet), and a person's own drag.

**Milestone 4, a second window in the platform (KR40): built Nov 26, 2026.** As above, and:

- **The list.** `HidState::windows`, `KV_WINDOWS` (8) by id; the first opened is the main one
  (id 0, `platform_window`, the `platform_hid.window` resource) and closes with the module.
  `platform_close_window` closes another, `platform_window_by_id` finds one by an event's id.
  The backend says how many it opens (`hid_backend_windows`: 1 on a phone).
- **Ids on the events**, a `window` field last with a default of 0, so a program with one
  window builds and runs as it did: `WindowResized`, `FocusChanged`, `CloseRequested`,
  `WindowVisibility`, and the mouse's four too, since a Krome screen will need to know which
  window a press is in (milestone 6). `MouseState::window` is the pointer's; a move into
  another window has no travel. Recordings keep actions, not events, and nothing hashes
  `Input`, so neither changes.
- **One pump.** The main window's pump takes every window's events in the order they came and
  gives each to its window: on SDL by the event's window id (an event of a window already
  closed is dropped); on Win32 each window keeps its own thread and they share one queue,
  each message saying its window. The loop's wait is on that queue, so any window ends it.
  Win32's global is `g_wins[KV_WINDOWS]` and a shared queue; a closed window's unpumped
  messages are dropped, since its id may be another's by the next pump.
- **`show(on)`**: SDL's hide and show, Win32's `SW_HIDE` and `SW_SHOWNA`; nothing on a phone.
  A new reason to be hidden, the program's, which `WindowVisibility` gives as `Hidden`.
- **What is the main one's, and what follows the focus**: the clipboard is the main
  window's; the cursor and capture the pointer's window's; the input method's area the
  focused window's; text input is turned on for every open window. The frame loop quits on
  the main window's `CloseRequested` only.
- **`examples/window --two`** opens a second window; H hides it and shows it again, and closing
  it hides it. `--two-script` hides it at 1 s, shows it at 2 s and fails unless each took.
- **Measured** on Windows 11: with the two side by side and clicks from synthesized input,
  the pointer was reported in window 0 at (720, 388), then window 1 at (360, 223), each in its
  own pixels; focus moved with ids each time; `--two-script` passed, the window 720 x 480
  after it came back; a close sent to the second hid it and the program went on. SDL's half
  (Linux, macOS), Android's and iOS's are written and not built here; nine windows at once
  is not tried.

**Verified for milestones 1 and 2** (Nov 25): `kormake test` on linux-debug, linux-dev and
linux-ship; `platform_hid` and `krome` under `--sanitize address,undefined` and `thread`
(`frameloop` has no tests of its own); `tools/determinism-check.sh` (rocks in every
configuration, arcfire's two recordings) unchanged; `tools/doccheck.sh` reports nothing
new. Windows and macOS are not built here. Milestone 3 (Nov 26): `kormake test ./modules
windows-dev`, 36 test programs, every check passed. Milestone 4 (Nov 26): the same, passed.
`tools/doccheck.sh` reports one error, `asset_render`'s, which was there before slice 13's
milestone 3.
