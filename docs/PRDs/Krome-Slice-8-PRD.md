# Krome Slice 8 — Placed, moving, and not a rectangle

Nov 23, 2026 · @starekrow · building (see As built)

## Goal and exit criteria

The second of three slices drafted from Blackwing's skin import note
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout, KR28 to KR42).
[Slice 7](Krome-Slice-7-PRD.md) draws a skin's pictures. This one puts them where the skin
says, moves them, and takes presses by their shape: KR30, KR33, KR35, KR36 and KR37. Each
keeps Blackwing's ID. [Engine slice 13](Engine-Slice-13-PRD.md) has the windows.

It comes after slice 7: a dial is a strip of frames (KR29), and a look confined to a mask
is KR34's look. As with slice 7, nothing in Blackwing waits on it and it has no date.

What the five have in common is that Krome's layout is a flow, and a skin is not. A skin
places by coordinate, slides a drawer by changing one, blinks on a timer, and is clicked
through a shape drawn in another file. Each is a small departure from "rows, columns and
stacks of rectangles", and the question for review is how many such departures the kernel
takes before a skin is better served by something else.

It is done when:

- In a stack, a node 10 from the left and 10 from the right of its parent stretches with
  it; one 30 from the right and 8 from the bottom keeps its size and follows the corner;
  one at a third of the width stays at a third.
- A panel bound to `open` slides 120 pixels in 0.2 s, and its screen's handler is told
  when it arrives.
- A 500 ms timer in a screen's script costs two wakeups a second in a window paced on
  demand, and none while the window is hidden.
- A long title scrolls in a 120-pixel box at a stated rate; a short one sits still and
  asks for no frame; neither asks for one while it is hidden or scrolled out of view.
- Two buttons whose rects overlap take presses by a mask's colours, and a press on a
  button's transparent corner goes to what is under it.
- A masked control brightens by 1.2 on hover over its shape and not its rect.
- `kromedemo` has a slider whose thumb follows a three-point curve and a dial of 32
  frames, both with the keys std's slider takes, written as behaviours in Lua with no
  kernel change of their own. Whatever part could not be written that way is named.
- The screens of `kromedemo`, `kromescreens` and goldberg's window lay out as they did,
  to the pixel.

## What the code has

Checked Nov 23 on `main` (52f9963).

