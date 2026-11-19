# Krome Slice 6 — Live

Nov 18, 2026 · @starekrow · draft, first review folded in

## Goal and exit criteria

Slice 6 closes the loop the design opened with: what makes web UI good is "a flex layout
model, a handful of drawing primitives, good text, reactive state and hot reload"
(`docs/design/krome.md`, "Why"). Slices 1 to 5 built the first four. This one builds the
fifth, and proves the whole of Krome where the master PRD said it would be proved: in
goldberg's window.

Two halves, each useless without the other:

- **Live.** Save a `.kim`, a prelude, a theme, a screen's Lua, an SVG or a PNG, and a
  running program shows it, without a restart and without losing what it was doing: the
  model, the focus, the scroll, the open tab. A save that is wrong keeps the last good
  screen and names its line.
- **goldberg's window.** `goldberg watch --window` opens a Krome window over the running
  session, on the same commands as its CLI (G16). The window's own screens are assets that
  goldberg cooks for itself, so editing goldberg's UI while it runs is the first and
  everyday use of the first half.

The master PRD's "Done, for the whole of it" is goldberg's window showing inspection, diff
review, live status, reports and forms over the same commands as its CLI. This slice is
that, with one substitution for diff review (see "Diff review without a lockfile").

It is done when:

- **The asset module reloads by path.** `asset_reload(e, path)` reads a loaded file again
  and, if its bytes changed, rebinds the same handle to the new contents. Textures,
  materials, shapes and blobs reload; what holds the handle sees the new asset on the next
  frame and needs no code of its own to do so. An unchanged file does nothing.
- **Krome's documents are assets.** `.kimb` files and Lua scripts are read through the
  asset module (blobs, in scopes), not through Krome's own I/O, so they reload like
  everything else. `krome_provide` stays.
- **Krome rebuilds on a rebind:**
  - a screen's `.kimb`: that screen, and its overlays, built again from the new document;
  - a prelude's `.kimb` (its definitions, styles, themes): every screen whose scope uses it;
  - a screen's or a prelude's Lua: its handlers and behaviours replaced, its model kept;
  - a shape or a texture: the nodes that draw it, painted again (laid out again if its
    natural size changed).
- **What a person was doing survives a reload.** The model's values, the focus (by id), each
  scroll offset (by id), the current theme, an `@key=value` tab, a tree's open set. The
  rebuild that a theme switch does today is the model for this.
- **A bad save keeps the last good screen.** A `.kim` that does not compile fails in
  goldberg's cook and nothing reaches the program; one that compiles but does not resolve
  (a style nobody declared, a token no theme has) fails at the rebuild. Either way the
  screen stays as it was, and a dev build shows the error with its file and line in a
  strip Krome draws over the window until the next good load.
