# Goldberg — Master PRD

Nov 14, 2026 · @starekrow

## What it is

goldberg is Korvid's asset orchestrator. You say what an asset is; goldberg works out
what to do with it, in what order, and where the result goes. It cooks a game's source
assets into an output tree, once or continuously, and later pushes what changed into a running
game. The design is `docs/design/asset-toolchain-design.md`; this document turns it into
something to build, and settles the parts the design left open that the first slices
cannot do without.

Today nobody cooks anything. kormake copies each module's `assets/` to
`bin/assets/<module>/`, kpack packs that copy in ship, and the tools that make assets
(`examples/arcfire/tools/*.lua`) are run by hand and their output checked in. goldberg
replaces the copy first, and then grows into the rest.

**One process, one loop, one graph.** A batch cook and a live session are the same
program doing the same thing: watchers up, a scan that feeds every file to the loop as if
it had just changed, and work until the graph is stable. A cook exits there; a live
session keeps listening. There is no second way to run the graph for "build everything".

A cook needs the loop and the watchers as much as a live session does, because goldberg
does not decide when files appear. Picture a level cooker spawned into the background: it
hums for a while, drops a `.map`, hums some more and starts dropping assemblies. Each of
those is a file arriving, and each gets solved, placed in the graph and cooked as it
arrives, while the cooker is still running. That is the live case, inside a batch. It is
not the common case: most tools write outputs known ahead of time, and goldberg accepts
them when the process exits (see The cook).

## Goals

- **The copy goes away.** No one copies assets. goldberg cooks `<module>/assets/` into
  an output tree (`build/assets/<game>/` in dev and debug, `dist/<game>/assets/` in ship), and
  the game reads them from there. Some cooks are copies; that is goldberg's
  decision, recorded like any other.
- **Zero rules on day one.** A project with no `.berg` file cooks correctly from
  conventions alone. Arcfire and arcanum cook with nothing added to their trees.
- **Batch is live that stops.** `goldberg cook` and `goldberg watch` differ only in what
  happens when the graph goes stable. Both run the loop, and both run the watchers.
- **Outputs are declared; watchers catch the rest.** Most tools say ahead of time what
  they write, and goldberg takes those files into the graph when the process exits. The
  watchers are for what that misses: a tool that drops files as it runs, an output nobody
  declared, and a person saving a file.
- **Rules can change at any time.** Editing a `.berg` file is an event like saving a
  PNG. Its consequences cascade through the graph and come back to the loop as work.
- **Every decision can be explained.** For any file: what it is, what will be done with
  it, where it lands, and which file and line decided each of those.
- **Built on the engine.** goldberg is an engine tool (`modules/tool`), with the engine's
  I/O, pools, CLI and Lua. What it needs and the engine lacks (file watching, child
  processes) is added to the engine, not to goldberg.

## Non-goals

