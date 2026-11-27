# Krome — Master PRD

Nov 15, 2026 · @starekrow · draft, first review folded in

## What it is

Krome is Korvid's UI library for tools and dialogs: screens written in a small markup, laid
out with rows, columns and constraints, styled by name, wired up in Lua, drawn by the
engine's renderer. To anyone outside it is Korvid UI, powered by its Krome engine. The
design is `docs/design/krome.md`; this document turns it into something to build, adds
the file formats the design left implied, and settles enough of its open questions to
start. Everything in the design is in scope except the Figma import.

Today there is nothing. The `ui` module draws game screens (menus, HUDs, dialogue) from
`.ui.json` documents as screen-space sprites; the `text` module draws one line of up to
64 bytes; the renderer draws textured quads with no clip rects; the platform opens one
window. goldberg's window (`docs/PRDs/Goldberg-Master-PRD.md`, G16, "The window") is
waiting for Krome, and is its first customer.

**Two forms of a screen.** A screen is written as text, `.kim` (Korvid Interface Markup),
and loaded as binary, `.kimb`: the same document in a canonical KBOR encoding
(`docs/design/kbor.md`). The runtime reads only `.kimb`. Turning text into binary is a
module of its own, `kim`, which goldberg uses to cook screens and which a game links only
if it wants to read the text form itself (a mod loader, a console that opens a screen
typed at it).

## Goals

- **Tools that look deliberate.** Dense, resizable, text-heavy screens that do not look
  like a debug overlay: a flex layout, rounded rectangles with borders and soft shadows,
  gradients, clipping, good text.
- **Small kernel, growing text.** Five node types in C++ (`box`, `text`, `image`, `hit`,
  `edit`); every widget is markup and Lua in a prelude a game can replace.
- **Binary at runtime.** Krome loads `.kimb` and nothing else. No parser, no text, no
  XML in a shipped game unless it asks for the `kim` module.
- **Mistakes name their line.** Every node keeps its source file and line through
  definition expansion and style resolution, in both forms.
- **Reload is a rebind.** Screens, preludes, styles, themes and shapes are assets held by
  handle; Krome redraws when one rebinds and does not care who rebound it.
- **Free when still.** A screen that is not changing costs no layout, no paint and sends
  the renderer nothing; a tool window with nothing happening sleeps.

## Non-goals

- The Figma import (`docs/design/krome.md`, "Figma import"). A separate PRD if it happens.
- A visual editor.
- Animation beyond transitions between style states.
- Accessibility, right-to-left text, and IME beyond what `edit` needs. Each needs its own
  design before a tool ships to anyone but us.
- LCD subpixel anti-aliasing.
- Replacing `ui`. The two stay separate; whether they share anything is an open question.
- Multiple windows in the first slices (see Open).

## Frame