- **A running program hears of what changed.** A dev build that links `asset_live`
  connects to `goldberg watch` and is told which outputs changed; the asset module reloads
  them (G15's down channel). goldberg's own window hears it in process. Live reload is
  optional: a program without `asset_live`, or run with `--no-goldberg`, runs as it does
  today.
- **goldberg's window works on a real tree.** `goldberg watch --window` over arcfire's roots
  (and over kromedemo's, kromescreens' and goldberg's own) shows:
  - **Status**: stable or working, the counts, each running step, each failure and each
    soft warning, live as the session moves; the programs connected to it.
  - **Inspect**: any file's `explain` answer as a page: type, metadata with the rule that
    set each key, the converter path, lands or not, the output, the step's state; its
    upstream and downstream (source, work files, outputs) as a tree; each rule a link that
    opens its `.berg` file.
  - **Reports**: failures and soft warnings in tabs (the fallback copy, "declare this
    output", a rule file that stopped parsing), each sortable and filterable, each row
    opening Inspect.
  - **Rules**: a `.berg` file's `meta` and `rules` as a form, saved back to the file with
    its comments and order kept (`bergedit`, as `learn` does).
  - **Diff review**: a form's edit shown, before it is saved, as the assets whose answers it
    changes, old beside new, to accept or reject (see below).
- **The window edits itself.** With `goldberg watch --window` running, saving
  `tools/goldberg/assets/ui/status.kim` (or `krome/std.kim`, or `status.lua`) changes the
  window within a frame of the cook landing, with the session's state on screen untouched.
- **Tests and numbers.** `asset_tests` reload each type headless; `krome_tests` reload each
  kind of document and check what survives; a goldberg test runs a watch with a dev build
  connected, saves files, and checks what the build reloaded; `goldberg watch --window
  --demo` drives each page and checks it against the session. The time from a save to the
  frame that shows it is measured for a `.kim`, a prelude, a PNG and an SVG.

## What the code has

- **No reload anywhere.** The asset module's README lists "hot reload of files that changed
  on disk" under Not in this module. A path is loaded once; a second load is the same
  handle; a record's contents never change after it is `Ready` except by a CLI `patch` of a
  material. Slice 5's `.kshape` was meant to "reload on change", and as built waits for this
  slice (KP8).
- **Handles are already the right shape.** A texture asset's renderer texture, a material's
  row and a shape's record are named by handle, and everything that draws holds the handle,
  not the bytes. Rebinding in place is what the design meant by "the best version currently
  available".
- **Krome reads its own documents** (`modules/krome/src/doc.cc`): each `.kimb` and script
  once, by path, through the platform's I/O queue or `krome_provide`, kept for the run in
  `KromeModule::docs`. Fonts and shapes come from the asset module; documents do not.
- **Krome can already rebuild everything without reading anything.** `krome_theme` rebuilds
  every screen and its overlays from the kept documents; focus is found again by id and
  scroll offsets survive a rebuild by id. A reload is the same rebuild from a new document.
- **goldberg watches and cooks live.** `goldberg watch` cooks what is saved, keeps a bad
  `.berg` file's last good version, holds a CLI on stdin and on `--cli-port`, and writes
  each output whole (a temporary name, then a rename). It does not know a program is
  running, and nothing it does tells one.
- **The `kim` converter does not report its `use`s.** A prelude saved does not re-check the
  screens that use it; goldberg's README says the check "waits for discovered edges"
  (goldberg slice 2, not built).
- **goldberg is headless.** Its module links `core`, `platform`, `cli`, `tool`, `lua`,
  `script`, `math`, `kimb`, `kim` and `svg`: no renderer, no window, no Krome. It runs on
  `tool_main`. `docs/design/cycles.md` leaves "a windowed goldberg needs its CLI request run
  the way `tool_main` runs it" to goldberg's window.
- **A tool with a window exists.** The frame loop with the tool's work in the Work cycle
  (`kromedemo --work`): 86 to 87% of wall time computing behind a live window.
- **goldberg's five screens exist on sample models** (`examples/kromescreens`), out of
  `krome/std`, with `--demo`. Their models are shaped like goldberg's but are not
  goldberg's: no session behind them, and a lockfile goldberg does not have.
- **The pieces a connection needs are here.** `platform/net.h` (non-blocking TCP,
  `net_connect_local`), the CLI's line protocol, `fs_watch` natively on Linux, Windows and
  macOS.

## The shape

### Where things are

| path | what |
|---|---|
| `modules/asset/` | `asset_reload`, the rebind per type, `AssetReloaded`; a reload source interface; the CLI's `reload PATH` |
| `modules/asset_live/` | NEW, optional. A dev build's connection to `goldberg watch`: connect, say who it is, hear `changed PATH`, call `asset_reload`. A program that does not link it does not reload live |
| `modules/platform/` | `platform_open_path`: a path handed to the OS to open in its default application |
| `modules/krome/` | documents and scripts as blob assets; the rebuild on `AssetReloaded`; Lua reload with the model kept; the dev error strip |
| `tools/goldberg/` | the listener for programs; discovered edges from `kim`; `--window`; the window's model and commands |
| `tools/goldberg/assets/ui/` | NEW. goldberg's screens (`.kim`, `.lua`), out of `krome/std`, cooked by goldberg for itself |
| `examples/kromescreens/` | stays, as the sample-model gallery of the five screens |

### Reload in the asset module

**One call.** `asset_reload(e, path)` finds the record by path (a path not loaded does
nothing) and submits a read. When the bytes land:

1. **Hash them.** If they are what the record was made from, stop. goldberg already drops
   identical writes; this holds when the trigger is not goldberg (`reload PATH` from the CLI,
   a test).
2. **Decode beside the old.** The new contents are built into a fresh record body while the
   old one keeps drawing. A decode that fails keeps the old one and sends `AssetFailed` with
   `reload = true`: the program goes on with what it had.
3. **Swap at the pump.** In `PreSim`, the same place loads land, the record takes the new
   body and `AssetReloaded{asset}` is sent at the next phase boundary. The old body is
   retired past the frames in flight, as a released asset is.

**Per type.**

| type | what a rebind does |
|---|---|
| texture | the same renderer texture handle, new pixels; a new size recreates the GPU texture behind the handle, as every backend's whole upload already does |
| material | the row patched in place, as `asset_material_patch` does; a new texture path loads and rebinds |
| shape | a new `PathShape`; Krome drops its cached pictures of that shape |
| blob | new bytes and a generation; whoever parses it hears `AssetReloaded` (Krome's documents, a game's own formats) |
| font, sound, stream | not in this slice (see Out of scope) |

**Who triggers it does not matter.** The design's "Krome redraws when a handle rebinds, and
does not care who rebound it" holds for the asset module too. Three triggers in this slice,
each a few lines over `asset_reload`:

- the CLI: `reload PATH` (and `reload` alone: everything loaded, hash-checked);
- goldberg, in a dev build connected to a watch (next section);
- goldberg's window, in process.

**Cosmetic, mostly.** Reloading a texture, a material, a shape or a Krome document changes
nothing a replay hashes. A blob a game's sim reads (a level) does. The asset module does
not decide: `AssetReloaded` is an event like `AssetReady`, and a game that reads blobs into
its sim either ignores it or knows that its replays stop matching from there. Reload is on
in dev and debug and absent from ship.

### A program's connection to goldberg

The master goldberg PRD decided it: the game dials goldberg, says who it is, and listens
(G15). This slice builds the down half, the half reload needs.

- **Finding the watch.** A running `goldberg watch` writes its port beside its lock
  (`<state>.port`). A dev build already finds its output tree through `<exe>.assets`;
  from there it finds the state and the port with no flag. `--goldberg [HOST:]PORT` says
  it outright, `--no-goldberg` turns it off. No watch running is not an error: the program
  runs from its tree as it is, and connects when one starts (a retry every couple of
  seconds, in Work).
- **The protocol** is the CLI's line protocol, as decided. Up: `hello PROGRAM BUILD` once.
  Down: `changed PATH` for each output that landed again, `removed PATH` for one that left,
  in the order the session wrote them, after the rename. Asset paths are the output tree's
  (`arcfire/kenney/ships.png`), which are the paths the program loaded.
- **The program side** is the `asset_live` module, which a program links if it wants live
  reload and leaves out if it does not: a small system in Work that reads lines, calls
  `asset_reload` for each `changed`, and logs each `removed` (an asset whose file went keeps
  what it had). It never blocks and never spawns a thread, as the sockets do not. The
  examples link it in dev and debug; nothing links it in ship. `asset_reload` and the CLI's
  `reload` stay in `asset`, for a program without it.
- **goldberg's side** keeps a list of connected programs, which Status shows, and
  `goldberg status` prints.
- **Not this slice:** the up channel's load reports (what the program loaded, what it asked
  for and did not find), KBOR on the wire, a target on another host. Nothing here rules them
  out.

### Krome on a rebind

- **Documents are blobs.** `krome_doc` asks the asset module for a blob in Krome's scope
  instead of reading through the I/O queue. A `.kimb` is decoded when its blob is ready, and
  again when it reloads; a script keeps its text, and is run again when it reloads.
- **What is built again.** On `AssetReloaded` for a document:
  - **a screen's `.kimb`**: its scope is built again (a `use` it gained is loaded first, as
    at open); then each screen opened from that file is rebuilt, overlays included, as
    `krome_theme` rebuilds them today;
  - **a prelude's `.kimb`**: every screen whose scope reaches it, through any chain of
    `use`s;
  - **a screen's Lua** (`script` lines, or a screen that is a script): run again in a fresh
    environment; its `on` handlers replace the old; its `model` table's keys that the
    running model does not have are added, and keys it already has keep their running
    values (a new default does not overwrite what a person typed);
  - **a prelude's Lua** (its behaviours): run again; the behaviour table replaced; each
    widget's `state` table kept while its id lasts; every node with a behaviour gets
    `update(w)` again.
- **What survives.** The model; the focus, by id; scroll offsets, by id; the theme by name
  (if the new prelude has no theme of that name, the first, with an error); transitions,
  which start from where the old nodes were, as after any rebuild; a virtualized `each`'s
  position. An edit's text is the model's; its caret and selection survive when its id does.
- **A failed rebuild keeps the old tree.** The new scope is built into a scratch first (as
  `krome_file_scope` does while a use is on its way); a mistake leaves the screen as it was,
  records the error, and waits for the next reload.
- **The dev error strip.** In dev and debug, while a screen's last reload failed, Krome draws
  a strip along the top of the window: the file, the line and the message, the use's line
  too when the mistake is in a definition's body. It is an overlay of Krome's own, in no
  prelude's look, that the pointer goes through; a good reload or `krome_clear_errors`
  takes it down. Ship has none.
- **Shapes and textures.** A shape that rebinds drops its cached pictures (every size and
  colour) and marks the nodes that draw it; their next paint rasterizes it again. A texture
  that rebinds marks the nodes that draw it; one whose size changed marks layout too.

### goldberg cooks for itself

- **The window's screens are goldberg's assets**, under `tools/goldberg/assets/ui/`, using
  `krome/std`. kormake's cook of goldberg puts them in goldberg's output tree, as for any
  program, and goldberg reads them from there.
- **A watch with a window watches its own UI too.** `goldberg watch --window` runs a second
  session, in the same Lua, over goldberg's own roots (its `assets/` and `krome`'s) into its
  own tree. Each output that session lands is reloaded in process: no socket. The two
  sessions share the loop, the I/O queue and the workers, and nothing else: goldberg's UI
  never appears in the game's tree, its graph or its counts.
