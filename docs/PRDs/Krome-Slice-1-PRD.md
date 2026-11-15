# Krome Slice 1 — Boxes from Lua

Nov 15, 2026 · @starekrow · draft

## Goal and exit criteria

Slice 1 builds Krome's kernel at its smallest useful size: the tree, layout, overlays,
the render primitive made for UI, Krome's own Lua state with screens written as Lua
tables, a model, named events, and a window that costs nothing while it is still. The
master PRD (`Krome-Master-PRD.md`) says what each piece is; this says what is built now,
in what order, and how we know it worked.

The slice is the proof of the kernel. If the layout model cannot say a tool screen, if
the primitive cannot draw a deliberate-looking one on three backends, if overlays do not
fit the input model, or if a still window is not free, this is where it shows, before a
markup, a prelude and a widget set are built on top. There is no markup in this slice:
everything is written in Lua, in the tree shape `.kimb` will decode to in slice 2.

It is done when:

- `examples/kromedemo` opens a window with a form written in Lua: a title bar, labelled
  rows, a list of a few dozen items in a scrolled, clipped box, buttons with hover and
  pressed looks, a disabled button, a menu button. Resizing the window reflows it: fills
  grow and shrink, ranges hold, nothing overlaps, edges land on whole pixels.
- The menu button opens a menu as an overlay below itself (above itself near the bottom
  of the window), drawn over everything and outside the box that clips the button; a
  press outside closes it and reaches nothing under it; Escape closes it. A modal overlay
  holds all input until it closes.
- A press on a button runs the screen's Lua handler and emits `KromeEvent` to C++; a C++
  call sets a model value and the text that reads it changes. Both show in
  `--trace-events`. With Krome running, a game's state hash is what it is without it.
- The UI primitive draws the gallery's new `ui` scene (radii, 1 px and 2 px borders,
  gradients, soft shadows, nested clips, clipped glyphs, an image) and the three
  backends' pictures agree within the gallery's tolerances; `tools/render-gate.sh` passes
  on Linux (Vulkan), macOS (Metal) and Windows (D3D12).
- `krome_tests` checks layout, hit testing, overlay input order, events and the display
  list without a GPU, at scales 1, 1.5 and 2.
- A still screen makes no layout pass, no display list and no upload: over 600 still
  frames Krome's systems do nothing measurable and the renderer is handed nothing new.
  With the window's pacing on demand, an idle `kromedemo` uses no CPU to speak of and
  wakes within a frame of a pointer move.
- The same form runs in a windowed tool (`tool_main` with a window) as it does in
  `kromedemo`.
- Numbers, in the notes: layout and paint of a 2000-node screen, the primitive's GPU
  cost for 10 000 quads, a still frame's cost, on the Linux laptop.

## What the code has

- **The renderer** (`modules/render`, `docs/design/renderer.md`): sprites as rows fed by
  deltas, sorted on the GPU into buckets by layer and material, drawn by indirect runs;
  materials with eight floats of parameters and up to five game shaders; a full-frame
  post pass split at a layer. No clip rects, no per-sprite parameters. The core and the
  three backends split as "The core and the backends" says: `rw_` linked, `rc_`
  compiled in, `rb_` per backend.
- **The gallery** (`examples/gallery`): fixed scenes drawn through the render interface
  and compared against committed references per backend with `picture compare`; the
  render gate runs it.
- **Fonts** (`korvid/asset/font.h`): an atlas at one pixel size per load, a fixed
  character set, glyph metrics and kerning, a renderer texture and material per font.
- **The window** (`platform_hid`): one per process; logical sizes, physical framebuffer
  and pointer, `pixel_scale()`; mouse state and wheel, key events, typed text; action
  overrides and `input_text_override` for demos, but no pointer override.
- **The frame loop** renders every vsync; a tool (`modules/tool`) has no window.
- **Lua** (`modules/lua`): Lua 5.5 with the engine's math, errors as `longjmp`. The `kv`
  libraries (`modules/script`) are for tools.
