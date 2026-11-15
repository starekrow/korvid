# krome

UI for tools and dialogs: screens laid out with rows, columns and stacks, styled per node
and per state, drawn on the renderer's UI primitive, wired up in a Lua state of Krome's
own. To anyone outside it is Korvid UI. The design is `docs/design/krome.md`; what is
built and what is next is `docs/PRDs/Krome-Master-PRD.md` and its slices. This is slice 1:
screens are Lua tables; the markup (`.kim`, `.kimb`) is slice 2.

## How it works

**The tree** (`korvid/krome/tree.h`). A screen is a tree of four kernel types: `box`,
`text`, `image`, `hit`. A node has flags (`fill`, `clip`, `scroll`, `disabled`,
`selected`, `hidden`, the alignments), properties set by name and refused with a reason,
a content string, an id, an event, and a look per state (`hover`, `pressed`, `focused`,
`disabled`, `selected`) over its base look. Text color, font and size inherit.

**Layout** is two passes: up for what each node wants, down for what it gets. Per axis a
size is `hug`, `fill` or a number, bounded by a range (`"120..400"`); fills share what is
left, minimums pinned before maximums; a bare `fill` fills along the parent's direction.
Rects are logical pixels; painting multiplies by the window's scale and rounds each edge.
Layout runs only when something marked the tree.

**Lua.** Krome holds a Lua state of its own (the engine's allocator and math; no files).
A screen is a script, read through the I/O queue (`krome_open`) or given as source; it runs
in an environment of its own and returns its tree, or `{ root, model, on }`. The helpers
every script sees (`row`, `col`, `stack`, `box`, `text`, `image`, `hit`, `spacer`) make
nodes in the shape `.kimb` will decode to and note the line each was written on. In a
helper's table: string keys are properties, `true` is a flag, the array part is children
and a content string, `id` and `event` are what they say, a state's name with a table is
its look, and `looks = { hover = {...}, disabled = {...} }` holds looks beside flags of
the same names.

**The model** is a table the screen's script sees as `model`. Writing a key refills every
text that reads it (`"Saved {saves} times"`). C++ writes with `krome_set`, which arrives as
a `KromeSet` event.

**Events.** A press goes to the nearest node with an event (or a `hit`), captured until the
release; a release over it runs the screen's handler (`on.save(id, value)`) and emits
`KromeEvent{screen, node, event, value}`. A script emits its own with `emit(name, value)`.
Krome is cosmetic: none of it is hashed, and its events are traced (`--trace-events`).

**Overlays.** `open_overlay(tree, { anchor = id, side = "below", modal = false, dim =
color })` opens a subtree over the screen, anchored to a node (below, above, right, left,
at the pointer), flipped and held inside the window. Input goes top first. A light
overlay closes on a press outside it, and the press goes no further; a modal takes all
input until `close_overlay()`; Escape closes the top one.

**The Frame cycle.** Krome's systems run in `Phase::Frame` (docs/design/cycles.md): once
before each frame drawn, never in a tick, so nothing Krome does is in a replay. Presses,
releases, the wheel and Escape arrive as events, so a click between two frames is kept.

**Drawing.** `krome.paint` lays out what changed and paints the screens of each
layer into one `UiList` (`korvid/render/ui.h`): a box per node with a background, border,
shadow or image, a glyph per character from the asset font at the size times the scale,
clips for `clip` and `scroll`. A layer whose screens did not change is not painted and
not handed over; a still screen costs nothing. A new list asks the loop for a frame
(`engine_want_frame`), so a window paced on demand shows it.

## Using it

```lua
-- assets/form.lua
return {
  model = { name = "Rowan", saves = 0 },
  on = { save = function() model.saves = model.saves + 1 end },
  root = col { pad = 16, gap = 8, bg = "#10131a", color = "#e6e9ef",
    text "Name: {name}",
    hit { id = "save", event = "save", pad = { 14, 8 }, radius = 6, bg = "#2f6fed",
          hover = { bg = "#4a84f5" }, text "Save" },
    text "Saved {saves} times",
  },
}
```

```cpp
Name form = krome_open(e, "mygame/form.lua");
krome_set(e, form, "name", "Ada");
for (const KromeEvent& ev : ctx.received<KromeEvent>()) if (ev.event == name_intern("save").hash) ...
```

`examples/kromedemo` is a whole screen: a form, a scrolled list, a menu and a modal;
`--demo` clicks through it and checks the model.

## Quick reference

Header `korvid/krome/krome.h`:

| name | kind | what |
|---|---|---|
| `krome_open(e, script, layer)`, `krome_open_source(e, name, source, layer)` | functions | a screen from a script; its name |
| `krome_close`, `krome_ready`, `krome_tree`, `krome_find`, `krome_find_rect` | functions | the screen and its nodes |
| `krome_set(e, screen, key, value)`, `krome_set_bool` | functions | a model value from C++ |
| `krome_model_text(e, screen, key)` | function | a model value as text shows it |
| `krome_press(e, screen, id)` | function | a node's event, as a press would fire it |
| `krome_viewport(e, w, h, scale)` | function | the view's size without a window |
| `krome_stats(e)` | function | layouts, paints, lists handed over, events, script errors |
| `KromeEvent`, `KromeSet` | events | out and in |
| `krome.pump`, `krome.input`, `krome.paint` (Frame) | systems | scripts and model values; the pointer; layout and paint |
| `/krome` | CLI mount | `ls` screens and named nodes, `print` a tree or a node, `patch NODE PROPERTY VALUE...` |

In Lua: the helpers, `model`, `emit(name, value)`, `open_overlay(tree, opts)`, `close_overlay([id])`.

Properties: layout `w`, `h`, `pad`, `gap`, `dir`, `anchor`; look `bg`, `bg2` (a gradient to
it), `angle`, `radius` (one, two or four), `border`, `border-color`, `shadow` (x y blur
[spread]), `shadow-color`, `color`, `opacity`, `font`, `size`, `image`, `tint`.

## Behaviour worth knowing

- **A bare `fill` is along the parent's direction.** A row in a column fills its height
  with `fill`, its width only with `w = "fill"`.
- **`return col { ... }` is a tail call**, which Lua gives no line: such a node takes its
  first child's.
- **Overlay texts are filled once**, when the overlay opens; the model does not refill them.
- **Text is one line**, in today's fonts. Paragraphs, wrapping and `edit` are slice 3.
- **Screen scripts cannot `require`** another file yet.
- **Metal and D3D12 draw no UI yet**: their backends have stubs for the primitive.

## Not in this module

The markup and the prelude (slice 2), paragraph text and editing (3), the widget set (4),
vector shapes (5), reload (6). A tool with a window is the frame loop with the tool's work
in the Work cycle (`kromedemo --work`).
