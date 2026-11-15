# Krome Slice 2 — The markup

Nov 15, 2026 · @starekrow · draft

## Goal and exit criteria

Slice 2 gives Krome its own language. Screens, preludes, styles and themes are written as
`.kim` text, compiled by the `kim` module to `.kimb` (canonical KBOR), and loaded by Krome,
which gives the words their meaning against the preludes a file uses. The master PRD
(`Krome-Master-PRD.md`) says what each piece is; slice 1 (`Krome-Slice-1-PRD.md`) built the
kernel this stands on. This says what is built now, in what order, and how we know it
worked.

The slice is the proof of the three layers: a kernel in C++, a prelude in markup, content in
markup, with nothing in the kernel that knows a widget. If a button cannot be a definition,
if a style cannot say what a hover looks like, if a theme cannot be swapped without
recooking, or if a mistake cannot name the line it was made on through all of that, this is
where it shows, before text, widgets and shapes are built on top.

It is done when:

- `examples/kromedemo`'s form is `form.kim`, its handlers in Lua files its `script` lines
  name, and draws the same pixels as the slice-1 Lua form (`picture compare`, exact) at
  both scales.
- Its buttons, fields and menu items are definitions in `krome/std`, the engine's prelude,
  written in `.kim`; the kernel has gained no node type.
- Switching the demo's theme from dark to light at run time restyles every screen and
  overlay with no recook and no reload of a file.
- A misspelled style, an unknown word, a property no prelude declares and a definition used
  with a parameter it does not take each name the `.kim` file and line they were written on,
  in all three places an error can surface: `kimc check`, goldberg's cook, and the running
  game. A mistake inside a definition names both lines: where it was used and where it
  was defined.
- `.kim` to `.kimb` is canonical: a file with its comments, white space and the order of
  each line's properties changed compiles to the same bytes. Its lines are a sidecar: a
  stripped `.kimb` loads and draws the same, its errors naming the node's path instead.
- `each` over a list of a thousand in the model, and `if` on a value, rebuild what they
  cover when the value changes and nothing else; the numbers are in the notes.
- goldberg cooks every `.kim` under an asset root into a `.kimb` beside it in the output
  tree, with no project `.berg` saying so.
- `ui`'s labels and Krome's texts write a model value the same way, `{key}`, and a tag
  only as `{*tag}`; arcfire's and arcanum's documents use them, and arcfire's replays match.
- `kim_tests` (the parser, the compiler, canonical bytes, errors) and `krome_tests`
  (loading, styles, themes, definitions, control flow) pass headless.

## What the code has

- **Krome, slice 1** (`modules/krome`): the kernel's four node types; layout; looks per
  state; the UI primitive; overlays; a Lua state of its own; screens as Lua scripts whose
  helpers (`row`, `col`, `text`, `hit`, ...) build nodes as tables
  (`w, f, p, t, e, id, s, c, l`, with `s` holding state looks and `looks` beside flags of
  the same names); a model of scalars; `KromeEvent` out, `KromeSet` in; the Frame cycle
  (`docs/design/cycles.md`). Properties are the kernel's own names, fixed in `tree.cc`.
- **KBOR** (`korvid/core/kbor.h`): `kbor_parse` into a `JsonDoc`, `kbor_write` from one,
  always normalized. There is no way to build a `JsonDoc` but to parse one.
- **goldberg** (`tools/goldberg`): rules and converters in `.berg` files; `copy` built in;
  script (`shell`) and program (`exec`) converters run as child processes; no discovered
  edges yet (its slice 2). `conventions.berg` types files by name; `.lua` lands as a
  script.
- **kbor** (`tools/kbor`): `decode` shows any `.kimb` as JSON.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/kim/` | the `.kim` parser and compiler: text to a document, the document to `.kimb`; depends on `core` only |
| `modules/core/` | `JsonBuilder`: a `JsonDoc` built by hand, for `kim` and for anything that writes KBOR |
| `modules/krome/` | the loader: `.kimb` files by asset path, their uses, resolution against preludes, styles, themes, definitions, `each`, `if`; the builder shared with Lua's tables |
| `modules/krome/assets/krome/std.kim` | the engine's prelude: properties, styles, two themes, the first widgets |
| `tools/kimc/` | the command line: `compile`, `check` |
| `tools/goldberg/` | built-in converters, `kim` the first; `*.kim` in `conventions.berg` |
| `examples/kromedemo/assets/` | `form.kim` and the Lua files its `script` lines name |

### `.kim`

The design's syntax (`docs/design/krome.md`, "The syntax"), with what the master PRD added:
`//` comments, `use`, and the top-level forms `use`, `script`, `prop`, `style`, `theme`,
`def`, `screen`. One node per line: a word; then flags, `name=value` properties, a content string,
a bare range, `@binding`, `-> event` and `[styles]` in any order; then an optional block.