- Running game code, or hosting play-in-editor (the design's non-goals stand).
- Replacing kormake. kormake builds code and calls goldberg for assets, as it calls
  kpack and the shader compiler today.
- For the first slices: tool monitors (Aseprite, Blender), write-back and reverse
  mutators, background refinement, the lockfile, pack closures and budgets. They are in
  the design and in Later below; nothing in the first slices may rule them out.

## Frame

- **Where it lives.** `tools/goldberg/`, module `goldberg`, a host tool like `kpack`:
  `tool_main`, commands in the CLI, work in systems.
- **Lua on C++.** As the design says: the asset model, the solver, the graph and the
  reports are Lua; C++ owns pools, I/O, hashing, watching and spawning. The loop's queue
  and the graph's nodes are engine pools, so they show up under `/kinds` in goldberg's
  own CLI for free.
- **JSON for now.** `.berg` files are JSON. KBOR (`docs/design/kbor.md`) is the cooked
  form of data assets and, later, the wire format; nobody writes it by hand.
- **Out of process, always.** goldberg never loads a game module. A game that wants to
  hear from it opens a connection to it.

## The pieces

### The event loop

One queue, drained by one system, in order. Everything goldberg does starts as an event
on it, and everything a piece of work produces goes back onto it.

| Source | Events |
|---|---|
| Scan | `file-seen` for every file under a watched root, once at startup |
| Filesystem watchers | `file-changed`, `file-added`, `file-removed`, `dir-added`, `dir-removed`; over the asset roots and the work directories tools write into |
| CLI | `command` (cook, explain, status, touch, quit), from stdin, a TCP session or `cli_run` |
| UI | the same commands, from goldberg's window (Krome, later) |
| The game | `target-connected`, `target-gone`, `load-report`, requests (later) |
| The graph builder | `node-dirty`, `node-removed`, `edge-discovered` |
| Cook jobs | `cook-started`, `cook-output` (as each lands), `cook-done`, `cook-failed`, discovered edges |

- **Events are data.** A row with a kind, a path or node, an origin and a sequence
  number. The log of events is enough to replay a session, which is how the loop is
  tested.
- **Nothing blocks.** The loop decides; reads, hashes, writes and converters run on the
  I/O queue, the job pool or in a child process, and report back as events.
- **Coalescing.** Several events for one path between two drains are one event. An
  editor that writes a temp file and renames it produces one `file-changed`.
- **Hashes decide.** A `file-changed` whose content hash is what the graph already holds
  is dropped. mtime and size only decide whether to hash.
- **Watchers first.** They start before the scan, in a cook as in a live session, so a
  file written during the scan is not missed; the hash drops the duplicate.
- **Stable.** The graph is stable when the queue is empty, no job or child process is
  running, no step is inside its `settle` time, every watcher has delivered what it
  has, and no node is dirty. A cooker humming in the background holds stability off for
  as long as it lives. `cook` exits
  there, 0 if nothing failed and 1 otherwise. `watch` prints a line and waits.
- **The only thing a cook leaves out** is the game's listener. Nothing else differs.

### Metadata and rules

Metadata is what is true of an asset: its type, its parameters, its tags. A rule is
metadata with a pattern in front of it. They are the same schema in the same files, and
every file contributes to one effective answer per asset.

**The files.** All are `.berg`, all are JSON, and none is ever cooked into the output tree.

| File | Speaks for |
|---|---|
| `goldberg.berg` in a directory | that directory and everything under it |
| `goldberg.berg` at an asset root | the same, so: the module's assets |
| `goldberg.berg` beside `korvid.config` or `kormake.lua` | the project |
| `ship.berg` | the files named `ship.*` beside it (`ship.png`, `ship.json`) |
| the conventions | everything; a `.berg` file that ships inside goldberg |

**The shape.** One object. `meta` speaks for the file's whole subject; `rules` narrow it
by pattern. A sidecar is usually just `meta`; a directory file is usually just `rules`;
either may hold both.

```json
{
  "meta": { "tags": ["stage-1"] },
  "rules": [
    { "match": "*.png",        "meta": { "type": "texture.color.srgb", "filter": "nearest" } },
    { "match": "bosses/**",    "meta": { "tags": ["bosses"] } },
    { "match": "notes/**",     "meta": { "cook": false } },
    { "match": "*.material.json", "meta": { "type": "material" } }
  ]
}
```

```json
{ "meta": { "type": "sprite", "pivot": [0.5, 0.8] } }
```

- **Patterns** are globs relative to the `.berg` file's directory; `**` crosses
  directories. A sidecar's `match` is tested against the full file name, so `ship.berg`
  can say something about `ship.png` alone.
- **Order.** Conventions, then project, then each directory from the asset root down,
  then the sidecar. Within a file, `meta` first, then `rules` in the order written. Later
  wins.
- **Merging** is per key. Objects merge key by key, scalars and arrays replace, `null`
  removes a key set by an earlier layer. `tags` is the exception: a set, with `"-name"`
  to remove one.
- **Provenance.** Every key of an asset's effective metadata remembers the file, and the
  rule in it, that set it last and those it overrode. This is what `explain` prints.
- **Converters are registered here too.** A `converters` object names each one, its
  signature and how it runs (see The cook). A converter is in scope wherever the file
  that registers it speaks.
- **What a file may also hold**, as later slices need them: `edges` (an explicit edge
  where inference is ambiguous), `profiles` (parameters by build profile). Unknown
  top-level keys are an error that names the file; unknown keys inside `meta` are kept
  and passed to converters.
- **Errors.** A `.berg` file that does not parse fails the assets it speaks for, and
  nothing else, with the file, the line and the previous good version still in force
  during a live session.

### The solver

Given one file and its effective metadata, the solver answers: what is this, and what is
done with it. It is a pure function of the file's name, its metadata and the registered
converters, so it can run for one file, for a thousand, or for none (`explain`).

- **Type.** From `meta.type`, which the conventions set by extension and directory. No
  type means the fallback: copy, with a soft warning ("consider declaring this").
- **Target.** The runtime type the asset is wanted as. By default, what the conventions
  say that source type cooks to.
- **Path.** A chain of converters from the source type to the target type. Infer when
  the path is unique; when it is not, fail with the candidates and the one-line `edges`
  entry that picks each.
- **`copy` is not special.** It is a built-in converter from any type to itself, chosen
  by the same search, keyed, skipped, explained and failed like any other. The eventual
  KBOR encode is another. Nothing in the loop, the graph or the cook knows that a step
  is "only a copy".
- **Terminal.** The solver also answers whether a file lands in the output tree (below).
- **Output.** Where each result goes. A terminal step writes to the output tree, by default at
  the source's path under its module; any other step writes to the work directory. A
  converter that changes the format changes the extension.
- **Files a converter made** are solved like files a person saved, with two additions:
  their type is the converter's declared `to` (a rule may refine it, not contradict
  it), and they match rules as if they sat beside the source they came from, so the
  `goldberg.berg` over `ships/` also governs what `ships/big.sheet` turned into.