- **Start order**, as the toolchain design asks: goldberg cooks its own UI first, then opens
  the window. A UI file that fails to cook is reported on the terminal, and the window opens
  with what is in the tree, or not at all when nothing is.

### Discovered edges, for `kim` only

- The `kim` converter reports the `use`s it compiled as edges from the screen to each
  prelude's source.
- A prelude saved re-runs the check of every screen that uses it (the check, not the
  compile: a screen's bytes do not depend on its preludes). A screen the new prelude breaks
  fails in the cook with its line, as if the screen had been saved.
- The running program does not need the edge to reload: it reloads the prelude it holds and
  rebuilds what uses it. The edge is so that goldberg reports the mistake, and Status and
  Reports show it.
- This is the smallest form of goldberg slice 2's discovered edges: a builtin converter's,
  kept in the graph and the saved state. Shell and Lua converters reporting edges stay in
  goldberg slice 2.

### goldberg's window

**One process, two front ends.** The window is goldberg, not a client of it: the session's
Lua answers it as it answers the CLI. Everything the window does is a command the CLI has
(or gains in this slice), so anything the window shows can be asked from a terminal.

- **The loop.** `goldberg watch --window` runs on the frame loop instead of `tool_main`: the
  session in the Work cycle, Krome in Frame, as `kromedemo --work` does. The CLI's requests
  run the way `tool_main` runs them. A still window and a stable session sleep.
- **`cook`, `watch` without `--window`, `explain`, `learn` and `clean` are unchanged**, and
  start no window system: goldberg still runs where there is no display. The window's modules
  are linked but not started.
- **Models.** goldberg's Lua builds each page's model from the session and sends it to Krome
  as KBOR (`krome_set_doc`), at most once a frame, and only what changed: Status's steps as
  they start and end, not the whole table each time. Krome's events (`KromeEvent`) come back
  as commands.
- **Pages.** One window, a page a tab, as kromescreens has them:

  | page | shows | commands under it |
  |---|---|---|
  | Status | stable or working, the counts, the running steps (a virtualized table), failures and warnings, the connected programs | `status`, `cook` |
  | Inspect | a file's answer, every key with its rule, the converter path, lands, output, state; upstream and downstream as a tree; each rule a link to Rules | `explain PATH`, `touch PATH` |
  | Reports | failures and soft warnings in tabs, sortable and filterable; a row opens Inspect; a context menu copies the path or touches the file | `status` (its lists) |
  | Rules | a `.berg` file as a form: `meta`, each rule's `match` and `meta`; add and remove a rule; save writes the file with its comments kept | `berg PATH` (read), `berg-set PATH ...` (write, new) |
  | Diff | a Rules edit's consequences before it is saved | `berg-preview PATH ...` (new) |

- **Diff review without a lockfile.** The design's diff review is the lockfile's re-routes,
  and goldberg has no lockfile (the Shipping slice). The same screen fits the question a
  rule edit raises today: what will this change? The solver is a pure function, so goldberg
  solves every file the edited `.berg` speaks for under the edit, without applying it, and
  shows each file whose answer changed: type, path, lands, output, old beside new. Accept
  saves the edit (and the watch cooks it, as for any save); reject drops it. When the
  lockfile comes, its re-routes are rows in the same screen.
- **Opening files.** A rule's link opens its `.berg` file in Rules; "open in editor" hands
  the path to the OS (`platform_open_path`, new: `xdg-open` on Linux, `ShellExecuteW` on
  Windows, `NSWorkspace` on macOS), which opens it in the application the person has set for
  the file's type. An override (`$VISUAL`, a setting) can come later if it is wanted.

## Out of scope

- **Fonts, sounds and streams reloading.** A font's strikes are shared by every size and
  text that uses them; a sound may be mixing on the device thread. Each is its own work, and
  nothing in this slice's use case needs them.
- **The up channel**: load reports, out-of-pack loads, what a program asked for and did not
  find (G15's other half). Reports shows what goldberg knows, not what a program saw.
- **Remote targets**: a phone, a devkit, goldberg on another host. Loopback only, as the
  CLI is.
- **A sim reloading safely.** A game that reads a reloaded blob into its sim gets no help
  with determinism.
- **Lua code reload beyond handlers and behaviours.** Upvalues of a screen's old script are
  not carried over; a handler that closed over a local gets the new script's local.
- **The lockfile, profiles and packs** (goldberg's Shipping): the diff screen waits for them
  in the form above.
- **Multiple windows.** goldberg's window is one, with pages.
- **The text input protocol and the accessibility tree** (K25, K26).
- **goldberg's window from another process** (a `goldberg ui` that dials a watch). The window
  speaks the CLI's commands, so it can be split off later without changing them.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KL1 | `asset_reload(e, path)`: read again, hash, decode beside the old, swap at the pump, `AssetReloaded`; an unchanged file does nothing; a failed decode keeps the old | P0 |
| KL2 | Textures, materials, shapes and blobs rebind in place; a texture's size may change | P0 |
| KL3 | The CLI's `reload [PATH]` | P0 |
| KL4 | Krome's `.kimb` documents and scripts read as blob assets in Krome's scope; `krome_provide` kept | P0 |
| KL5 | A screen's `.kimb` reloaded: its screens and their overlays rebuilt from the new document | P0 |
| KL6 | A prelude's `.kimb` reloaded: every screen whose scope reaches it rebuilt | P0 |
| KL7 | A screen's Lua reloaded: handlers replaced, the model kept, new keys added | P0 |
| KL8 | A prelude's Lua reloaded: behaviours replaced, widget state kept by id, `update` again | P1 |
| KL9 | Through any reload: the model, focus, scroll offsets, theme, tabs, a tree's open set, an edit's caret | P0 |
| KL10 | A failed reload keeps the last good tree; the dev error strip shows the file and line until a good one | P0 |
| KL11 | A shape or texture rebind redraws what draws it, laid out again if its natural size changed; a still screen still costs nothing | P0 |
| KL12 | `asset_live`, optional: a dev build that links it finds a running watch by itself and connects; `--goldberg`, `--no-goldberg`; reconnects when a watch starts; a program without it is unchanged | P0 |
| KL13 | goldberg's listener: `hello`, then `changed` and `removed` after each rename; connected programs in `status` | P0 |
| KL14 | The `kim` converter's `use`s as discovered edges; a prelude save re-checks its screens | P1 |
| KL15 | `goldberg watch --window` on the frame loop; the session in Work; the CLI as before; other commands start no window | P0 |
| KL16 | goldberg cooks its own UI first, in a session of its own, and reloads it in process | P0 |
| KL17 | Status, Inspect and Reports over the live session | P0 |
| KL18 | Rules: a `.berg` file as a form, saved with its comments and order kept | P1 |
| KL19 | Diff: a Rules edit's changed answers, old beside new, accepted or rejected before it is saved | P1 |
| KL20 | Every window action is a CLI command; the new ones (`berg`, `berg-set`, `berg-preview`) documented in goldberg's README | P0 |
| KL20a | `platform_open_path` on Linux, Windows and macOS; "open in editor" uses it | P1 |
| KL21 | `goldberg watch --window --demo` drives each page and checks it against the session | P0 |
| KL22 | Numbers: save to frame for a `.kim`, a prelude, a PNG and an SVG; the window's cost during a 10,000-step cook (the heavy test with a window) | P1 |
| KL23 | READMEs (asset, krome, goldberg); the master PRDs corrected (Krome's K22, K23; goldberg's G15, G16) | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **Rebind** (KL1 to KL3). `asset_reload` and the swap for textures, materials, shapes and
   blobs; the CLI's `reload`. Headless tests per type: changed, unchanged, failed, a texture
   that grew, a material whose texture path changed. The renderer's resized texture behind a
   handle, on every backend.
2. **Krome reloads** (KL4 to KL11). Documents as blobs; the rebuilds; Lua reload with the model
   kept; what survives; the error strip. `krome_tests` reload each kind and check the model,
   the focus, the scroll and the pixels; `kromedemo` and `kromescreens` reload from the CLI's
   `reload` while their demos run.
3. **The connection** (KL12 to KL14). goldberg's listener and the port file, the dev build's
   side, `kim`'s edges. A goldberg test: a watch, `kromedemo` connected, a `.kim`, a prelude, a
   PNG and an SVG saved, each seen reloaded; a bad save seen as an error with its line in the
   cook and in the program; the numbers.
4. **The window** (KL15 to KL17, KL20). `--window`, the frame loop under goldberg, the second
   session for goldberg's own UI; Status, Inspect, Reports on the live session, written fresh
   in `tools/goldberg/assets/ui/` from kromescreens' screens. The window editing itself.
5. **Forms, diff and close** (KL18, KL19, KL21 to KL23). Rules and Diff; `--demo`; the heavy
   test with a window; the READMEs; the master PRDs corrected where this slice proved them
   wrong.

## Decided

From the master PRDs:

- **Reload is a rebind.** Handles stay; contents change under them.
- **The game connects to goldberg**, and the protocol is the CLI's lines to begin with.
- **goldberg starts headless, cooks its own UI, then opens the window.**
- **The UI and the CLI are two front ends over the same Lua core.**

In review of the first draft (Nov 18):

- **A program hears of a change by dialing `goldberg watch`** (G15's down half), not by
  watching its own output tree. One way in, the one that later reaches a phone or a devkit,
  and the one that puts the program in Status's connected list. A dev build with no watch
  running reloads nothing, except by the CLI's `reload`.
- **Live reload is optional.** The connection is a module of its own, `asset_live`, that a
  program links or leaves out; `--no-goldberg` turns it off in one that links it.
- **The trigger is not the asset module's business.** `asset_reload` is the one call; the
  CLI, `asset_live` and goldberg's own window each call it.
- **Krome's documents move onto the asset module** as blobs, so there is one reload path.
- **A reloaded script keeps the model.** New keys are added; existing keys keep their values.
- **The error strip is Krome's**, in every dev build, not goldberg's alone.
- **The window is in goldberg's process**, opt-in with `--window`.
- **Diff review is a rule edit's preview** until a lockfile exists.
- **`kim`'s discovered edges are in this slice**, for that builtin converter only.
- **"Open in editor" is the OS's** (`platform_open_path`). An override can come later.
- **The window is proved on** arcfire's, kromedemo's, kromescreens' and goldberg's own trees.

## Open

The first draft's questions were settled in review (Nov 18; see Decided). What was left:

- [x] **A texture that changes size** (milestone 1): nothing to build. Every backend's
      whole upload (`renderer_texture_upload`) already makes a new image behind the handle
      and destroys the old one past the frames in flight.
- [ ] **The name `asset_live`** is this draft's, and can change before milestone 3.