```
use "krome/std"
script "kromedemo/form.lua"
script "kromedemo/assets_list.lua"

screen form {
    col [page] {
        field "Name" "{name}"
        row gap=8 {
            button "Save" -> save
            button "Later" disabled
        }
        each item in assets {
            row [list-row] -> pick { text "{item.name}" }
        }
        if dirty { badge "unsaved" }
    }
}
```

Values are numbers, strings (`"..."`), ranges (`120..400`, `..400`, `120..`), colors
(`#rrggbb`, `#rrggbbaa`), lists (`radius=4,8`) and bare words, which are flags as a node's
argument and tokens as a property's value (`bg=panel2`).

### `.kimb`

The document, compiled: one KBOR map per file, normalized, so a repeated key costs a byte.

```
{ "kim": 1, "file": "kromedemo/form.kim",
  "uses":    ["krome/std"],
  "scripts": ["kromedemo/form.lua", "kromedemo/assets_list.lua"],
  "props":   [{"name": "bg", "type": "color"}, ...],
  "styles":  {"button": {"p": {...}, "hover": {...}, "pressed": {...}}, ...},
  "themes":  {"dark": {"panel2": "#202430", ...}},
  "defs":    {"button": {"params": ["label"], "body": <node>}},
  "screens": {"form": <node>},
  "lines":   [3, 4, 5, ...] }

node: { "w": "row", "f": ["fill"], "p": {"gap": 8}, "s": ["toolbar"], "t": "Name {name}",
        "r": [1, 99], "b": "level", "e": "save", "id": "save", "c": [<node>, ...] }
each: { "w": "each", "v": "item", "in": "assets", "c": [...] }
if:   { "w": "if", "on": "dirty", "c": [...] }
```

- **Canonical.** Properties in name order; a repeated one takes its last value; flags in
  the order written (they are words, and a later alignment wins); no comments, no spacing.
- **Lines in a sidecar**, one per node in the order a walk meets them: the screens by name,
  then the definitions by name, each depth first. Stripping it changes no other byte.
- **One shape with Lua.** Lua's tables take the same keys. `s` is styles in both; a Lua
  table's state looks move to `looks` alone (slice 1's `s` for them goes), so a `.kimb`
  decodes to exactly the table a script would write.

### The `kim` module

`kim_compile(text, len, file, alloc, out, errors)`: the parser (one pass, a node per line,
blocks by braces, every token with its line) builds a `JsonDoc` through core's new
`JsonBuilder`, and `kbor_write` writes it. It knows the grammar, not the meaning: an
unknown word is not its error. It reports what it can see (an unclosed brace, a bad
value, a duplicate definition or style in one file, `each` without `in`), each with its
line, and compiles nothing when there is one.

`kim_check(doc, preludes, errors)` is the meaning without Krome: given the compiled file
and the documents of what it uses, it resolves every word, property, style, token and
definition the way the loader will, and reports what does not resolve. `kimc check` and
goldberg's cook call it; the running game finds the same mistakes the same way, since the
loader is built on the same resolution.

### Meaning: preludes, properties, styles, themes

- **A prelude is a `.kim` file with no screens.** A file resolves a word against its own
  definitions, then each `use` in order (and theirs, depth first, each file once), then the
  kernel. `krome/std` ships as the krome module's asset; nothing uses it unless a file says
  so.
- **Properties.** The kernel's properties keep their names (`bg`, `pad`, `radius`, ...) and
  are always there, fixed. `prop NAME TYPE` declares a parameter a definition takes by
  name, its type checked where it is given. A prelude does not rename, retype or
  re-declare a kernel property; that waits until something needs it.
- **Styles.** `style NAME { props; STATE { props } }`, merged left to right on a node
  (`[button primary]`), then the node line over them. A style may set layout as well as
  looks; state blocks set looks only, as slice 1's state looks do. A style may name others
  it starts from (`style primary [button] { ... }`).
- **Themes.** `theme NAME { token=value ... }`. A bare word as a property's value is a token,
  resolved against the screen's theme when its looks are resolved; a token no theme defines
  is an error with its line. `krome_theme(e, name)` (and `theme("dark")` from Lua) switches
  every screen; looks are resolved again from the documents, which Krome keeps, and nothing
  is read or recooked.

### Definitions

`def NAME [PARAM...] { body }`: a named subtree. A use is a node whose word is the
definition's name:

- **The content string** is the first parameter; `name=value` for a parameter by name.
- **Everything else on the use line** (flags, properties that are not parameters, styles,
  the event, the binding, the id) goes onto the body's root, over what the body says.
- **One slot.** A `slot` in the body is where the use's children go; a body with none
  takes no children (an error at the use).
- **Parameters in text.** `{label}` in a body's strings is the parameter first, then the
  model, so `text "{label}"` in a button shows its label and `text "{name}"` in content
  shows the model's.
- **Errors carry two lines**: the use's (`form.kim:12`) and the body's (`std.kim:40`).
- **Expansion happens at load**, every time a screen is built: definitions can come from a
  prelude swapped since the screen was cooked.

### Control flow and the model

- **`each item in list { ... }`** repeats its block for each element of a model value that
  is a list; `{item.field}` reads the element's fields. **`if name { ... }`** builds its
  block when the value is truthy (present, not false, not 0, not "").
- **The model holds lists and tables** from Lua (`model.assets = {...}`). Writing a key
  rebuilds what reads it: the `each` or `if` on that key, with everything under it, and
  nothing else. Changing a list in place is not seen; assign it again, or call
  `changed("assets")`.
- **From C++ too.** `krome_set` for a scalar, as now; `krome_set_doc(e, screen, key,
  bytes, len)` for a list or a table, as KBOR (`JsonBuilder` makes one), arriving as a
  `KromeSetDoc` event and landing as the Lua table it decodes to. Lua and C++ may both
  write any key; the last write wins, and what reads it rebuilds the same either way.
- **What a rebuild keeps.** A rebuilt subtree's scroll offsets and the pointer's place are
  kept for nodes with the same id.

### Bindings

`@name` on a node binds its `selected` state to the model's truthiness, and a press on it
writes the opposite back before its event fires, so `checkbox "Wrap" @wrap` needs no
handler. A slider's or a field's two-way value is `edit`'s (slice 3) and a widget's
(slice 4).

### Lua and `.kim` together

**A `.kim` file names its handlers.** `script "kromedemo/form.lua"` at the top level, by
asset path, as many as the file needs:

- **`krome_open(e, "kromedemo/form.kimb", "form")`** opens the screen named `form` and reads
  the file's scripts with it through the I/O queue. When all are in, they run in the order
  written, in the screen's own environment (one per screen, as in slice 1), before the
  screen is first built.
- **A handler script returns `{ model = ..., on = ... }`**, with no root. Models merge in
  order, a later key over an earlier; handlers merge, and one event handled in two scripts
  is an error at load naming both files.
- **A script that fails** to load or run names itself, with its line, and the screen opens
  with what the others gave.
- **No `script` lines** is a screen with no Lua: its model from C++ and its events out as
  `KromeEvent` only.
- **A prelude's scripts are not run.** A widget whose behaviour needs Lua is slice 4's
  question.

**A Lua script can still be the root**, as in slice 1, and splice markup in: `root =
kim("kromedemo/form.kimb", "form")` hands back the screen's tree as the tables a script
would build. The calling script owns the handlers then; the file's `script` lines are
not run.

Either way the builder is one: it takes a node from a `.kimb` document or a Lua table,
resolves its word against the file's preludes, and builds the kernel's tree.

### `kimc`

`kimc compile in.kim out.kimb` and `kimc check in.kim [--prelude DIR]...` (the preludes'
`.kim` or `.kimb` files found by their asset paths under the directories given). An engine
tool, as `kbor` is; `kbor decode` shows a `.kimb`. A formatter (`kimc format`) waits;
whether the parser keeps comments for it or the document carries them comes out the same.

### Model values and tags

In text, `{key}` is a model value and only that, in Krome and in `ui`; `{{` is a brace in
both. A tag that changes how text looks or plays starts with `*`: `ui`'s rich text becomes
`{*c=gold} ... {/c}`, `{*wave} ... {/wave}`, `{*shake} ... {/shake}`, `{*pause=0.4}`. A
closing needs no `*`, since no key starts with `/`. There is one way to write each, and a
key never looks like a tag: `{=key}` and the bare tags go, with no aliases. Krome's text
spans (slice 3) take the same `{*...}` form. `ui`'s documents move over: arcfire's and
arcanum's, about 30 model values and 10 tags. A meter's `"=key"` is a property's value, not
text, and stays.

### goldberg