- **Nothing.** `"cook": false` is an answer too: the file is known, watched and ignored.
- **The answer is a record**: type, target, path, parameters, outputs, and for each the
  rule that decided. The graph builder consumes it; `explain` prints it.

**When a file is terminal.** Proposed, and the part of the solver most likely to change:

- **Types say it.** Each type in the conventions is marked runtime or not. `texture`,
  `material`, `sound`, `map` are runtime; `sheet`, `aseprite`, `frame-set` are not.
- **A file whose type is runtime lands.** Its path ends in a step that writes to
  the output tree. If it already is what the game reads, that step is `copy`.
- **A file whose type is not runtime does not land.** It needs a converter that takes
  it somewhere, or it is an error ("nothing cooks a `sheet`"), or it is `"cook": false`.
- **A file made by a converter follows the same rule.** The `.map` a level cooker drops
  is runtime and lands, by `copy` from the work directory; the frame set a cutter drops
  is not, and waits to be an input.
- **`"land"` overrides it**, in any `meta`: `false` for a runtime-typed file that is
  only an ingredient (a texture that exists to go into an atlas), `true` to ship
  something as it is (a `.blend` the game's own tool reads).
- **Explained.** `explain` prints `lands` or `does not land`, and the type or rule that
  said so.

### The graph builder

The graph holds sources, rule files, cook steps and outputs, and the edges between them.
The builder is the only thing that changes it, and it accepts changes at any time.

- **Inputs.** Solver answers for files (new, changed, gone), rule files (new, changed,
  gone), and edges discovered by a converter while it ran.
- **Rule files are nodes.** Every asset depends on the `.berg` files that contributed to
  its metadata, and on the places where one could appear. Saving `goldberg.berg`
  re-solves exactly the files under it; adding `ship.berg` re-solves `ship.*`.
- **A re-solve that changes nothing is nothing.** Only a different answer dirties a
  node. A rule edit that leaves every asset's answer as it was cooks no asset.
- **Cascade.** A dirty node dirties what depends on it; the builder posts `node-dirty`
  for each node that is ready to cook (its inputs clean), and the loop schedules it.
  Order is the graph's topology and nothing else.
- **Fan-in.** A step may depend on a set ("every sprite in this directory"). Membership
  is an edge, so a file arriving in the directory dirties the step.
- **Removal.** A source that goes away takes its steps and its outputs with it: the
  output tree never holds a file the graph does not own.
- **The output tree is scanned at startup too.** Every file in it is solved backwards:
  which step would have written this, from which source. A file whose source is gone
  (deleted while goldberg was not running) is removed, with a line saying so; a file no
  rule could have produced is removed the same way. The output tree is goldberg's, and
  nobody else keeps files there.
- **Collisions.** Two steps that claim one output path is an error naming both sources.
- **State.** The graph and the hashes it rests on are saved under `build/goldberg/`, so
  the second cook of an unchanged tree reads, compares and exits. Deleting that directory
  is always safe.

### The cook

- **A step** is one converter run: inputs, parameters, outputs. Its key is the hashes of
  its inputs, its parameters and the converter's version. A step whose key matches what
  produced the outputs on disk does not run.
- **Declared outputs, accepted at exit.** This is the normal case. A converter's
  registration says what it writes (names or patterns, from its inputs and parameters).
  While its process runs, watcher events for those paths are held; when it exits
  cleanly, the files are hashed and enter the graph together. Nothing downstream ever
  sees a file half written, and a failed run contributes nothing.
- **Scanned outputs, by declaration.** A converter that runs long and produces as it
  goes (the level cooker), or cannot say what it will write, says `"outputs": "scan"`.
  goldberg then takes whatever appears in the step's work directory, as it appears: each
  file enters the graph once it has been quiet for the converter's `settle` time, is
  solved, gets an edge from the step, and its downstream steps start while the producer
  is still running. Exit ends the step; it is not what admits the files.
- **If a scanning step fails**, the files it already delivered are withdrawn with
  everything cooked from them, as if the sources had been removed.
- **Undeclared outputs are swept up.** A file that appears in a step's work directory
  and that nothing declared is taken in at exit, attributed to the step, and reported
  once ("declare this output").
- **`settle`, in the tool's metadata.** A converter whose real writer is not the process
  goldberg spawned (it pokes a daemon, or an editor exports on its own schedule) has no
  lifetime goldberg can wait on. That is a flaw in the tool's fit, not something the
  graph models: its registration carries `"settle": seconds`, and goldberg holds the
  step open, and stability off, until its outputs have been quiet that long. With
  `"outputs": "scan"` the same key is simply how a file is known to be finished. On a
  converter with declared outputs it is a patch, and `explain` says so.
- **Intermediates** land in a work directory under `build/goldberg/`, which is watched
  like an asset root. Only a terminal file goes to the output tree.
- **When a long step ends**, what it wrote last time and did not write this time is
  removed, with everything downstream of it.
- **Tool categories.** A converter says what kind of tool runs it, and the category
  owns how it is launched. `builtin` (`copy` now, KBOR encode later). `shell`, a script
  run as a child process, in from the first slice because it models most of what any
  converter needs (a process with a lifetime, inputs, outputs, an exit status, stderr).
  Later: `exe` (a native binary, no shell), `lua` (run inside goldberg on kvscript's
  libraries), and programs that speak a protocol over a pipe, which is also how a game
  supplies its own.
- **A shell converter** is registered in a `.berg` file:

  ```json
  {
    "converters": {
      "cut-sheet":  { "from": "sheet", "to": "texture.color.srgb",
                      "shell": "tools/cut_sheet", "args": "{in} {work}",
                      "outputs": ["{stem}_*.png"] },
      "cook-level": { "from": "level-source", "to": "map",
                      "shell": "tools/cook_level", "args": "{in} {work}",
                      "outputs": "scan", "settle": 0.5 }
    }
  }
  ```

  `shell` names a script without its extension, relative to the `.berg` file. On
  Windows goldberg runs `tool.bat` if it is there and `tool.sh` otherwise; everywhere
  else, `tool.sh`. Finding an interpreter for `.sh` on Windows is the category's
  business, not the converter's. `{in}`, `{work}`, `{stem}` and the asset's
  metadata are substituted, and the metadata is also in the environment.
  Someday the child's environment is also settable: an `env` key in metadata, layered
  like any other, and `.env` files applied the same way. The script
  file is an input of every step that runs it, so editing the script recooks. A non-zero
  exit fails the step, with the tail of stderr in the message.
- **The streaming case is tested with a script.** Create a file, sleep, write, sleep,
  write another, exit; registered with `"outputs": "scan"` and a `settle`. The test
  asserts that each file is admitted once quiet, that its downstream step runs before
  the script exits, and that the cook does not go stable until it has.
- **Outputs are written whole.** To a temporary name, then renamed, so a game reading
  the output tree never sees half a file.
- **Failure is local.** A failed step fails what depends on it and nothing else. The
  message names the asset, the rule that sent it down this path and the declaration that
  would change it; never only a converter's stack trace.

### Where the output goes

- **Dev and debug: `build/assets/<game>/<module>/...`**, the tree that
  `bin/assets/<module>/` is today. The game mounts it; asset paths in code do not
  change (`arcfire/ships/ship.png`).
- **Ship: `dist/<game>/assets/`**, fully cooked. `dist/<game>/` is the shipped game and
  nothing else: the executable and licenses from `kormake package`, and the assets from
  goldberg. goldberg owns the `assets/` directory in it and touches nothing beside it.
- **Which one is the config's**, as it is for code. Until profiles exist the two trees
  hold the same bytes; they are separate so that they can stop doing so.
- **kormake stops staging assets.** A module's `assets/` becomes an input to a goldberg
  rule, not a copy rule. `kormake arcfire` builds the code and runs `goldberg cook
  arcfire`; `kormake package` and kpack read the cooked tree.
- **Roots.** goldberg is told the game and gets from kormake the modules in its closure
  and where their `assets/` are. It does not parse `module.lua` itself.

### The game's connection

The game dials goldberg, not the other way round: a dev build started with
`--goldberg [HOST:]PORT` connects out, says which game and build it is, and listens.
That works the same for a devkit or a phone as for a window on the same machine.

- **Down:** "these outputs changed". The asset module reloads what it holds by path and
  rebinds the handles (the design's best-available handle, in its smallest form).
- **Up:** what the game loaded, and what it asked for and did not find.
- **Protocol:** the CLI's line protocol to begin with, KBOR later.
- **A cook never opens the listener.** A game that finds no goldberg runs from its
  output tree as it is.

## The command line

```
goldberg cook    [GAME] [CONFIG]      cook until stable, then exit
goldberg watch   [GAME] [CONFIG]      cook, then keep watching; listen for games
goldberg explain PATH                 what this file is, what happens to it, and why
goldberg status                       dirty, running, failed; connected games
goldberg clean   [GAME]               remove what goldberg wrote, and its state
```

- `GAME` defaults the way a kormake name does: the module whose directory this is.
  `CONFIG` defaults as kormake's does, and picks the output tree.
- `explain` and `status` against a running `watch` ask it (`--port`); `explain` without
  one solves the file on the spot and cooks nothing.
- While `watch` runs, the same verbs are its CLI, on stdin and on TCP, beside the
  engine's own (`ls /kinds/goldberg/...`, `/mem`, `/perf`).

```
$ goldberg explain examples/arcfire/assets/kenney/ships.png
kenney/ships.png
  type    texture.color.srgb   conventions: rule "*.png"
  filter  nearest              examples/arcfire/assets/goldberg.berg: rule "kenney/**"
  path    copy                 the only path from texture.color.srgb to texture
  output  build/assets/arcfire/arcfire/kenney/ships.png   lands: texture is a runtime type
  state   clean (cooked from 3fa9...)
```

## What the engine gets

goldberg asks for these, and they belong to the engine:

| Need | Where | Slice |
|---|---|---|
| File watching (inotify, then `ReadDirectoryChangesW`, FSEvents) | `platform` | 1 |
| Content hashing off the main thread | `core` or `platform` | 1 |
| JSON with line numbers for errors, key order kept | `core` (today in kvscript only for writing) | 1 |
| Glob matching | `core` | 1 |
| Child processes (spawn, exit status, stderr) on the I/O queue | `platform` | 1 |
| Reload by path and handle rebind | `asset` | later (Live) |
| Outbound TCP from a game | `platform` (the CLI listens today) | later (Live) |
| Krome | `docs/design/krome.md` | later (The window) |

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| G1 | One event loop; every source and every result is an event on it; replayable from a log | P0 |
| G2 | `cook` and `watch` run the same loop and the same watchers, and differ only at stability | P0 |
| G2a | Declared outputs enter the graph when their process exits; scanned outputs, by declaration, as they settle; watchers sweep up the undeclared and the live edits | P0 |
| G2b | `settle` in a converter's metadata holds a step open for writers goldberg cannot wait on, and admits scanned outputs | P0 |
| G3 | `.berg` files: JSON, `meta` and `rules` in one file, composed across conventions, project, directory and sidecar | P0 |
| G4 | Per-key provenance for effective metadata | P0 |
| G5 | The solver is a pure function and answers with a record; ambiguity is an error, never a pick | P0 |
| G6 | The graph accepts file and rule changes at any time and cascades them back to the loop | P0 |
| G7 | Content hashes decide; an identical write cooks nothing | P0 |
| G8 | Output in `build/assets/<game>/` (dev, debug) or `dist/<game>/assets/` (ship); written atomically; nothing stale left behind | P0 |
| G8a | The output tree is scanned at startup and files with no source are removed | P0 |
| G9 | kormake stops copying assets and calls goldberg; games read the output tree | P0 |
| G10 | Every example cooks with no `.berg` file of its own | P0 |
| G11 | `explain` names the rule behind every decision | P0 |
| G12 | A second cook of an unchanged tree runs no step | P0 |
| G13 | A bad `.berg` file or a failed step fails only what depends on it | P1 |
| G14 | Command-line (shell-script) converters registered in `.berg`, with declared or scanned outputs; `copy` is a converter like them | P0 |
| G14a | Whether a file lands in the output tree is a solver answer, from its type and `land`, and is explained | P0 |
| G14b | Lua converters in process; protocol converters; discovered edges; fan-in | P1 |
| G15 | The game's connection: changed outputs down, loads up, reload in the asset module | P1 |
| G16 | goldberg's window on Krome over the same commands | P2 |
| G17 | Profiles, the lockfile, tag closures and packs | P2 |

## Slices

1. **The loop and the copy.** The module, the loop, the scan, watchers on Linux, `.berg`
   loading and composition, the conventions file, converter registration, typed
   signatures and path search, `copy` and command-line converters, declared and scanned
   outputs, `settle`, the work directory, the terminal rule, the graph with rule nodes
   and removal, saved state, `cook`, `watch`, `explain`, `status`, `clean`. kormake
   hands assets to goldberg; games mount the output tree.
   *Done when* every example builds and runs from a goldberg cook, replays hash as they
   do today, saving a `goldberg.berg` in a watched tree re-cooks exactly what it
   governs, a second cook does nothing, a two-step chain of script converters cooks in
   order and lands only its last output, and the streaming script (see The cook) passes
   in a plain `goldberg cook`.
2. **Real cooks.** Lua converters in process, programs that speak a protocol,
   discovered edges, fan-in.
   Arcfire's `cut_art`, `make_art`, `make_maps` and `make_sounds` become converters and
   `assets/generated/` leaves the repository. JSON data cooks to KBOR when KBOR exists.

**Later slices, proposed.** Not numbered and not ordered yet; each is a candidate for
slice 3 when the time comes.

- **Live.** The game's connection, reload and rebind in the asset module, load reports.
  Save a PNG, see it in the running game. Watchers on Windows and macOS.
- **The window.** goldberg's UI on Krome: inspection, status, reports, forms that write
  `.berg` files.
- **Shipping.** Profiles, the lockfile and re-route diffs, tags, pack closures, budgets;
  kpack becomes a sink of the graph.
- **From the design:** tool monitors, capabilities and write-back, background refinement
  and draft/final state, load telemetry feeding edges, game-supplied converters,
  goldberg on a different host from the target.
- **Small things, noted:** `env` metadata and `.env` files for child processes; a
  `cook` that finds a `watch` already running on the same game and asks it, where today
  the two would both write.

Each slice gets its own PRD (`Goldberg-Slice-N-PRD.md`) before it is built.

## Decided

- **`.berg` is JSON**, and metadata and rules share one schema and one file. This closes
  the design's "declaration format" question for now; Lua tables are not ruled out for
  converters.
- **A sidecar is usually named for the stem**: `ship.berg`, not `ship.png.berg`.
- **`goldberg.berg`** is the directory file, at an asset root or anywhere under it.
- **Assets cook into an output tree**, and nothing else copies them:
  `build/assets/<game>/` for dev and debug, `dist/<game>/assets/` for ship.
- **The output tree is goldberg's.** It is scanned at startup, and what no present
  source accounts for is removed.
- **`shell` is a tool category**, apart from a native binary. On Windows it runs
  `tool.bat` if present, else `tool.sh`.
- **Batch and live are one path.** A scan is a burst of events, and a cook runs the
  watchers too: tools decide for themselves when to write.
- **Process exit is the commit point** for a step's declared outputs. Scanning is
  opted into per converter, with `settle`.
- **`copy` is a converter**, built in and otherwise ordinary.
- **Shell-script converters come first**, before Lua ones: they are the model the
  others are variations of.
- **A writer with no lifetime goldberg can see is a flaw in a static cook**, patched
  with `settle` in the tool's metadata and not modeled further.
- **The game connects to goldberg.**
- **The conventions are a `.berg` file**, so they are read, explained and overridden
  like any other layer.

## Open

- **Per-platform output.** One tree per game and config is enough while every cook is
  platform-neutral. Platform encodes want `<target>` in the path, or a cache with the
  tree as a view of it. The Shipping slice decides.
- **Packs.** goldberg cooks loose, in ship as in dev, until a later slice. How a pack
  is then made from the graph (kpack as a sink, tag closures), and whether the loose
  tree still reaches `dist/` beside it, is open until then. Meanwhile kpack packs the
  cooked tree as it packed the staged one.
- **How a dev build finds its output tree.** A path kormake bakes in, a file beside the
  executable, or an argument. Android and iOS dev builds bundle assets today and need
  an answer of their own until the Live slice gives them the connection.
- **Sources and targets for a game.** Slice 1 uses a roots file and a path file that
  kormake writes. A game wants a better way to say where its assets come from and which
  trees they cook into, including splitting dev from debug in the project's `.berg`.
- **How kormake hands over the roots.** Arguments on the rule's command line, or a
  manifest in the build directory that `goldberg watch` can also read.
- **Terminal, beyond the proposal.** Types marked runtime plus a `land` override is the
  starting rule. Not settled: whether being consumed by a fan-in step should by itself
  stop a runtime-typed file from landing (the atlas case, without anyone writing
  `"land": false`); and whether a file may both land and feed a further step.
- **A stem with two files.** `ship.berg` speaks for `ship.png` and `ship.json` alike,
  narrowed by `match`. If that surprises in practice, `ship.png.berg` is the fallback.
- **Comments in `.berg`.** JSON has none. A `"//"` key, JSON5, or wait for the format
  after JSON.
- **Tag precedence**, still open in the design; slice 1 uses the set rule above.
- **Shaders.** Compiled by a kormake tool rule today. They are assets by any definition;
  whether goldberg takes them, and when.
- **Where Lua ends.** The design says Lua for everything above the primitives. If the
  loop or the graph walk shows up in a profile at tens of thousands of assets, the hot
  part moves to C++ systems over the same pools.