| ID | Where | What is there now |
|---|---|---|
| KR30 | `krome/src/layout.cc` | A stack gives each child its size on both axes and places it at one of nine anchors (`anchor=`, else the stack's alignment) inside the padding. No offset. Sizes can be a percentage of the parent's inside |
| KR33 | `krome/src/view.cc` | `hit_node` returns the deepest node whose rect holds the point, later children over earlier. Krome holds an image as a texture handle and never sees its pixels |
| KR35 | `krome/src/view.cc`, `tree.h` | `transition` is a look's property; an `Anim` eases the colours and opacity a look resolves to. Layout runs only when something marked the tree. Krome's Lua has no clock: the caret and the tooltip ask for a frame at a time (`engine_want_frame_at`) from C++ |
| KR36 | `krome/assets/std.lua` | `slider` is a behaviour: `press`, `drag`, `key`, `wheel`, and `update` drawing its parts with `w:part(name, prop, value)`. Its thumb moves by a part's width |
| KR37 | `krome` | A text with `nowrap` that does not fit is clipped. An edit scrolls to keep its caret in view |

## Placement by edge (KR30)

A child of a stack can be tied to its parent's edges. Four properties, each the place of
one of the child's edges inside the parent's padding:

| written | the edge is |
|---|---|
| `l=10` | 10 from the parent's left |
| `r=30` | 30 from the parent's right |
| `l=33.3%` | a third of the way across, from the left |
| `l=100%-40` | 40 short of the right edge: a fraction and a distance together |

`t` and `b` the same, down. With both edges of an axis tied, the child is as wide as what
lies between and `w` is ignored; with one, it keeps its size and follows that edge; with
neither, it is placed by its anchor as now.

**`x=` and `y=`** are an offset from wherever the above put it, in logical pixels, for a
thing that moves: a thumb, a drawer. All six can be bound (`x=@thumb.x`), and set by a
behaviour's `w:part`.

They mean something only in a stack; anywhere else each is an error naming the parent's
direction. A tied child adds nothing to what a hugging stack wants, as a percentage does
not: the parent's size would depend on it.

## Easing and arrival (KR35, first half)

`transition` covers `w`, `h`, `x`, `y`, `l`, `t`, `r` and `b`: when one of them changes on
a node whose look has a transition, by a binding, a state or a `w:part`, it eases from
where it is, turning around mid-way as a colour does. While any is easing the tree is
laid out each frame and the window asked for the next; when the last arrives both stop.

**`arrived=name`** on the node fires that event once when its sizes and offsets have all
reached their targets: the screen's `on.name(id)`, and `KromeEvent`. A transition
interrupted and turned around arrives once, at the end it finally reaches.

A change made by layout itself (the window resized, a sibling grown) does not ease. Only
a change to the node's own property does.

## Timers (KR35, second half)

Three functions every script and behaviour sees:

| call | what |
|---|---|
| `after(seconds, fn)` | calls `fn()` once, that long from now; returns a handle |
| `every(seconds, fn)` | calls it at that interval until cancelled; returns a handle |
| `cancel(handle)` | stops either; nothing if it has already gone |

They run on the Frame cycle's clock, in `krome.pump`, before layout. Krome asks the loop
for a frame at the soonest one due (`engine_want_frame_at`), so a timer wakes an on-demand
window when it fires and at no other time. A hidden window draws no frame and so runs no
Frame cycle: its timers wait, and one that came due while hidden fires once when the
window is seen, not once for each interval missed.

A screen's timers go when it closes, and when its script is reloaded (slice 6 runs it
again in a fresh environment; the old run's closures are not kept). A timer that throws is
cancelled and its error named with the line it was set at. At most 256 a screen.

Like the rest of Krome, none of it is hashed and none of it is in a replay.

## Text that scrolls (KR37)

A one-line text (`nowrap`) wider than its rect can move instead of being cut:

| property | what |
|---|---|
| `marquee=30` | logical pixels a second; 0 or absent: clipped, as now |
| `marquee-step=8` | moves that many pixels at a time, not smoothly |
| `marquee-mode=loop` | `loop` (the text follows itself after `marquee-gap`) or `bounce` |
| `marquee-gap=40` | the space between the end and the start, in a loop |
| `marquee-pause=1.5` | seconds at rest at each end, and before it starts |

The flag `marquee-drag` lets the pointer drag it; it starts moving again when released.

A text that fits does not move and asks for nothing. One that moves asks for its next
frame at the time it will next be a whole pixel further (a step further, with
`marquee-step`), so 30 pixels a second at scale 1 is 30 frames a second, not the
display's rate. It asks only while its rect, clipped by its ancestors, has any area and
its window is shown.

## Masks (KR33)

**A node's own alpha.** The flag `mask-alpha` on a node with an image: a point is in the
node only where the image's alpha there, through its region, frame and fit, is over a
half. A press on a round button's clear corner falls to whatever is under it.

**A shared mask.** `mask="skin/map.png"` on any node declares a mask image stretched over
that node's rect, for the nodes inside it. A descendant says which part is its own:

```
box mask="skin/map.png" {
    hit -> play  mask-color=#ff0000
    hit -> stop  mask-color=#00ff00
    hit -> seek  mask-value=12
}
```

`mask-color` is the mask's colour there, exactly. `mask-value` is its red channel, 0 to
255, for masks that number their regions (XMPlay's palette indexes: the converter writes
the index as a grey). Such a node is hit where the mask has its colour or value, whatever
its own rect is, and it needs no size of its own.

**Held once.** A mask is read as a blob and decoded by the `image` module into bytes Krome
keeps on the CPU, one copy a path however many nodes name it; it is never a texture
unless a look needs it (below). `mask-alpha` decodes the node's image a second time for
its alpha, one byte a pixel.

**`hit_node`** asks the mask where it now asks the rect. Nothing else in input changes:
capture, the release, focus by Tab all follow the node as they do.

**A look inside a mask.** With slice 7's `gain`, `hue` and `saturation`, a masked node's
look applies only where the mask is its own. For that the mask is also a texture, and the
UI item carries its slot and the colour to match. This is the larger half of KR33's cost
and the only part that touches the renderer.

## Sliders along a path, and dials (KR36)

Blackwing asks for a check, not a feature: that with bound offsets and frames, both are
behaviours in Lua. The last milestone writes them in `examples/kromedemo`:

- **A path slider**: `update` puts its thumb part at the point of a Bezier a fraction
  along (`w:part("thumb", "x", ...)`), and `press` and `drag` find the nearest point on
  the curve.
- **A dial**: `update` sets `frame-at` on its image part; `drag` turns the pointer's angle
  between `min` and `max` into a value.

Both take std's slider's keys. If either needs something a behaviour cannot reach, that
is named in the status and asked for by its own ID. Whether either belongs in std is For
review, 4.

## Out of scope

- **Rotation**, of a node or an image. A dial here is frames, as the skins' are.
- **Grid areas** (K24). Edges in a stack are not a grid.
- **Easing curves by name, keyframes, sequences.** One ease, in and out, as colours have.
- **Per-pixel hit testing of text or shapes.** A mask is an image.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR30 | `l`, `t`, `r`, `b` in a stack: a distance, a fraction, or both; `x`, `y`; all bound | P1 |
| KR35 | `transition` eases sizes and offsets; `arrived=` | P1 |
| KR35 | `after`, `every`, `cancel`; a timer wakes an on-demand window only when it fires, and waits while hidden | P1 |
| KR37 | `marquee` and its four properties; `marquee-drag`; frames asked for only while moving and in view | P1 |
| KR33 | `mask-alpha`; `mask`, `mask-color`, `mask-value`; a mask held once | P1 |
| KR33 | A look confined to its node's mask | P2 |
| KR36 | A path slider and a dial as Lua behaviours in `kromedemo`; what could not be is named | P1 |
| KM1 | Every screen in the tree lays out as before: the layout tests' rects do not move | P0 |
| KM2 | The module pages and quick references of `krome`, `kimb` and `render` change with their headers; `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **Edges and offsets** (KR30). Layout tests headless for each row of the table above,
   at three parent sizes; a hugging stack with a tied child.
2. **Easing** (KR35). Sizes and offsets through `Anim`; `arrived`; the 120-pixel drawer,
   with the frames it drew counted and the window asleep after.
3. **Timers** (KR35). The three calls; reload and close; the 500 ms timer's wakes for ten
   seconds shown and ten hidden.
4. **Scrolling text** (KR37). Loop and bounce, smooth and stepped, the drag; wakes counted
   for a short title, a long one, and a long one scrolled out of view.
5. **Masks for the pointer** (KR33). The CPU copy, `mask-alpha`, colours and values; the
   two overlapping buttons and the clear corner, in `krome_tests` with presses by
   coordinate.
6. **Looks in masks** (KR33). The item's mask slot, the shader and the CPU renderer;
   render-gate's reference on each backend.
7. **The path slider and the dial** (KR36).

**Verification.** `kormake test` on linux-debug, linux-dev and linux-ship; `krome_tests`
under `--sanitize address,undefined`; render-gate at milestone 6. `kromedemo --demo` and
`kromescreens` pixel for pixel against their references after milestones 1 and 2, which
touch layout every screen goes through.

## For review

1. **How far the kernel goes.** After slices 7 and 8 the kernel has absolute placement,
   masks and marquees: things a tool's UI never asked for and a skin cannot do without.
   The alternative is a second, smaller thing for skins (a "skin" node that is a canvas
   of placed pictures, its children not laid out by flow), which would keep K1's "nothing
   in C++ that a prelude could say" cleaner and split the vocabulary in two. The draft
   keeps one kernel.
2. **`l`, `t`, `r`, `b`.** Short, beside `w` and `h`. `left`, `right`, `top` and `bottom`
   are already flags (a text's alignment), so the long names would read two ways.
3. **Palette indexes as greys.** `mask-value` reads a channel, and the converter is asked
   to write an indexed mask as a grey image. The `image` module was not checked for
   whether its PNG decoder can hand back palette indexes; if it can, `mask-index` could
   read the file as XMPlay made it.
4. **Where the dial and the path slider live.** A dial is a widget a tool might want and
   could go in std. A slider along a Bezier is a skin's, and is better in a prelude of
   Blackwing's. The draft writes both in `kromedemo` and moves neither.
5. **A second decode for `mask-alpha`.** Krome asks the asset module for a texture and
   has no pixels. Whether the asset module still has them after the upload was not
   checked; if it does, the second decode goes.
6. **Timers in a hidden window.** The draft holds them, as Blackwing asked (its B25: a
   hidden window costs nothing). A script that counts on a timer for something other than
   drawing would be wrong to; the header says so.

## Decided

In review of the draft (Nov 26):

- **One kernel.** Korvid is a game engine, and well-defined image capabilities in Krome are
  welcome: no second, smaller thing for skins.
- **`l`, `t`, `r`, `b`**, the short names, beside `w` and `h`.
- **`mask-index`** as well as `mask-value`, if the PNG decoder can give a palette's indexes:
  a mask read as XMPlay made it.
- **The dial goes in std.** The path slider is written and tested in `kromedemo`, and left
  for someone who needs it to move.
- **The second decode for `mask-alpha` goes** if the asset module still has the pixels.
- **Timers wait while their window is hidden**, as drafted; the header says so.

## As built

**Milestone 1, Edges and offsets** (Nov 26).

- **On the node, as layout's**: `KromeNode::edge[4]` (a fraction of the parent's inside and
  a distance, `KromeEdge`), `offset`, and `tied`, a bit for each of the six said. `none`
  unties an edge. `krome_parse_edge` reads `"10"`, `"33.3%"`, `"100%-40"`, `"50%+8"`.
- **In the stack's placement only**: the anchor places the child as before, then the edges
  it ties replace that on their axis (`tie`), then the offset is added. A tied axis adds
  nothing to what a hugging stack wants, nor to its least width, nor to its height in the
  second pass. Edges that cross leave a width of 0.
- **Outside a stack**, the builder says `l= places a child of a stack, and this one's
  parent is a row` (or a col, or a root) and unties the node; `w:part` refuses the same.
- **kim** reads a percentage followed by a signed number as one value, `"100%-40"`. kimb's
  new type `Edge` (a number, a percentage, both, or `none`) checks `l`, `t`, `r`, `b`; `x`
  and `y` are numbers. All six are layout's, so a state's look cannot set them.
- **Tested.** `krome_tests`' placement test: the PRD's three (stretched, at a corner, at a
  third) and an edge at `100%-40` and `50%+2`, at three parent sizes; one edge an axis with
  the anchor on the other; an offset moving a child and what is under it; crossed edges;
  untying; a hugging stack with tied children; the words and their refusals. Through a
  running Krome (`placed_test.cc`): a `.kim` screen with a bar, a corner badge and a mark,
  followed through a resize, a drawer's `x=@drawer` moved by `krome_set`, and the mistakes
  in a row and a col, by line.
- **KM1.** `kromedemo --demo` (Kim and Lua), `--player --demo` and `kromescreens --demo`:
  45 pictures, every one byte for byte main's (largest channel difference 0).

**Milestone 2, Easing** (Nov 26).

- **A record per moving node** (`KromeModule::Motion`), beside the colours' `Anim`: twelve
  places (`w`, `h`, `x`, `y`, the four edges' fractions and distances), a mask of those
  moving, where from and where to. The node holds where it is shown while it eases;
  `krome_moved` is told the node as it was before a binding (`krome_model_changed`) or a
  behaviour's `w:part` changed it, and the frame (`apply_motions`, before the window's
  layout) writes each place where the clock has it, eased in and out as a colour is. A size
  eases only between two numbers of one mode (fixed, or a percentage), an edge only while it
  stays tied; anything else jumps. Turned around mid-way it goes from where it is shown, and
  sent again where it is already going it goes on undisturbed.
- **Not through a state.** The PRD's "by a state" is not built: a state's look cannot set
  layout (kimb refuses `hover { w=5 }`, as before this slice), and these eight are layout's.
- **`arrived=NAME`** is a node's (`KromeNode::arrived`, kimb's `String`, layout): fired once
  every place has arrived, after the paint, as a strip's event; at once when the node has no
  transition. A build never eases and never arrives.
- **Wakes.** While anything moves, the window is laid out each frame and the next asked for;
  `krome_settled` waits for it.
- **Found on the way.** In Lua, `l = 10` was taken for the helpers' own line number (they
  wrote `l` into every node table, and the converter read any integer `l` as one): the
  helpers' key is `_l` now.
- **Tested** (`placed_test.cc`): the drawer, `x=@drawer transition=0.2 arrived=opened`, sent
  120 pixels: moving on 12 frames of 60 a second, never backwards, a frame asked for on each
  and none after, its handler told once with its id; 30 frames after it, nothing laid out,
  painted or asked for. Turned around mid-way, from where it was, arriving once. A size and an
  offset at once, one arrival. A node with no transition, there at once and arrived. A
  resize, which moves a tied bar and eases nothing.
- **KM1**: the 45 pictures again. One, `kromedemo_form`, differs in a band of a row's colour
  on some runs; main's own build differs the same way from main's own picture on 5 runs of
  5, so it is the demo's timing (a row's hover caught or not), not this slice.

**Milestone 3, Timers** (Nov 26).

- **`after`, `every`, `cancel`** in each screen's environment (closures over its id), and as
  globals for the behaviours, which take the screen whose call is running
  (`KromeModule::lua_screen`, set around a behaviour's call and a timer's). A record each
  (`KromeModule::Timer`): its function's registry ref, the clock's time due, the interval,
  and `luaL_where`'s chunk and line for its error.
- **In `krome.pump`**, after the model's sets and before layout: those due, by handle (a call
  may cancel the next); one that repeats is due again an interval on, or an interval from now
  when that is already past, so a run of missed ones is one call. One that raises is
  cancelled, `a timer set at t/timers.lua:14 raised, and is cancelled: ...`. 256 a screen, the
  257th refused by `luaL_error`.
- **The clock moves in the pump now**, at the frame's start, not in the paint: a frame asked
  for at a timer's time ran the pump on the last frame's clock, found it not yet due, and asked
  again at once (two wakes for each tick). The paint moves it only when no pump came first.
- **Wakes.** The paint asks for a frame at the soonest due timer, as for a strip's frame,
  among the screens whose window is seen (`HostWindow::visible`); a window that is hidden
  asks nothing, and the frame loop draws it again when it is seen. A due time within a
  millisecond counts as due: the loop wakes when asked and the frame's dt can fall a hair
  short.
- **Gone** when its screen closes, and when the screen's scripts run again (`make_env`, so a
  reload's run starts with none).
- **Tested** (`placed_test.cc`), on a clock of the test's own: an on-demand loop stepped by the
  millisecond that runs a frame cycle only when one is wanted or due. Nothing set, no wakes in
  2 s. `every(0.5)` for ten seconds: 20 ticks, 20 wakes, the text showing them refilled in the
  same frames. Ten seconds with no frame (hidden), then one: one tick. Cancelled: one more
  wake, the frame the loop had already been asked for, then none in 3 s. `after` once; one
  cancelled before it fired never does, and a second cancel is nothing. One that raises,
  called once and named by its line. 256 and the 257th refused. A reload, and a close, leave
  none. A behaviour's `every` from its `update`, its screen's, ticking and gone with it.
- **Not tested headless:** the hidden window's silence itself (no window here can be hidden);
  the test stands in for it with no frames, which is what the loop gives a hidden window.

**Milestone 4, Scrolling text** (Nov 26).

- **The node's** (`KromeNode::marquee`, `marquee_step`, `marquee_gap`, `marquee_pause`,
  `marquee_bounce`; the flag `KROME_MARQUEE_DRAG`), so not a state's. The gap is 40 when
  unsaid. On a node that is not a text, or a text without `nowrap`, the builder says
  `marquee= moves a one-line text, and this is a text without nowrap` and turns it off.
- **The timeline is one pure function**, `krome_marquee_offset`: the offset `t` seconds in,
  moved by a unit at a time (a physical pixel, or the step), and when it next moves. Looped:
  a rest, then on by the text's width and the gap, round to its start. Bounced: a rest, to
  the far end, a rest, back.
- **A record per node** (`KromeModule::Marquee`), kept as a strip's play is: begun at the first
  paint that finds the text too wide, again when its text or properties change, dropped by a
  paint that no longer finds it moving (hidden, fitting). Paint draws the line clipped to the
  text's inside at the offset (twice, looped), through `paint_line`, the one-line glyph run
  `paint_text` drew inline before. A marquee whose inside, clipped by its ancestors, has no
  area keeps its record and asks for nothing; one in a window that is not seen asks for
  nothing either (`window_seen`, which the timers' check is too).
- **Wakes**: a due marquee repaints its screen, and the paint asks for the soonest due frame,
  with the strips' and the timers'.
- **`marquee-drag`** makes the text take a press. Held, it asks for nothing and the pointer moves
  it (looped round its width and gap, bounced held between its ends); released, its timeline is
  set so it moves on from there, forward.
- **Tested.** The timeline by hand: a loop smooth, stepped by 8, with a 1.5 s rest, round again;
  a text that fits; a bounce out, resting at the far end, back and home. Through a running
  Krome on the test's clock: a long title in a 120-pixel text at 30 pixels a second is 60
  wakes in 2 s and 60 pixels; a short one has no record; one in a scroll box scrolled out of
  view has a record that asks for nothing; stepped by 8, 15 wakes in 4 s on multiples of 8;
  hidden, no record and no wakes in 3 s, and shown again from its rest; dragged 25 pixels,
  held, released and on from there. The `.kim` words, and the two mistakes by line.