`kim` is a cheap in-process tool, as `kbor` is, so goldberg gains built-in converters: a
converter name it knows without a registration, run in process on its I/O workers like a
hash, with no child process and no path to a host binary. `kim` is the first:
goldberg links the `kim` module, and `conventions.berg` types `*.kim` as `kim` (an
ingredient) converted by `kim` to `kimb` (which lands, as `{stem}.kimb`). A cook reports
a compile error with its file and line, and the step fails as any other. `kim_check`
against the uses is run too when they are in the same tree, so a misspelled style fails
the cook; a use from another module's assets (`krome/std` from a game) is checked when
that module's assets are in the cook.

## Out of scope

Paragraph text, `edit`, the clipboard (slice 3). The widget set beyond the first few,
focus order, shortcuts, transitions, virtualized `each` (slice 4). Vector shapes (slice 5).
Reload and discovered edges (slice 6 and goldberg's). `kimc format`. Templating beyond
parameters and one slot. Grid areas. Metal and D3D12 drawing the UI primitive.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KM1 | `JsonBuilder` in core: a `JsonDoc` built by hand, tested by writing KBOR and reading it back | P0 |
| KM2 | `kim`: the parser and compiler, every grammar error with its line, depending on `core` only | P0 |
| KM3 | `.kimb` canonical: comments, spacing and property order change no byte; lines a strippable sidecar | P0 |
| KM4 | `kim_check`: resolution without Krome, the same rules as the loader | P0 |
| KM5 | Krome loads `.kimb` by asset path with its uses; one builder for documents and Lua tables | P0 |
| KM6 | Lua tables take the `.kimb` keys; `s` is styles; state looks are `looks` | P0 |
| KM7 | Styles: merged left to right, the node line over them, state blocks, styles from styles | P0 |
| KM8 | Themes and tokens; `krome_theme` switches every screen without a reload | P0 |
| KM9 | `prop` declarations: typed parameters by name | P1 |
| KM10 | Definitions: parameters, one slot, the use line onto the body's root, two-line errors | P0 |
| KM11 | `each` and `if`, rebuilding only what reads the key that changed; the model holds lists from Lua | P0 |
| KM12 | `@binding` on `selected`, written back on a press | P1 |
| KM13 | `krome/std`: properties, the first styles, dark and light themes, label, heading, button, checkbox, field, panel, menu-item, badge, separator | P0 |
| KM14 | `kimc compile`, `kimc check` | P0 |
| KM15 | goldberg's built-in converters; `kim` the first; `*.kim` cooked by convention | P0 |
| KM16 | `kromedemo` in markup: the same pixels as the Lua form; a theme switch | P0 |
| KM17 | `krome_set_doc`: lists and tables from C++ as KBOR | P1 |
| KM18 | The module READMEs, `kimc`'s, the notes | P1 |
| KM19 | `script` lines: handlers in several Lua files, merged, read with the screen | P0 |
| KM20 | `{key}` for model values and `{*tag}` for tags, in `ui` as in Krome; `{=key}` and bare tags gone; arcfire's and arcanum's documents moved; the replays match | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **The document** (KM1 to KM3, KM14 in part). `JsonBuilder`; the `kim` module; `.kimb`;
   `kimc compile`. `kim_tests`: the grammar, errors, canonical bytes, the sidecar.
2. **Meaning** (KM4 to KM9, KM19). Krome's loader and the one builder; Lua's keys;
   preludes and their order; styles; themes; `prop`; `script` lines and merged handlers;
   `kim_check` and `kimc check`; errors with lines at load.
3. **Definitions and control flow** (KM10 to KM12, KM17). Expansion with the slot and the
   use line; `each`, `if`, lists in the model from Lua and from C++, partial rebuilds;
   bindings.
4. **The prelude and the demo** (KM13, KM16). `krome/std`; `form.kim` and its scripts; the
   picture against slice 1's; the theme switch.
5. **The cook, and close** (KM15, KM18, KM20). goldberg's built-in converters and the
   convention; `{key}` and `{*tag}` in `ui`; the numbers; the READMEs; the master PRD corrected where
   this slice proved it wrong.

## Decided

- **`kim` is a cheap in-process tool**, as `kbor` is: goldberg links it and compiles `.kim`
  as a built-in converter, not a child process.
- **`prop` declares typed parameters only.** The kernel's properties are a fixed
  vocabulary; a prelude does not re-declare them.
- **One spelling each**: `{key}` is a model value, `{*tag}` a tag, in Krome and in `ui`;
  nothing else is accepted.
- **Both Lua and C++ write the model**, lists and tables included (`krome_set_doc`, KBOR).
- **`kimc format` waits**, and how comments reach it is left to whoever builds it.
- **Handlers are named by `script` lines** in the `.kim`, any number of them, merged in
  order; one event handled twice is an error.
