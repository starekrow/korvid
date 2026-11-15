# krome

UI for tools and dialogs: screens laid out with rows, columns and stacks, styled per node
and per state, drawn on the renderer's UI primitive, written in markup (`.kim`, loaded as
`.kimb`) or Lua, wired up in a Lua state of Krome's own. To anyone outside it is Korvid UI.
The design is `docs/design/krome.md`; what is built and what is next is
`docs/PRDs/Krome-Master-PRD.md` and its slices (1: the kernel; 2: the markup; 3: text).

## How it works

**Three layers.** A kernel in C++ (four node types, layout, looks, paint, input), a
prelude in markup (the widgets, the styles, the themes: `krome/std`, this module's asset),
and content in markup. Nothing in the kernel knows a widget: a button is a definition.

**The tree** (`korvid/krome/tree.h`). A screen is a tree of five kernel types: `box`,
`text`, `image`, `hit`, `edit`. A node has flags (`fill`, `clip`, `scroll`, `disabled`,
`selected`, `hidden`, the alignments), properties set by name and refused with a reason,
a content string, an id, an event, a binding, and a look per state (`hover`, `pressed`,
`focused`, `disabled`, `selected`) over its base look. Text color, font and size inherit.

**Layout** is two passes: up for what each node wants, down for what it gets. Per axis a
size is `hug`, `fill` or a number, bounded by a range (`"120..400"`); fills share what is
left, minimums pinned before maximums; a bare `fill` fills along the parent's direction.
Rects are logical pixels; painting multiplies by the window's scale and rounds each edge.
Layout runs only when something marked the tree.

**Text wraps** (slice 3) at the width layout gives it: a hugging node is no wider than its
room unless it cannot be narrower (its `narrowest`: a wrapping text's longest word), and
when any text wrapped, heights are laid out again at the widths given and the tree placed
again. A tree with no wrapped text lays out as before. `nowrap` keeps a text on one line;
its `left`, `center` and `right` align its lines; `line-height` (times the size, inherited)
spaces them. Paragraphs are `typeset`'s (`modules/typeset`); glyphs are made at the exact
pixel size the window's scale gives, when first drawn, from a fallback font where the first
lacks one (`modules/asset`, "Fonts"). `subpixel` keeps a text's pens to a quarter pixel.

**`edit`** (slice 3) holds text a person types: one line, or many with `multiline`;
`readonly`; `placeholder="..."` while empty; `@name` binds its text both ways; `-> event` on
Enter (Ctrl+Enter in many lines), `change=` once a frame it changed in, `blur=` when it loses
the keyboard. A press gives it the keyboard (the `focused` look); a double press selects a
word, a triple a line, a drag a selection; a press elsewhere, Escape or closing its screen
takes it away. Keys move, select and erase by character or word (Ctrl, or Option on a Mac),
by line and page in many lines; Ctrl or Command with A, C, X, V, Z, Shift+Z and Y; the
clipboard is the system's (`platform_clipboard_*`). An input method's composition is drawn
underlined at the caret, and the input method told where it began. It clips its text and
scrolls to keep the caret in view; the caret blinks while the window has focus, waking an
on-demand window at each turn (`engine_want_frame_at`). The focus is found again by id when
its tree is built again. `cursor=` sets the pointer's shape (an edit's is the I-beam).

**Documents.** A `.kimb` (`modules/kim` compiles it from `.kim`; goldberg does that as it
cooks) is read once, through the I/O queue or from what `krome_provide` gave, and kept:
`modules/kimb` reads it, with each node's line from the sidecar. A screen's names resolve
in its file, then each `use` in order (and theirs), then the kernel; `krome_open(e, file,
screen)` waits until the file, its preludes and its scripts are all in.

**The one builder** (`src/build.cc`) takes a node of a document, a `.kimb`'s or a Lua
script's tables made into one, and builds the kernel's tree:

- **Styles** (`[button primary]`) merge left to right from the scope, a style's bases
  first; the node line goes over them. A style's state blocks are looks.
- **Tokens.** A word a theme defines is its value there (`bg=panel`): resolved when the
  tree is built, so `krome_theme` (or Lua's `theme("light")`) rebuilds every screen and
  its overlays from the kept documents, reading nothing again.
- **Definitions.** A word that is a `def` is its body, expanded where it is used: its
  parameters (strings in order, or by name) fill `{name}` in the body's texts; everything
  else on the use line (flags, properties, styles, the id, the event, the binding) goes onto
  the body's root over what the body says; the use's children go where the body's `slot`
  is. A mistake inside names the body's line and the use's.
- **`each item in list`** builds its block once for each element of a model list, and
  `{item.field}` reads it; **`if name`** builds its block when the value is truthy. Each
  sits after a hidden anchor; when the model key its path starts with changes, what follows
  the anchor is built again and nothing else.
- **`@name`** binds a node's `selected` state to the model; a press flips the value before
  the event's handler runs.

**Lua.** Krome holds a Lua state of its own (the engine's allocator and math; no files).
A screen is a script returning its tree, or `{ root, model, on, uses }`; or a `.kimb`
screen whose `script` lines name Lua files returning `{ model, on }`, run in order in one
environment, their models merged and their handlers merged (an event handled twice is an
error naming both files). The helpers every script sees (`row`, `col`, `stack`, `box`,
`text`, `image`, `hit`, `spacer`) make nodes in the shape a `.kimb` holds: string keys are
properties, `true` is a flag, the array part is children and a content string, `id` and
`event` are what they say, `style = "a b"` its styles, and a state's name with a table
(or `looks = { hover = {...} }`) its look. `kim("file.kimb", "screen")` is a piece of markup
in a script's tree, built in that file's scope.

**The model** is a table the scripts see as `model`. Writing a key refills every text that
reads it (`"Saved {saves} times"`), sets every binding on it, and rebuilds every `each` and
`if` on it; a list changed in place says so with `changed("key")`. C++ writes scalars with
`krome_set` (a `KromeSet` event) and lists and tables with `krome_set_doc` (KBOR).

**Events.** A press goes to the nearest node with an event, a binding or a `hit`, captured
until the release; a release over it flips its binding, runs the screen's handler
(`on.save(id, value)`) and emits `KromeEvent{screen, node, event, value}`. A script emits
its own with `emit(name, value)`. Krome is cosmetic: none of it is hashed, and its events
are traced (`--trace-events`).

**Overlays.** `open_overlay(tree, { anchor = id, side = "below", modal = false, dim =
color })` opens a subtree over the screen (a table, or `kim(...)`), anchored to a node
(below, above, right, left, at the pointer), flipped and held inside the window. Input goes
top first. A light overlay closes on a press outside it, and the press goes no further; a
modal takes all input until `close_overlay()`; Escape closes the top one. An overlay is a
tree of its own and inherits nothing from the screen under it.

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

```
// assets/form.kim
use "krome/std"
script "mygame/form.lua"

screen form {
    col [page] pad=16 gap=8 {
        field "Name" "{name}"
        checkbox "Wrap" @wrap
        row gap=8 {
            button "Save" id=save -> save
            button "Quit" id=quit -> quit [danger]
        }
        each item in assets {
            text "{item.name}"
        }
        if dirty { badge "unsaved" }
    }
}
```

```lua
-- assets/form.lua
return {
  model = { name = "Rowan", wrap = true, dirty = false, assets = { { name = "a.png" } } },
  on = { save = function() model.dirty = false end, quit = function() emit("quit") end },
}
```

```cpp
Name form = krome_open(e, "mygame/form.kimb", "form");
krome_set(e, form, "name", "Ada");
krome_theme(e, "light");
for (const KromeEvent& ev : ctx.received<KromeEvent>()) if (ev.event == name_intern("quit").hash) ...
```

A screen can still be all Lua (`krome_open(e, "mygame/form.lua")`, slice 1's way).
`examples/kromedemo` is a whole screen both ways: `form.kim` and `form_lua.lua` draw the
same pixels; `--demo` clicks through it and checks the model.

## Quick reference

Header `korvid/krome/krome.h`:

| name | kind | what |
|---|---|---|
| `krome_open(e, file, screen, layer)` | function | the screen of a `.kimb`, named `"<file>#<screen>"` |
| `krome_open(e, script, layer)`, `krome_open_source(e, name, source, layer)` | functions | a screen from a script; its name |
| `krome_close`, `krome_ready`, `krome_tree`, `krome_find`, `krome_find_rect` | functions | the screen and its nodes |
| `krome_set(e, screen, key, value)`, `krome_set_bool`, `krome_set_doc(e, screen, key, kbor, size)` | functions | a model value from C++; a list or table as KBOR |
| `krome_model_text(e, screen, key)` | function | a model value as text shows it |
| `krome_press(e, screen, id)` | function | a node's binding and event, as a press would fire them |
| `krome_theme(e, name)`, `krome_current_theme(e, screen)` | functions | every screen in a theme; the one a screen is in |
| `krome_provide(e, path, data, size)` | function | a file's bytes from memory, in place of reading it |
| `krome_errors(e)`, `krome_clear_errors(e)` | functions | the latest mistakes, with files and lines |
| `krome_settled(e)` | function | nothing loading, fonts in, every change painted |
| `krome_viewport(e, w, h, scale)` | function | the view's size without a window |
| `krome_stats(e)` | function | layouts, paints, lists handed over, events, errors, regions and nodes rebuilt |
| `KromeEvent`, `KromeSet` | events | out and in |
| `krome.pump`, `krome.input`, `krome.paint` (Frame) | systems | documents, scripts, rebuilds and model values; the pointer; layout and paint |
| `/krome` | CLI mount | `ls` screens and named nodes, `print` a tree or a node, `patch NODE PROPERTY VALUE...` |

In Lua: the helpers (and `edit`, with `bind = "key"`), `model`, `emit(name, value)`,
`open_overlay(tree, opts)`, `close_overlay([id])`, `theme(name)`, `changed(key)`, `kim(file, screen)`.

Properties: layout `w`, `h`, `pad`, `gap`, `dir`, `anchor`; an edit's `placeholder`,
`change`, `blur`; `cursor`; look `bg`, `bg2` (a gradient to it), `angle`, `radius` (one, two
or four), `border`, `border-color`, `shadow` (x y blur [spread]), `shadow-color`, `color`,
`opacity`, `font`, `size`, `line-height`, `image`, `tint`, `caret-color`, `selection-color`.
Flags: `fill`, `clip`, `scroll`, `disabled`, `selected`, `hidden`, the alignments,
`subpixel`, `nowrap`, `multiline`, `readonly`. The
vocabulary is fixed (`modules/kimb`); `prop name type` in a prelude types a definition's
parameter.

**`krome/std`** (`assets/std.kim`): themes `dark` (first, so a screen starts in it) and
`light`; `row`, `col`, `stack`, `spacer`, `separator`, `label`, `heading`, `field`, `badge`,
`button` (and `[danger]`), `checkbox` (bind it: `checkbox "Wrap" @wrap`), `input` and
`textarea` (bind them: `textarea @notes placeholder="Notes"`), `menu-item`, `panel`; styles `page`, `menu`, `panel`, `list`, `list-row`. A game's own `def` or `style`
of the same name wins, and its `theme dark` overrides the tokens it names.

## Behaviour worth knowing

- **A bare `fill` is along the parent's direction.** A row in a column fills its height
  with `fill`, its width only with `w=fill`.
- **One node to a line** in markup; `row { text "a" slot }` is a text with a flag `slot`.
- **A token wins over a literal spelled the same**, and a word that is neither is an
  error naming the theme.
- **Definitions are expanded at load**, every time a screen is built: a prelude swapped
  since the screen was cooked is the one used.
- **A list changed in place is not seen**: assign it again, or `changed("key")`.
- **Overlay texts are filled once**, when the overlay opens; the model does not refill them,
  and an each or if in one is built once.
- **A rebuilt region leaves dead nodes** in the tree's array; past as many as live ones,
  the tree is built whole again. Scroll offsets survive a rebuild by id.
- **`return col { ... }` is a tail call**, which Lua gives no line: such a node takes its
  first child's.
- **One edit has the keyboard** across every screen. Tab order and keyboard focus without a
  pointer are slice 4's.
- **No shaping** beyond kerning, left to right only.
- **Screen scripts cannot `require`** another file; a `.kimb` screen takes several scripts.
- **Metal and D3D12 draw no UI yet**: their backends have stubs for the primitive.

## Not in this module

The parser (`modules/kim`) and the document's meaning (`modules/kimb`), which this links;
paragraphs and the edit buffer (`modules/typeset`); fonts and their pages (`modules/asset`);
typed text, composition, the clipboard and the cursor (`modules/platform_hid`). The widget
set and focus order (slice 4), vector shapes (5), reload (6).
A tool with a window is the frame loop with the tool's work in the Work cycle
(`kromedemo --work`).
