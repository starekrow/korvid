# Krome Slice 4 — Widgets and screens

Nov 15, 2026 · @starekrow · draft

## Goal and exit criteria

Slice 4 makes Krome enough to build a tool's screens out of. The standard prelude grows the
widgets a tool needs (slider, dropdown, tabs, tree, table, splitter, progress, tooltip,
menus, context menus, dialogs, scroll views with bars), the kernel grows the few things
those widgets cannot be built without (behaviour in the prelude's Lua, keyboard focus and
shortcuts, properties that read the model, scrollbars, virtualized lists, transitions), and
goldberg's five screens are written to prove it. The master PRD (`Krome-Master-PRD.md`,
slice 4) asks for exactly this; slices 1 to 3 built the kernel, the markup and the text this
stands on.

The test is the master PRD's: **goldberg's five screens can be written without a kernel
change.** They are the five things goldberg's design asks of its window
(`docs/design/asset-toolchain-design.md`, "Dialog UI library"): inspection, diff review,
live status, reports, forms. Wiring them to goldberg's session is goldberg's own G16, not
this slice; here they run on sample models, so the slice proves the library, not the tool.

It is done when:

- **The five screens** are `.kim` and Lua in `examples/kromescreens`, on sample models
  shaped like goldberg's (`explain`'s answer, a lockfile's re-routes, the session's steps, a
  cook's notes, a `.berg` file's rules), using `krome/std` and styles of their own, and
  nothing added to the kernel after milestone 3. `kimc check` finds nothing in them.
- **`--demo` drives each one** with the pointer and the keyboard and checks the model: an
  asset inspected and its upstream followed; a re-route accepted and one rejected; a step's
  progress moving; a report sorted and filtered; a rule edited in a form and saved, through
  a dropdown, a slider, a checkbox, tabs and text fields.
- **The forms screen works without a pointer**: Tab and Shift+Tab move through its fields in
  order, Space and Enter work what has the focus, arrows move a slider, a dropdown's list and
  a tree, and Ctrl+S (Command+S on a Mac) saves.
- **A table of 10,000 rows** scrolls with its bar and the wheel, building and painting only
  the rows in view: a frame of it costs what a frame of 50 rows does, within 10%, and the
  numbers are in the notes.
- **kromedemo has a second page**, a gallery of every widget in both themes, and a way
  between its two pages; its form page still draws the same pixels as the Lua form.
- **No widget's name appears in the kernel.** std's widgets are markup and prelude Lua; one
  widget in the tests is written with a behaviour in C++, registered from outside the
  kernel, to prove a widget can live in either place.
- `krome_tests` covers the kernel's additions headless (behaviours, focus order, shortcuts,
  bound properties, percent sizes, scrollbars, virtualization, transitions); `render-gate`
  passes on Linux.

## What the code has

- **The kernel** (`modules/krome`): `box`, `text`, `image`, `hit`, `edit`; layout with
  wrapping; looks per state (`focused` only for an edit with the keyboard); overlays
  anchored (below, above, right, left, at the pointer), light or modal; scroll boxes moved
  by the wheel, with no bar; the model with `{key}` in texts, `@key` bindings of `selected`
  (truthy) or an edit's text, `each` and `if` rebuilt alone; definitions with one slot;
  `cursor=`; a press goes to the nearest node with an event, a binding, a `hit` or an edit;
  left button only. Keys reach only an edit with the keyboard.
- **Lua**: a screen's scripts handle its named events with `(id, value)`; nothing gets
  the pointer's position, a drag, or a key. A prelude's scripts are not run (slice 2's
  decision, left to this slice).
- **`krome/std`**: row, col, stack, spacer, separator, label, heading, field, badge, button
  (and `[danger]`), checkbox, input, textarea, menu-item, panel; dark and light themes.
- **Properties are static** once built, but for tokens (a theme switch builds again) and
  texts that read the model.