- **Events** reach handlers through the scheduler; `EventTrace` (`--trace-events`) is a
  dev ring of delivered events with their emitters. The state hash covers tables, not
  events.
- **The `ui` module** lays out and draws game screens as sprites, crops on the CPU, and
  emits `UiEvent`. Krome takes nothing from it in this slice.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/krome/` | the module: tree, layout, paint, input, overlays, Lua state, the C++ API, `/krome` in the CLI |
| `modules/krome/lua/` | the Lua side Krome loads into its own state: the tree constructors, the model |
| `modules/render/` | the UI list: `renderer.h` calls, `rw_ui_*` (linked), `rc_ui_*` (compiled in), `shaders/ui.slang` |
| `modules/renderer-{vulkan,metal,d3d12}/` | the `rb_ui_*` functions: the pipeline, the instance buffer, the draws, scissor-free clipping |
| `examples/kromedemo/` | the form, the menu, the modal; `--demo` drives it |
| `examples/gallery/` | a `ui` scene and its three references |
| `modules/frameloop/` | on-demand pacing |
| `modules/tool/` | a window for a tool that asks for one |
| `modules/platform_hid/` | `input_pointer_override` |

### The tree

A screen is a tree of nodes in Krome's own arrays (not kinds: nothing in it is sim
state). A node has a kernel type (`box`, `text`, `image`, `hit`), its flags and
properties, an optional content string, an optional event name, an optional id (what
`KromeEvent` names it by), its children, and per-state overlays of properties (`hover`,
`pressed`, `disabled`, `selected`, `focused`). The overlays are what slice 2's styles
compile into; in this slice Lua writes them directly.

`row`, `col`, `stack` and `spacer` are not node types: they are `box` with a direction,
and an empty `box` that fills. A Lua helper spells them, as the prelude will in slice 2.

### Layout

Per axis, a size is `hug` (the default), `fill`, or a number in logical pixels, any of
them bounded by a range `lo..hi`. Two passes over the tree:

1. **Up: what each node wants.** Text and images their content; a box its children
   along its direction plus gaps, and the largest across, plus padding; ranges applied.
2. **Down: what each node gets.** A box gives fixed and hugging children what they want,
   then shares what is left equally among its fills, honouring their ranges (a fill that
   hits its maximum gives the rest back to the others), and places them by its alignment
   flags. A `stack` gives every child the whole inside and places each by its own
   alignment within it.

Rects are logical; the paint multiplies by the window's scale (`pixel_scale()` times a
Krome scale a screen may set) and rounds each edge to a whole pixel, so siblings stay
abutting and a 1 px border is one pixel at any scale. Content that does not fit is
clipped where a box says `clip`, and overflows otherwise. A box with `scroll` clips and
moves its content by the wheel; it has no scrollbar until a prelude draws one.

Layout runs only when something marked the screen: a model value a node read, a
property set from Lua, a resize, a font that turned ready, an overlay opened or closed.

### Text in this slice

Today's fonts, one line, no wrapping: a `text` node draws its string with a font asset
loaded at its pixel size times the window's scale (loaded again when the scale changes),
kerned, clipped like anything else. `{name}` in a string reads the model. Paragraphs are
slice 3.

### Paint and the display list

Painting walks the laid-out tree in order and writes UI quads into the screen's list,
then each open overlay's into its own. A box gives up to one quad (its shadow, fill and
border are one instance); a glyph one; an image one. A node's clip is the intersection
of its ancestors' clipping boxes, carried on every quad under it.

A list is rebuilt only when its screen was laid out again or a node's look changed (a
hover); an unchanged list is not handed to the renderer at all.

### The UI primitive

A new path in the renderer beside the sprites, for drawing that sprites do badly.

- **A list per layer.** `renderer_ui_set(r, layer, quads)` replaces what is drawn at
  `layer` (in the sprites' layer space: it draws after the sprite buckets of that layer
  and before those of the next), and `renderer_ui_clear(r, layer)` removes it. The list
  is copied once into a GPU buffer the layer keeps (grown when it must, the old one
  retired through the garbage queue, a copy per frame in flight while a change lands)
  and drawn every frame from there until it changes; an unchanged list costs a draw call
  and no upload. Krome's screens draw at a layer the
  screen chooses (above the post layer by default, so a game's post shader does not
  touch them).
- **Two instance forms.** In physical pixels, top-left origin. A *box* is the rect, four
  corner radii, a border width and color, a fill (solid, or a two-stop linear gradient
  with its angle), a shadow (offset, blur, spread, color), a clip rect, and optionally a
  texture slot and uv rect (an image times the fill). A *glyph* is compact: the rect, the
  uv rect, the atlas's slot, a color and the index of its clip in the list's clip table;
  a tool screen is mostly glyphs. A list holds both, in draw order.
- **One shader** (`shaders/ui.slang`): the quad expanded by its shadow's reach; coverage
  by the rounded rectangle's signed distance, anti-aliased over one pixel; the shadow as
  the closed form of a Gaussian-blurred rounded rectangle; the border as the band inside
  the edge; the clip by discarding outside the clip rect, so there are no scissor
  changes and the whole list is one draw. Premultiplied alpha. Colors are given in sRGB
  and blended in the swapchain's space, as sprites are.
- **The split.** The list's bookkeeping (layers, copies, the frame's runs) is `rw_`, in
  the linked half, tested in `render_tests`; staging into the ring and the draw's order
  in the frame are `rc_`; the pipeline, the instance buffer binding and the draw call are
  three `rb_ui_*` functions per backend. The gate is `tools/render-gate.sh`, with the
  gallery's `ui` scene added.

### Input and overlays

`krome.input` (PreSim, main thread) reads the pointer and the keys from `input.state`
and routes them top first: the topmost overlay, then the ones under it, then the screen.
Hit testing is against the laid-out rects in physical pixels, clipped. A `hit` node, or
any node with an event, takes the pointer: hover, press, release, drag (with the
pointer captured until release), wheel to the nearest `scroll` box.

An overlay is a subtree opened above a screen, anchored to a node: below it, above it,
beside it or at the pointer, moved to stay inside the window (a menu near the bottom
edge opens upward, near the right edge leftward). Light overlays close on a press
outside them, which is then swallowed, and on Escape; a modal overlay takes all input
until it is closed and dims what is under it if its tree says so. Overlays stack: a
submenu is an overlay over its menu.

### Lua, the model and events

Krome holds one Lua state of its own, made at startup with Lua's base libraries, the
engine's math, and Krome's own library. Nothing else in the process touches it.

- **A screen is a script.** `krome_open(e, "kromedemo/form.lua", model)` loads the file
  through the asset module and calls it; it returns a tree (or a function that does, run
  once). Scripts `require` others by asset path.
- **Trees are tables** in the shape `.kimb` will decode to: `{w = "box", f = {"fill"},
  p = {gap = 8}, t = "Name {name}", e = "save", id = "save", c = {...}, hover = {...}}`.
  The helpers spell it shorter: `row{gap = 8, fill = true, text "Name", button{...}}`,
  where string keys are properties, `true` values are flags and the array part is
  children.
- **The model** is a table the screen opens against. Writes go through a proxy that marks
  the nodes that read the key. C++ sets values with `krome_set(e, screen, key, value)`,
  which arrives as an engine event Krome's Lua state applies; it never writes into the
  state directly.
- **Events.** A node's event (`e = "save"`) calls the screen's handler for it in Lua with
  the node's id and value, and emits `KromeEvent{screen, node, event, value}` on the
  engine's bus for C++. Raw events (`press`, `release`, `hover`, `drag`, `wheel`) go to
  Lua handlers that ask for them. Krome's events are engine events, so `EventTrace` shows
  them; nothing Krome holds is hashed.
- **Errors** in a script are logged with the script's file and line and the screen stays
  as it was; a screen whose script fails to load opens as nothing, logged.

### Pacing, and a tool with a window

- **On demand.** `FrameLoopConfig::pacing = Pacing::OnDemand` (and `--pacing demand`):
  the loop waits for an OS event, an I/O completion or a requested frame, with the next
  timer as its deadline, instead of rendering every vsync. Anything may ask for a frame
  (`frameloop_request_frame(e)`); Krome does when it has a new list. Ticks still run at
  their rate while frames happen; between frames nothing runs. A game keeps
  `Pacing::Continuous`, today's behaviour.
- **A tool with a window.** `ToolConfig::window` opens the window and the renderer
  through the frame loop's own setup, with on-demand pacing, so a tool like goldberg can
  put up Krome screens while its systems run as they do headless.

### `/krome` in the CLI

`ls /krome` lists open screens and overlays; under a screen, its nodes by id or index
with their type, their rect, their state and the properties that applied; `print` on a
node shows them; `patch` sets a property for the length of the run. It is how the tests
and a person ask what Krome thinks is on screen.

## Out of scope

The markup, `.kim`, `.kimb`, `kim` and `kimc`; styles, themes and tokens, the prelude,
definitions, `each` and `if` (slice 2). Paragraph text, wrapping, fallback, glyphs on
demand, subpixel positioning, `edit`, the clipboard, IME, cursor shapes (slice 3).
Widgets beyond the demo's Lua helpers, focus order, shortcuts, transitions, virtualized
lists, grid areas (slice 4). Vector shapes (slice 5). Reload (slice 6). More than one
window. Accessibility and native text input. Drawing the `ui` module on the primitive.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KS1 | `krome` module: the tree, the four kernel types of this slice, state overlays, ids | P0 |
| KS2 | Layout: directions, stack, gap, pad, hug, fill, fixed, ranges, spacer, alignment, clip, scroll; pixel-rounded at any scale; tested headless | P0 |
| KS3 | The UI primitive in `render`: lists per layer, the quad, `ui.slang`, `rw_`/`rc_`/`rb_` split, `render_tests` | P0 |
| KS4 | `rb_ui_*` in Vulkan, Metal and D3D12; the gallery's `ui` scene with three references; the render gate passes on each | P0 |
| KS5 | Paint: the display list from the tree, clips carried, rebuilt only when marked, not handed over when unchanged | P0 |
| KS6 | One-line text through today's fonts at the window's scale, with `{name}` from the model | P0 |
| KS7 | Input routed top first; hit testing; hover, press, release, drag with capture, wheel to scroll | P0 |
| KS8 | Overlays: anchored, kept in the window, light and modal, stacked, outside presses swallowed | P0 |
| KS9 | Krome's own Lua state; screens as scripts by asset path; tree tables and the helpers | P0 |
| KS10 | The model with read tracking; `krome_set` from C++ as an event | P0 |
| KS11 | Events: Lua handlers, `KromeEvent` to C++, traced, never hashed; a test that the state hash is unchanged | P0 |
| KS12 | `Pacing::OnDemand` in the frame loop; `frameloop_request_frame` | P0 |
| KS13 | `ToolConfig::window`: a tool with a window and Krome in it | P1 |
| KS14 | `input_pointer_override` for demos and tests | P0 |
| KS15 | `examples/kromedemo` with `--demo` | P0 |
| KS16 | `/krome` in the CLI | P1 |
| KS17 | `modules/krome/README.md`; numbers in `docs/design/krome-notes.md` | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **The tree and layout** (KS1, KS2). The module, a C++ builder for tests, layout and
   its tests at three scales. Nothing drawn.
2. **The primitive** (KS3, KS4). The render interface, the shader, the Vulkan backend,
   the gallery scene and its Vulkan reference; Metal and D3D12 written here and run
   through the render gate on their machines before the slice closes.
3. **Lua and the model** (KS9, KS10, KS11). Krome's Lua state, scripts as screens, the
   tree tables and helpers, the model, events both ways, the trace, the hash test.
4. **Paint, input and overlays** (KS5 to KS8, KS14, KS15). The display list, text, hit
   testing, states, scroll, overlays; `kromedemo` and its `--demo`.
5. **Still, and close** (KS12, KS13, KS16, KS17). On-demand pacing, the tool window,
   `/krome`, the README, the numbers, the master PRD corrected where this slice proved
   it wrong.

## Decided

Reviewed Nov 15; each was a question in the first draft.

1. **A compact glyph form** from the start, beside the box form.
2. **Lists draw in the sprites' layer order**, after that layer's buckets.
3. **`id` is a property of its own**, separate from the event name.
4. **No scrollbar** in this slice; the scrollbar is slice 4's.
5. **Model values are scalars** (numbers, strings, booleans) in this slice.
6. **Pacing and the tool window stay in this slice.**

## As built

Nov 15, 2026, on branch tool-stuff; Linux proved. Numbers in `docs/design/krome-notes.md`.

**Milestone 1, the tree and layout** (0b81565). As planned. Ranges are bounds and set no
mode, so `fill` with a range still fills; minimums are pinned before maximums.

**Milestone 2, the primitive** (2d6e97f). On Vulkan, with the gallery's `ui` scene and its
reference; the render gate passes on Linux. The lists live in one host-visible buffer per
frame slot, rewritten before recording only when they changed (a bigger buffer rewrites
the slot's descriptor set, which a recording command buffer must not see). The push
block's spare words carry a draw's region and count, so the sprite shaders compile to the
same bytes; the render gate now allows new shaders and holds the old ones byte-identical.
**Not done: Metal and D3D12** have stubs that draw no UI; their `rb_ui_buffer` and
`rb_ui_draw`, and the gallery references, wait for those machines.

**Milestone 3, Lua and the model** (840a331). As planned. The module's state is
`KromeModule` (`KromeState` was taken by the looks' states). A node made in a tail call
(`return col { ... }`) has no line from Lua and takes its first child's. Numeric strings
are numbers. Screens cannot `require` another file yet.

**Milestone 4, paint, input and overlays** (2098661). As planned, and `kromedemo --demo`
passes with no validation message. Additions: `looks = { ... }` in a helper's table, for a
look whose state shares its name with a flag (`disabled`); `krome_find_rect`, which looks
in overlays; `input_pointer_override` in `platform_hid`; goldberg's conventions now land
`.lua` as a script. Overlay texts are filled once, when the overlay opens. Lua's raw
events (press, release, drag) are not given to handlers yet; captures and clicks are.

**Milestone 5, still, and close.** `Pacing::OnDemand` (`--pacing demand`) and
`engine_want_frame`; `/krome` in the CLI; the README, the notes. **Not done: KS13, a tool
with a window.** `tool_run` ticks as fast as the work allows. A window on it either spins
a core when idle, or throttles the work to the display: a render at vsync every tick caps
goldberg's cooking at 60 ticks a second, and sleeping between frames slows it the same
way. Which a tool wants (render on its own thread, a frame every N ticks, or pace on
demand only while the work is idle) is a decision for goldberg's window, which is the only
customer, and is left for that slice.

**After the slice, KS13** (Nov 15): settled by cycles (`docs/design/cycles.md`) rather than
`ToolConfig::window`. A tool's work goes in the Work cycle and Krome in the Frame cycle; a
tool with a window is the frame loop with both, the work filling the time between frames
(`kromedemo --work`: 86% of the wall time to the work, the window drawing meanwhile). What
remains for goldberg's window is running its command line through the frame loop.