- **Modules.**

  | module | holds | who links it |
  |---|---|---|
  | `krome` | the kernel nodes, layout, paint, input, styles, the prelude's meaning, the Lua bridge, the `.kimb` loader | any tool or game with Krome screens |
  | `kimb` | the `.kimb` document and what its names mean: reading it with its lines, the scope, the kernel's vocabulary, the check | `krome`, `kim`, `kimc`, goldberg |
  | `kim` | the `.kim` parser and the `.kim` to `.kimb` compiler; reports what a file references | goldberg, `kimc`, and a game that reads text |
  | `tools/kimc` | the command line over `kim` and `kimb`: compile, check (format waits) | people and scripts |

  `kimb` depends on `core` only (KBOR is core's), and `kim` on `core` and `kimb`, so either
  can sit in goldberg, a tool or a game without bringing Krome. `krome` links `kimb`, so a
  mistake is found by the same rules everywhere, and does not depend on `kim`.
- **Lua for behaviour.** A prelude is markup and Lua; a screen's logic is a Lua function
  that returns a tree. A widget's behaviour may also be C++, registered by a module
  (`krome_behaviour`), with the same calls (slice 4). Lua is `modules/lua`, with the engine's math, so a handler's
  arithmetic is the same on every platform.
- **Its own Lua.** Krome holds a Lua state of its own, separate from any other in the
  process. It talks to the rest of the engine, and to other Lua states, through events.
- **Cosmetic by default.** Krome's nodes, its Lua state and its events are never part of
  the state hash, as `text.glyph` and `frameloop.overlay` are not. Its events are traced
  (they show in the engine's trace and the CLI, so a session can be followed) but not
  hashed. That is wrong for a game that drives its sim from Krome, since the input log
  does not hold Krome's pointer and keyboard; that case is deferred.
- **On the renderer, not around it.** Krome draws through the render interface like
  everything else, on a primitive added there for this kind of drawing (see Drawing), in
  every backend.

## The pieces

### `.kim`, the text form

The syntax is the design's ("The syntax"): one node per line, a word, then flags,
properties, ranges, a content string, a binding, an event and styles in any order, then
an optional block in braces. To that this PRD adds:

- **Comments** are `//` to the end of the line. `#` is taken by colors.
- **A file says what it uses.** `use "krome/std"` at the top names a prelude (or any
  other `.kim` with definitions and styles) by asset path. Names resolve against the
  file's own definitions, then its uses in order, then the kernel. This is also what
  goldberg needs to know which screens to cook again when a prelude changes.
- **Top-level forms.** A file holds any of: `use`, `prop`, `style`, `theme`, `def`, and
  `screen NAME { ... }`.
- **A prelude is a `.kim` file**, one with no screens, cooked to `.kimb` like any other.
  The engine ships a default, `krome/std`, as assets of the `krome` module: the
  properties, the standard widgets, a light and a dark theme. An author uses it, uses it
  and overrides parts of it, or uses something else entirely; nothing in the kernel
  knows which.
- **Themes are tables.** `theme dark { panel2=#202430 accent=#5aa0ff ... }`. A theme
  names tokens; a style's values are tokens or literals.

### `.kimb`, the binary form

The `.kim` file compiled: the same document, nothing resolved that needs another file.
It is KBOR, normalized, so a screen with a hundred `row`s pays a byte for each repeated
key.

- **Canonical.** The same document gives the same bytes. Comments and white space do not
  survive. Within a node line, properties are written in name order and a repeated one
  takes its last value, so reordering a line changes nothing downstream and an identical
  cook is identical (goldberg's G7).
- **One file in, one file out.** Each `.kim` cooks to its own `.kimb`; `use` stays a
  reference, resolved at load. Nothing collapses a screen and its preludes into one file
  for now.
- **Lines in a sidecar.** The tree holds no line numbers. Beside it, the file holds one
  array of lines, one entry per node in the order a walk of the tree meets them, so a
  node's line is found by its position. The array can be stripped (a ship cook, a
  `kimc strip`) or ignored, and the tree's bytes do not change either way; an error with
  no array to look in names the file and the node's path instead.
- **One shape for every source.** A `.kimb` decodes to the tree a Lua table constructor
  builds for the same screen, so `krome_open(screen_kimb)` and
  `krome_open(lua_table)` are the same call underneath, and the Lua form can be tested
  before the markup exists.
- **Versioned.** The root map carries `"kim": 1`. A runtime refuses a version it does
  not know rather than guessing.

A sketch of the shape (field names to be settled in slice 2):

```
{ "kim": 1, "file": "goldberg/ui/inspect.kim",
  "uses": ["krome/std"],
  "props": [...], "styles": {...}, "themes": {...}, "defs": {...},
  "screens": { "inspect": <node> },
  "lines": [3, 4, 5, 9, ...] }

node: { "w": "row", "f": ["fill"], "p": {"gap": 8}, "s": ["toolbar"],
        "t": "Name {name}", "r": [1, 99], "b": "name", "e": "save", "c": [<node>...] }
```

### The kernel

The design's five nodes, in C++: `box` (lays out children; background, border, shadow;
clips and scrolls), `text` (a run, wrapped to its width), `image` (a texture or a shape),
`hit` (pointer and focus, raw events), `edit` (an editable line or block). Content cannot
tell a kernel word from a prelude word.

### Overlays

Menus, dropdowns, context menus, tooltips, popovers and modals are not widgets a prelude
can build from boxes alone: they draw above everything, escape their parent's clip, take
the pointer first and own the focus while open. So the kernel has layers.

- **A screen has an overlay stack.** `open_overlay(node, anchor)` puts a subtree above
  the screen, positioned against a node (below it, beside it, at the pointer) and kept
  inside the window: a dropdown near the bottom edge opens upward.
- **Hit testing and focus go top first.** A modal overlay takes all input and holds the
  focus in itself until it closes; a light one (a menu, a tooltip) closes on a press
  outside it or on Escape, and the press does not reach what is under it.
- **Overlays are markup too.** A menu is a `def` whose body is opened as an overlay by
  its handler; the prelude decides what a menu looks like, the kernel only where it goes
  and who gets the input.

### Layout

As the design has it: `row`, `col`, `stack`; `gap`, `pad`; per axis `hug` (the default),
`fill` or a number, any of them bounded by a range; `spacer`; alignment flags per axis.
One pass down for constraints and one up for sizes, every frame something changed, and
nowhere else. Layout is a pure function of the tree, the styles and the window size, so
it is tested without a GPU.

### Styles, themes and the prelude

As the design has it: named styles, never selected; merged left to right; states
(`hover`, `pressed`, `focused`, `disabled`, `selected`) inside the style; values are
theme tokens; the node line wins over the bracket; properties declared by the prelude
with a type and whether they inherit. Transitions between style states are a property
(`transition=0.12`), the only animation in scope.

Definitions are named subtrees with parameters and one slot for children. `each` and
`if` are the only control flow; anything more is a Lua function returning a tree.

### State, events and Lua

A screen opens against a model (a table in Krome's Lua state). `@name` binds two ways,
`{name}` reads into text, and a change to the model marks what read it for layout and
paint. `-> save` emits `save` with the node and its value to Lua; the kernel supplies the
raw events (press, release, drag, wheel, key, focus, text).

Krome's Lua state is its own. Everything that crosses out of it is an event: a game's
C++, or another Lua state, hears Krome's named events as an engine event (as it hears
`UiEvent` from `ui`), and sends Krome engine events that its Lua handles (a model value
changed, a screen to open). Nothing reaches into Krome's tables from outside.

### Drawing

A new primitive in the render interface, made for this kind of drawing rather than bent
out of sprites. Sprites are entities with placements, interpolation and sort keys, fed as
deltas, with their look in a shared material; UI is thousands of small rectangles whose
look differs per rectangle, drawn in painter's order, clipped, and changed all at once
when it changes at all.

- **One instance, one look.** A UI quad carries its own rectangle, per-corner radii,
  border width and color, a solid or two-stop gradient fill, a soft shadow (offset, blur,
  spread, color), a texture rect (for glyphs, images and shapes, from an atlas) and a clip
  rectangle. One shader draws all of it by signed distance.
- **A list, not a delta stream.** Krome hands the renderer one list per window (per
  layer, for overlays) in draw order. The list is rebuilt only when something on it
  changed, and an unchanged list is not sent again: a still screen costs the renderer
  nothing.
- **Pixel-exact.** Positions in physical pixels at the window's scale, no interpolation,
  no world space. Edges anti-aliased by the distance, so a 1 px border is 1 px.
- **Every backend.** Vulkan, Metal and D3D12, held to `tools/render-gate.sh` like the
  sprite path. The `ui` module could draw on it later; nothing makes it.

### Text

The largest piece of work, and not Krome's alone: paragraph text belongs to the `text`
module, where a game's labels and `ui` get it too, and Krome is one customer.

- **Paragraphs.** Wrapping at word boundaries (and inside a word too long for its line),
  alignment, line height, the measure of a run without drawing it, and the caret and
  selection geometry `edit` needs. A game's `Text` asks for a width and wraps; today's
  one line of 64 bytes stays the cheap default.
- **Glyphs on demand.** A glyph cache filled as text needs it, in pages, at the exact
  pixel size the window's scale gives (today a font rasterizes a fixed set up front);
  font fallback down a list when a glyph is missing; kerning.
- **Subpixel positioning, when asked for.** Opt-in per font or label: glyphs rasterized
  at fractional pen offsets (a few per pixel, cached like any other glyph), so small text
  spaces evenly instead of snapping each glyph to a whole pixel. Anti-aliasing stays
  grayscale. LCD subpixel anti-aliasing (coverage per color channel, ClearType's kind) is
  not planned: it needs the panel's channel order, dual-source blending in every
  backend and a known background, is wrong on a rotated, scaled or transparent label, and
  is easy to get subtly wrong.
- **Out of the first version:** shaping beyond kerning (ligatures, complex scripts) and
  right-to-left (see Open).

### Native text and accessibility

To act like a native text box, `edit` has to speak to the OS on two channels the engine
does not have today. Neither is the key and text events `platform_hid` delivers now.

- **The text input protocol.** The OS's text system talks to the focused field and asks
  it things: the text around the caret, where a range is on screen, replace this range
  with that. NSTextInputClient on macOS, UITextInput on iOS, TSF on Windows, text-input-v3
  on Wayland, InputConnection on Android. It is what makes IME composition, dictation,
  autocorrect, the accent menu, the emoji picker and a soft keyboard's suggestions work.
  SDL3 handles part of it internally (composition text, the candidate window's position)
  but gives the app no way to answer for the text around the caret. The iOS and Android
  ports are native already, so there the engine owns the view that would answer.
- **The accessibility tree.** A screen reader (VoiceOver, Narrator, Orca, TalkBack) does
  not read input; it pulls a tree of elements from the app (role, name, value, bounds,
  state, actions) through UIA, NSAccessibility and UIAccessibility, AT-SPI or Android's
  node info, and the app sends notifications when they change. Queries in, events out,
  often on the OS's thread.

Krome is a good fit for both, since every node is named, typed and laid out by the
kernel: a `text` is a static text, a `hit` with `-> press` is a button, an `edit` is a
text field. The first version does what SDL3's composition events allow (K15); the text
protocol per platform and the accessibility tree are a design of their own, after the
widgets exist (K25, K26).

### Vector shapes

As the design has it: a shape asset of paths, fills and strokes; SVG converted by the `svg`
module (C++, run by goldberg in process; decided in the slice 5 PRD), never read at runtime; rasterized on the CPU at the exact pixel size and
cached by shape and size; the stated subset (cubics and arcs flattened, both fill rules,
strokes with joins, caps and dashes expanded to fills, solid, linear and radial paint,
transforms, clip paths, group opacity). The shape asset is KBOR too (`.kshape`).

**The rasterizer is the engine's own**, a module of its own (`patherizer`)
that depends on `core` and `math` only, and is not Krome's. stb_truetype is left alone:
its rasterizer is shaped for glyph outlines, and SVG needs more of one anyway. `patherizer`
fills paths with both rules at analytic coverage, expands strokes to fills, and paints
solid, linear and radial fills into a caller's buffer, with the engine's math so a shape
rasterizes to the same pixels everywhere. Krome caches its output per shape and size, and
per colour for a shape with colours of its own; a shape painted only `currentColor` (std's
icons) is rasterized once, white, and tinted, so a hover, a theme or a transition costs no
rasterization (slice 5). The SVG tool, kvscript (`kv.patherizer`, for asset scripts) and a
game can call it directly.

### Assets and reload

`.kimb` screens, preludes and themes, and shapes, are asset records in the asset module,
loaded into scopes like any other. goldberg cooks `.kim` to `.kimb` with the `kim`
module, and reports each file's `use`s as discovered edges, so saving a prelude re-cooks
exactly the screens that use it. Shape references are not edges (slice 5): every `.svg`
under a root cooks by convention, and a screen naming a shape that is not there finds out
at load (logged, drawn as nothing). Reload is the asset module's rebind (slice 6): a screen,
a prelude, a script or a shape saved under a running watch changes the running program.

### Errors

Every node carries its file and line through expansion and resolution. An unknown word,
a property the prelude never declared, a token no theme defines, a binding to nothing in
the model, a definition used with the wrong arguments: each names the line written in
`.kim`, whether found by `kimc check`, by goldberg's cook or at runtime.

## What the engine gets

| Need | Where | Slice |
|---|---|---|
| The UI primitive: per-instance look, clip rects, a list per window, in Vulkan, Metal and D3D12 | `render`, the backends | 1 |
| A tool with a window: `tool_main` opening one, frames paced by input and idle | `tool`, `frameloop` | 1 |
| Krome's own Lua state, with the engine's math, and events in and out of it | `krome` (on `lua`) | 1 |
| Event tracing that is not hashed | `core` (the engine's trace), `cli` | 1 |
| Cursor shapes (arrow, I-beam, resize, hand) | `platform_hid` | 3 |
| Clipboard text, both ways | `platform_hid` | 3 |
| IME composition events and the candidate window's position | `platform_hid` | 3 |
| Paragraph text for every customer: wrapping, measure, caret geometry, glyph cache on demand, fallback fonts | `text`, `asset` (fonts) | 3 |
| Subpixel positioning, opt-in | `text`, `asset` (fonts) | 3 |
| A general-purpose vector rasterizer: fills with both rules, strokes, gradients | `patherizer` (new) | 5 |
| The platforms' text input protocols, and an accessibility tree | `platform_hid`, the platform ports | later |
| Reload by path and handle rebind | `asset` (`asset_reload`), `asset_live` (goldberg's connection) | 6 |
| A check a loop pacing on demand makes at each poll (a socket's) | `core` (`engine_add_waker`) | 6 |

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| K1 | The kernel's five nodes, and nothing else in C++ that a prelude could say | P0 |
| K2 | Layout: row, col, stack; gap, pad; hug, fill, fixed, ranges; spacer; alignment; tested headless | P0 |
| K3 | A screen built as a Lua table and as `.kimb` is the same tree and the same pixels | P0 |
| K4 | The runtime loads `.kimb` only; `krome` does not link `kim` | P0 |
| K5 | `.kim` to `.kimb` is deterministic and canonical: the same document gives the same bytes | P0 |
| K6 | `kim` depends on `core` and `kimb` only; a game can link it to read text | P0 |
| K7 | Named styles with states, merged left to right, no selectors; theme tokens; declared inheritance | P0 |
| K8 | Definitions with parameters and one slot; `each` and `if`; nothing more in markup | P0 |
| K9 | Model binding: `@name` two-way, `{name}` in text, a change redraws only what read it | P0 |
| K10 | Named events to Lua, and to C++ as an engine event; Krome's Lua state is its own and only events cross it | P0 |
| K10a | Krome's nodes, Lua and events are never hashed; its events are traced | P0 |
| K11 | Every error names the `.kim` file and line, through expansion and resolution | P0 |
| K12 | A UI primitive made for this drawing (per-instance look, clipping, a list per window) on every backend, held to render-gate | P0 |
| K13 | A still screen costs no layout and no paint, and a still tool window sleeps | P0 |
| K14 | Paragraph text in `text`, for games and `ui` as well as Krome: wrapping, fallback, glyphs on demand, crisp at small sizes and any scale | P0 |
| K14a | Subpixel positioning, asked for per font or label; grayscale anti-aliasing always | P1 |
| K15 | `edit`: caret, selection, clipboard, IME composition (as SDL3 and the native ports give it), undo of typing | P1 |
| K16 | The standard prelude covers goldberg's screens (see Done) | P1 |
| K17 | Overlays in the kernel: layers above the screen, anchored and kept in the window, input top first, modal focus | P0 |
| K18 | `each` over ten thousand items lays out and paints only what is visible | P1 |
| K19 | Keyboard focus order and shortcuts, without a pointer | P1 |
| K20 | Transitions between style states | P2 |
| K21 | Vector shapes, from SVG, rasterized on demand at the drawn size | P1 |
| K21a | The rasterizer is a module of its own, for any caller, not stb_truetype's | P1 |
| K22 | goldberg cooks `.kim` with discovered edges; a prelude save re-cooks its screens (slice 6: re-checks them) | P1 |
| K23 | Hot reload through rebind (slice 6) | P2 |
| K24 | Grid areas for screen shells | P2 |
| K25 | `edit` speaks each platform's text input protocol: dictation, autocorrect, accent menus, soft keyboards | P2 |
| K26 | An accessibility tree from Krome's nodes, for each platform's screen readers | P2 |
| K27 | The prelude is `.kim`; the engine ships `krome/std`, which an author may use, override or replace | P0 |

## Slices

1. **Boxes from Lua.** The kernel's `box`, `text` (today's one-line text), `image` and
   `hit`; layout; overlays; the UI primitive in the three backends; Krome's Lua state;
   screens as Lua tables against a model; named events, traced; a tool that opens a
   window.
   *Done when* a form written in Lua lays out, resizes with the window, shows hover and
   pressed states, opens a menu over itself that closes on a press outside it, and fires
   events, in a Krome example and in a windowed tool; layout tests pass headless;
   render-gate agrees on all three backends.
2. **The markup.** The `kim` module, `.kimb`, `kimc` (compile, check, format), the
   loader; the prelude (props, styles with states, themes, definitions, `each`, `if`);
   errors with lines; a first standard prelude (label, button, checkbox, row and column
   helpers); goldberg's converter (built in: goldberg compiles and checks `.kim` in
   process).
   *Done when* slice 1's form is a `.kim` file, is pixel for pixel what the Lua form
   drew, and a misspelled style names its line in all three places an error can surface.
3. **Text.** Paragraphs in `text`, with a game's `Text` able to wrap; fallback, glyphs on
   demand, scale; subpixel positioning on request; `edit` with
   caret, selection, clipboard and IME; cursor shapes.
   *Done when* a multi-line field takes typed, pasted and composed text, a game label
   wraps at a width, and a 9 px label
   is as crisp as the platform's own.
4. **Widgets and screens.** The standard prelude for tools: input, slider, dropdown,
   tabs, tree view, table with columns, scroll view, splitter, progress, badge, tooltip,
   menus, context menus, modal, on slice 1's overlays; virtualized `each`; focus order and shortcuts; transitions.
   *Done when* goldberg's five screens (below) can be written without a kernel change.
   *As built*: they could not, quite. Writing std's widgets asked the kernel for a few
   general hooks first (a behaviour's `update` drawing its `part`s, `pick` from what it
   opened, `w:open_slot`; `if key=value`; `{@}` and `@@` for a definition's use's binding),
   and the widgets and the five screens then found four kernel bugs (an ancestor's
   `selected` reaching its descendants' looks, Enter in an edit with nothing to submit,
   dotted paths in texts, braces in model values). After those, the five screens
   (`examples/kromescreens`) needed nothing more.
5. **Shapes.** The `patherizer` module, the shape asset, the SVG tool, Krome's shape cache; icons in the
   standard prelude. Built (Krome-Slice-5-PRD.md; docs/design/patherizer.md has what each
   milestone found). The SVG tool is C++ (the `svg` module, in process in goldberg), not
   Lua; std's icons are Material Symbols. An icon rasterizes in microseconds; a 1000 px
   illustration takes 14 to 48 ms, so a shape over 256 x 256 is rasterized on a task worker
   of the platform's I/O queue (patherizer's README has the numbers).
6. **Live.** Hot reload of screens, preludes, themes and shapes through goldberg and the
   asset module's rebind. Built (Krome-Slice-6-PRD.md): `asset_reload` rebinds a texture, a
   material, a shape or a blob under its handle; Krome reads its documents as blobs and, on a
   reload, checks the new reading (`kimb_check`) before it builds anything, holding a screen
   that does not check on the reading it had; a dev build that links `asset_live` hears a
   `goldberg watch` say what changed, a save reaching the window in about 9 ms; and goldberg's
   window (`goldberg watch --window`) shows status, inspection, reports, rule forms and their
   diffs over the live session, and edits itself.

**Done, for the whole of it:** goldberg's window shows the five things its design asks for
(`docs/design/asset-toolchain-design.md`, "Dialog UI library"): inspection, diff review,
live status, reports and forms, over the same commands as its CLI. *As built*: all five, with
diff review as a rule edit's preview, since goldberg has no lockfile yet (slice 6).

**After it, drafted and not built** (Nov 23, 2026). Blackwing's skin import note asks
Krome to draw other players' skins, which are made of pieces of bitmaps placed by
coordinate (`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout, KR28 to
KR42). Two slices are drafted from it, and the first is built. Neither is part of "done"
above.

7. **Pictures.** A region of an image, strips of frames, tiled and nine-slice fits,
   bitmap fonts, looks computed from an image's pixels (KR28, KR29, KR31, KR32, KR34).
   Built on Linux (Nov 26; [Krome-Slice-7-PRD.md](Krome-Slice-7-PRD.md)): `region`,
   `pixel`, `fit`, `frames` and their play, `.kbf` and inline bitmap fonts, `gain`, `hue`,
   `saturation`; Metal and D3D12 not verified. A skin written by hand found what a converter
   still needs, placement (KR30) first; its As built lists the rest.
8. **Placed, moving, and not a rectangle.** Edges and offsets in a stack, easing of sizes
   and positions, timers for scripts, text that scrolls, hit testing through a mask
   (KR30, KR33, KR35, KR36, KR37). Drafted: [Krome-Slice-8-PRD.md](Krome-Slice-8-PRD.md).

Both go past this document's non-goals ("animation beyond transitions between style
states") and bear on K1; slice 8's "For review" asks how far the kernel should go. The
windows a skin needs (shaped, several, snapping: KR39 to KR42) are the engine's:
[Engine-Slice-13-PRD.md](Engine-Slice-13-PRD.md).

## Decided

Nov 15, 2026, in review of the first draft.

- **`.kim` is text, `.kimb` is binary**, and the runtime reads only binary.
- **`kim` is its own module**, depending on `core` and `kimb` (the document's meaning,
  which `krome` links too); `krome` does not depend on `kim`.
- **The prelude is a `.kim` file.** The engine ships reasonable defaults (`krome/std`);
  the author does what they like with them.
- **`.kimb` is syntax, not meaning, one per `.kim`.** Definitions are expanded and styles
  resolved at load, against whatever prelude is loaded then. Nothing collapses a screen
  and its preludes into one file for now. `kimc check` and goldberg's cook resolve too,
  to report errors early, but do not bake the result in.
- **A repeated property takes its last value.** Properties are written in name order.
- **Lines live in a sidecar array** in the `.kimb`, so they can be stripped or ignored.
- **A new render primitive for UI**, not sprites with parameters.
- **Cosmetic by default.** Krome's events are traced, not hashed. A game that drives its
  sim from Krome is a later problem.
- **Krome holds its own Lua state** and talks to others through events.
- **Overlays are in the kernel**, from slice 1.
- **Paragraph text and subpixel positioning are the `text` module's**, available to
  games; subpixel positioning is asked for. No LCD anti-aliasing.
- **Comments are `//`. A file names its preludes with `use`.**
- **Shapes rasterize in a module of their own**, general-purpose, built for SVG's needs;
  stb_truetype stays as it is.

## Open

- [ ] Shaping: HarfBuzz later, or our own shaper for the scripts we need first?
- [x] Glyphs at small sizes: exact-size bitmaps per scale, made on demand into pages (Slice 3 PRD).
- [ ] Where Krome's update and draw sit in the schedule, and what a tool window with no
      simulation runs.
- [ ] Multiple windows: goldberg's window is one. A tool with a detached panel needs
      `platform_hid` to open more than one. Drafted as engine slice 13
      ([Engine-Slice-13-PRD.md](Engine-Slice-13-PRD.md), KR40).
- [ ] Native text input per platform and the accessibility tree: the design of their own
      (K25, K26), and whether SDL3 can be kept on the desktops or has to be gone around.
- [x] String interpolation: `{key}` for a model value and `{*tag}` for a tag, in Krome and `ui` alike (Slice 2 PRD).
- [ ] Grid areas for screen shells.
- [ ] Templating beyond parameters and one slot.
- [x] `.kshape` for the shape asset's extension (slice 5).
- [ ] Does `ui` ever move onto Krome's layout or documents, or its UI primitive?