- **The platform** (slice 3): every key, typed text, the clipboard, cursor shapes; the right
  and middle buttons arrive as events but Krome ignores them.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/krome/` | behaviours, keyboard focus and shortcuts, bound properties, `@key=value`, percent sizes, the right button, tooltips, scrollbars, virtualized `each`, transitions |
| `modules/kimb/` | the vocabulary's additions |
| `modules/krome/assets/std.kim`, `std.lua` | the widgets, and the behaviour behind them |
| `examples/kromescreens/` | goldberg's five screens on sample models, driven by `--demo` |
| `examples/kromedemo/` | a second page, the widget gallery, and navigation between the two (the Lua form draws the same navigation, so the form pages still compare) |

### Behaviour, in Lua or in C++

A widget is a definition (what it looks like) and a behaviour (what the pointer and the keys
do to it). A behaviour lives in either place: Lua in a prelude (the design's "a prelude is a
pair", and how std's are written), or C++, registered by a module
(`krome_behaviour(e, "name", KromeBehaviourDesc{...})`) for a widget that wants the speed or
the engine's types. The kernel knows neither by name; it looks a behaviour up when a node
names it, and one name registered both ways is an error at the second registration.

- **A prelude runs its scripts.** `script` lines in a prelude are run once per Lua state, in
  a prelude environment of their own, before any screen that uses it; slice 2 left them
  unrun. What they return is a table of behaviours by name.
- **A node names its behaviour**: `behave=slider`, written in the definition's body. The
  kernel sends it what it gets, as calls (Lua functions, or the C++ descriptor's function
  pointers with the same arguments): `press(w, x, y, button)`, `drag(w, x, y)`,
  `release(w, x, y)`, `wheel(w, dy)`, `key(w, name, mods)` (when it has the focus),
  `focus(w, on)`, `hover(w, on)`. `x` and `y` are in the node's own logical pixels.
- **`w` is the widget instance**: its id, its rect, its bound path, and `w.value` /
  `w:set(v)` to read and write the model where it is bound; `w.state` is a table kept for
  the node as long as its id lasts (a dropdown's open overlay, a drag's start); `w:emit(name,
  value)` fires an event as the screen's own would; `w:open(tree, opts)` opens an overlay
  anchored to it.
- **The screen's handlers stay the screen's**: a behaviour changes the model and emits; the
  screen's script hears the events. A behaviour is the same for every screen that uses it.
- **In C++** the instance is a `KromeWidget&` with the same members (`value()` and
  `set()` as text or numbers, `state` an engine-side blob of a size the descriptor says).

### Keyboard focus and shortcuts

- **Focusable**: an edit, a `hit` or anything else with an event or a behaviour, unless
  `nofocus`; anything with `focusable`. **Tab** and **Shift+Tab** move through a screen's
  focusable nodes in document order (`taborder=N` puts one earlier or later), skipping the
  disabled and the hidden; the top overlay's first, if a modal is open, and only its.
- **The focused look**: what has the focus takes its `focused` look, as an edit does now; a
  focus ring is std's style (`focused { border-color=accent }`), not the kernel's.
- **Activation**: Space and Enter on a focused node with an event fire it (a press without
  the pointer); a focused node with a behaviour gets the key instead.
- **Shortcuts**: `key="mod+s"` on a node fires its event from anywhere in its screen while
  its screen is the top one (only the top one's, for now) and no edit has taken the key (`mod` is Ctrl, or Command on a
  Mac; `ctrl`, `alt`, `shift`, `cmd` as written). A disabled node's shortcut does nothing.
- **An edit keeps Tab** only with `tabs` (a code editor's); otherwise Tab leaves it.

### Properties that read the model

- **A property bound to the model**: `w=@left_w`, `bg=@item.color`, `opacity=@fade`, the
  binding's form, as a node's `@key` is. The node is set again when the key changes, with no
  rebuild. A value that does not parse for the property is the property's error, at the
  node's line. In a document the value is the string `"@left_w"` (a token never starts with
  `@`), so Lua writes `w = "@left_w"`.
- **`@key=value`**: a binding by equality, for radio buttons, tabs and a dropdown's items:
  the node is `selected` while the model's value is `value`, and a press writes `value`.
- **Percent sizes**: `w=40%`, `h=100%`, of the parent's inside, so a progress bar is a box at
  `w=@done` in a track, `done` a model value such as `"40%"`.

### Scroll boxes with bars

A `scroll` box gets a bar along its direction while its content is longer than it is: drawn
by the kernel in the box's `scrollbar` look (a track and a thumb, std's style), dragged, a
press on the track paging, the wheel and Page Up and Down when it has the focus moving it.
`scroll=x` and `scroll=xy` scroll across or both ways (a table's wide rows); Shift+wheel
scrolls across. The bar overlays the content (no layout space), and hides when the content
fits.

### Virtualized `each`

`each item in rows height=24` inside a scroll box builds only the elements in view (and a
screenful each side), with a spacer before and after of the height of what was not built;
scrolling builds the newly visible and lets the gone go, as a region rebuilds today. Elements
are a fixed height: variable heights wait. Without `height=`, `each` builds
everything, as now.

### Transitions

`transition=0.15` (seconds; on a node or in a style) eases its look between states: colors
(the fill, the gradient's second, the border's, the shadow's, the text's) and opacity, over
the Frame cycle's `dt`, asking for frames while they run. That sets the conventions (what is
written, how a transition is interrupted, where its state lives); sizes and positions wait.

### Tooltips, the right button, context menus

- **`tooltip="..."`** shows its text in an overlay at the pointer after it has rested on the
  node 0.6 s, in std's `tooltip` style, gone when the pointer leaves or presses.
- **The right button** presses as the left does but fires `context=event` (with the pointer's
  position in the event's value pair, and to the handler as `(id, x, y)`); std's
  `context-menu` opens a menu at the pointer from a handler.

### The widgets: `krome/std`

Each is a definition, std's styles in both themes, and where it needs one, a behaviour in
`std.lua`:

| widget | written | behaviour |
|---|---|---|
| `slider` | `slider @level 1..99 step=1` | drag and the arrows set the value |
| `dropdown` | `dropdown @kind { option "texture" ... }` | opens a list below, picks with the pointer or the arrows and Enter |
| `radio` | `radio "Fast" @mode=fast` | `@key=value` |
| `tabs`, `tab`, `page` | `tabs @tab { tab "Rules" name=rules ... }` and `page name=rules { ... }` | the arrows move between tabs |
| `tree` | `tree @open { each node in nodes height=22 { tree-row ... } }` | expanding and collapsing writes the model's open set; the arrows walk it |
| `table`, `column`, `cell` | a header row of columns (sortable, resizable) over a virtualized `each` | a header press sorts (an event), a column's edge drags its width |
| `splitter` | `splitter @split { pane ... pane ... }` | its handle drags the split |
| `progress` | `progress @done` | none: a bound width |
| `tooltip` | `tooltip="..."` on anything | the kernel's |
| `menu`, `menu-item`, `menu-bar`, `context-menu` | as now, and a bar of menus | the arrows move, Enter picks, Escape closes |
| `dialog` | `dialog "Title" { ... }` opened modal, with `ok` and `cancel` | Enter is ok, Escape cancel |
| `scroll-view` | `scroll-view { ... }` | the kernel's bar |
| `badge`, `field`, `button`, `checkbox`, `input`, `textarea` | as now, focusable, with focus rings | |
| `secret` | `input secret` | an edit's flag: drawn as bullets, never copied (slice 3's decision) |

### The five screens

On sample models in the shapes goldberg has (`examples/kromescreens/assets/`):

- **Inspection**: a tree of an asset's upstream and downstream; for the selected one its
  type, resolved path, parameters and the rules that governed each, every rule a link that
  inspects its `.berg` file. A splitter between the tree and the details.
- **Diff review**: the lockfile's re-routes in a table, each with its old and new route side
  by side and accept and reject; accept all and reject all; a dialog to confirm.
- **Live status**: steps in a table, virtualized (the 10,000-row case), with a progress bar
  each and a state badge, updated from the sample model as a cook would; connected targets
  in a side list.
- **Reports**: warnings grouped in tabs (fallback matches, out-of-pack loads, budgets), each a
  sortable, filterable table; a context menu on a row ("inspect", "copy path").
- **Forms**: a `.berg` file's rules as a list; the selected one in a form (pattern, type in a
  dropdown, a slider for a number, checkboxes, a free-text meta field) that writes the model,
  with save (Ctrl+S) and revert; every field reachable by Tab.

## Out of scope

goldberg's window itself, wired to its session (goldberg's G16). Vector shapes and icons
(slice 5): widgets draw with boxes and text, and an arrow is a glyph. Variable-height
virtualized lists. Animated sizes and positions. Drag and drop between widgets. Rich text
spans. Grid areas. Multiple windows. The text input protocol beyond composition and the
accessibility tree (K25, K26). Metal and D3D12 drawing the UI.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KW1 | A prelude's scripts run, once per state, before the screens that use it; they return behaviours | P0 |
| KW2 | `behave=`: press, drag, release, wheel, key, focus and hover to the behaviour, with the instance (`w`: id, rect, value, set, state, emit, open) | P0 |
| KW2a | C++ behaviours: `krome_behaviour` from a module, the same calls and instance; one in the tests | P0 |
| KW3 | Keyboard focus: focusable nodes, Tab order (`taborder`), the focused look, Space and Enter, keys to a focused behaviour, a modal holding the focus | P0 |
| KW4 | Shortcuts: `key="mod+s"` on a node, `mod` per platform | P0 |
| KW5 | Properties bound to the model (`w=@key`), set again when the key changes | P0 |
| KW6 | `@key=value` bindings | P0 |
| KW7 | Percent sizes | P1 |
| KW8 | Scrollbars: drawn, dragged, paged; `scroll=x`, `scroll=xy`, Shift+wheel | P0 |
| KW9 | Virtualized `each` (fixed height): only what is in view built and painted | P0 |
| KW10 | Transitions between looks: colors and opacity | P2 |
| KW11 | `tooltip=`; the right button and `context=` | P1 |
| KW12 | std: slider, dropdown, radio, tabs, tree, table, splitter, progress, menus and a menu bar, context menu, dialog, scroll-view, focus rings on what exists | P0 |
| KW13 | An edit's `secret` | P1 |
| KW14 | The vocabulary (`kimb`) and `kimc check` for all of the above | P0 |
| KW15 | `examples/kromescreens`: the five screens on sample models, `--demo` driving each by pointer and keyboard | P0 |
| KW15a | kromedemo's second page, the widget gallery, and navigation between its pages; the form page's pixels unchanged against the Lua form | P0 |
| KW16 | The 10,000-row table's numbers; the pictures in both themes; the READMEs; the master PRD corrected | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **What widgets read and draw** (KW5 to KW8, KW11). Bound properties, `@key=value`,
   percent sizes, scrollbars, tooltips, the right button. Headless tests.
2. **Behaviour and the keyboard** (KW1 to KW4, KW13). Prelude scripts and behaviours in Lua
   and in C++, the instance; keyboard focus, Tab order, activation, shortcuts; `secret`. A
   slider behaviour written each way drives the same markup headless.
3. **Many rows, and motion** (KW9, KW10). Virtualized `each`; transitions. The kernel is
   frozen after this milestone: the next two may not change it.
4. **The widgets** (KW12, KW14, KW15a). std's definitions and behaviours, both themes;
   kromedemo's gallery page and its navigation.
5. **The five screens, and close** (KW15, KW16). `examples/kromescreens`, `--demo`, the
   numbers and pictures, the READMEs, the master PRD corrected where this slice proved it
   wrong.

## Decided

- **A widget can live in either place**: a behaviour in a prelude's Lua or in C++, registered
  by a module; std's are Lua. The kernel knows neither by name.
- **The five screens are examples**, on sample models. goldberg makes its own pass once Krome
  is an MVP.
- **Bound properties use the binding's form**: `w=@left_w`.
- **Virtualized `each` is fixed-height** for now.
- **Transitions start with colors and opacity**, to set the conventions.
- **Shortcuts reach the top screen** only, for now.
- **kromedemo gets a second page**, a widget gallery, with navigation between the pages.
