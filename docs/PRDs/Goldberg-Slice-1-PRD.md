# Goldberg Slice 1 — The loop and the copy

Nov 14, 2026 · @starekrow

## Goal and exit criteria

Slice 1 builds goldberg's whole architecture at its smallest size: the event loop, the
watchers, `.berg` files, the solver, the graph and the cook, with two kinds of converter
(`copy`, and shell scripts) and one job in the build (replacing kormake's asset copy).
The master PRD (`Goldberg-Master-PRD.md`) says what each piece is; this says what is
built now, in what order, and how we know it worked.

The slice is the proof of the architecture. If a cook and a live session are not one
path, if rules cannot change under a running graph, or if a long-running tool does not
fit the loop, this is where it shows, before Lua converters, the game's connection or a
window are built on top.

It is done when:

- `kormake arcfire` cooks arcfire's assets with `goldberg cook` and copies nothing
  itself. Every example and every test executable reads its assets from goldberg's
  output tree.
- The output tree for each game is byte-identical to what kormake staged at the parent
  commit (`diff -r` against the old `bin/assets/`, for all eight asset roots, 183
  files today), with no `.berg` file added to any tree.
- The recorded replays and demo hashes are unchanged (arcfire Shoreline and Orbit,
  arcanum, arcfire's Normal and Hard replays).
- A second `goldberg cook` of an unchanged tree runs no step, and for arcfire takes
  under 100 ms.
- `goldberg explain FILE` names the type, path, output and whether it lands, and the
  file and rule that decided each, for any file under an asset root.
- With `goldberg watch` running: saving a source re-cooks it and nothing else; saving a
  `goldberg.berg` re-solves exactly the files under it and re-cooks only those whose
  answer changed; adding and removing a sidecar does the same for its stem; deleting a
  source removes its output; a `.berg` file that does not parse fails only what it
  speaks for and the last good version stays in force.
- A source deleted while goldberg was not running has its output removed by the next
  cook's scan of the output tree.
- A two-step chain of shell converters cooks in order, lands only the last output, and
  re-cooks when either script is edited.
- The streaming script (create, sleep, write, sleep, write, exit; `"outputs": "scan"`
  with a `settle`) passes in a plain `goldberg cook`: each file admitted once quiet, its
  downstream step run before the script exits, and no exit before the script's.
- The same event log replayed into a fresh goldberg produces the same graph and the
  same list of steps, with no filesystem under it.
- Linux is proved. macOS and Windows build and cook the examples (`copy` only) on the
  polling watcher; their native watchers and Windows process spawning are written when
  those machines are next at hand, and said so in the notes.

## What the code has

- **The copy.** `tools/kormake/scripts/emit.lua` writes one Ninja `copy` edge per file
  of each module's `assets/` to `bin/assets/<module>/<path>`, hidden files left out.
  Eight modules have assets: `text`, `touchpad`, and six examples.
- **The mount.** `modules/platform/src/module.cc` mounts `<base_path>assets` (the
  directory beside the executable) when it exists, then `<exe>.kpak`, then `--pack` and
  `--assets DIR`. Tests read the same directory.
- **The pack.** kpack's rule (`per = "exe"`, `inputs = "assets"`) packs
  `bin/assets/` into `bin/<exe>.kpak`, in ship by default. `package.lua` ships the pack,
  or the loose folders when there is none; `package_apk.lua` and `ios.lua` bundle them.
- **Tools.** `modules/tool` gives a host executable the engine, the CLI and
  `tool_finish`. kpack, picture and kvscript are built on it.
- **Lua.** `modules/lua` is the engine's Lua. The `kv` libraries a script gets
  (`kv.fs`, `kv.json` for writing, `kv.sha256`, images) live in `tools/kvscript/src`
  and are linked into kvscript alone.
- **JSON.** `korvid/core/json.h` reads a text into a flat tree and reports errors by
  byte offset. It keeps member order. It has no line numbers.
- **Files.** `io_list_files`, `io_read_now`, `io_write_now`, the I/O queue.
- **What is missing.** File watching. Child processes. Glob matching. Hashing as a
  job. Any binding of engine pools to Lua.

## The shape

### Where things are

```
tools/goldberg/
  module.lua               host tool; deps core, platform, cli, tool, lua, script
  src/                     the C++ primitives and the commands
  lua/                     the application: loop, berg, solver, graph, cook, explain
  conventions.berg         the built-in layer
  tests/                   unit tests, the event-log replays, the scripts for the shell tests
modules/script/            kvscript's kv libraries, as a library both tools link
modules/platform/          fs_watch.h, process.h
```

### What is C++ and what is Lua

| C++ | Lua |
|---|---|
| The event queue, as an engine pool (`goldberg.event`) | What each event means |
| File, step and rule-file records, as pools (`goldberg.file`, `.step`, `.berg`) | Edges, dirtiness, the cascade, readiness |
| Watching, spawning, hashing, reading and atomic writing | Composition of `.berg` layers and provenance |
| JSON to Lua tables, with a line for every node | The solver and path search |
| Glob matching | `explain`, `status`, the messages |
| `copy` | Converter registration and command lines |

The pools are declared in C++ (the design's decision) and read and written from Lua
through one binding: a kind's rows as userdata with field access by name, built from the
kind's `FieldDesc` table. They appear under `/kinds/goldberg/` in goldberg's own CLI.
Lua holds what a row cannot: lists of edges, effective metadata, provenance.

### The loop, concretely

One system, `goldberg.pump`, each tick:

1. Poll the sources: the watchers, the child processes, finished hash and write jobs,
   the CLI. Each appends event rows.
2. Coalesce: one event per path and kind since the last drain.
3. Hand the batch to Lua in sequence order. Lua solves, changes the graph, and asks for
   work (hash this, run that step, remove that output) by calling back into C++.
4. If the queue is empty, nothing is running, nothing is settling and nothing is dirty:
   `cook` calls `tool_finish`; `watch` prints `stable` once and keeps ticking.

- **`--log FILE`** writes every event as a line of JSON, with content hashes.
  **`--replay FILE`** feeds a log in place of the sources, with the filesystem and the
  processes stubbed: asking for work records the request and nothing runs. The graph
  and the step list are printed and compared. This is the loop's test harness.
- **A tick that waits only on a child or a settle sleeps**, as a tool tick waiting on
  the disk does today.

### The watcher

`korvid/platform/fs_watch.h`: watch a directory tree, poll for events, never block.

- **`inotify`** on Linux: a watch per directory, new directories picked up as they
  appear, renames reported as remove and add, overflow answered by rescanning the tree.
- **`poll`** everywhere: a walk comparing size and mtime against the last walk, at an
  interval (250 ms). It is the fallback on Linux (`--watch poll`) and the only backend
  on macOS and Windows in this slice.
- **What is watched:** each asset root, each step's work directory while it runs, and
  the directories where a project or root `goldberg.berg` could appear. The output tree
  is not watched; it is scanned at startup.

### Processes

`korvid/platform/process.h`: spawn with an argument vector, a working directory and an
environment; poll for exit; read stderr without blocking; kill. `posix_spawn` on Linux
and macOS. Windows (`CreateProcess`) is written with the Windows watcher.

A `shell` converter runs `tool.sh` through `/bin/sh`; on Windows `tool.bat` through
`cmd /c` if it is there, else `tool.sh` through the `sh` found on `PATH`, and a clear
error if there is none.

### Rules, as slice 1 reads them

Everything in the master PRD's "Metadata and rules", including `converters`. Not
`edges` or `profiles`. The metadata keys the slice itself understands:

| Key | Meaning |
|---|---|
| `type` | the asset's type, a dotted name; a prefix is a supertype (`texture.color.srgb` is a `texture`) |
| `cook` | `false`: known and ignored |
| `land` | override whether it lands |
| `to` | the target type, when it is not the conventions' default for `type` |
| `out` | the output path under the module, when it is not the source's |
| `tags` | kept and explained; nothing reads them yet |

**The conventions**, complete for today's trees so that no example draws a fallback
warning:

| Pattern | Type | Runtime |
|---|---|---|
| `*.png` | `texture` | yes |
| `*.material.json` | `material` | yes |
| `*.sheet.json`, `*.anim.json` | `anim` | yes |
| `*.ui.json`, `*.boss.json`, `*.stage.json`, other `*.json` | `data` | yes |
| `*.tmj`, `*.tsj` | `map` | yes |
| `*.xml` | `data.xml` | yes |
| `*.ogg`, `*.wav` | `sound` | yes |
| `*.ttf` | `font` | yes |
| `*.txt`, `*.md` | `text` | yes |
| `*.berg`, hidden files | (none) | `"cook": false` |

Later rules win, so the specific JSON patterns come after `*.json`. Types are declared
in the conventions too, each with `runtime` true or false; a project adds its own the
same way.

### Path search

Types and converters form a small graph. From the file's type, breadth first over
converters whose `from` is the type or a supertype of it, to the first depth at which
the target is reached. One path at that depth is the answer; more is the ambiguity
error. A file whose type already is the target has the path `copy`. With only `copy`
and a project's own shell converters this is tiny, and it is the real algorithm.

### The cook, concretely

- **`copy`** is a C++ function behind the same interface a shell step has: inputs, an
  output directory, a result event. It reads and writes through the I/O queue.
- **A shell step** gets a fresh work directory, `build/goldberg/<game>/work/<step>/`,
  and its command line. Declared outputs are checked at exit: one that is missing fails
  the step, by name. Scanned outputs are admitted as they settle.
- **Step key:** the hashes of the inputs, the script and its registration, the
  parameters substituted into the command line, and for `copy` a version number.
- **Hashing:** SHA-256, on the job pool. A file is hashed when its size or mtime differs
  from the saved state, or when it is new.
- **Writing:** `<name>.tmp-<n>` in the same directory, then rename.

### The output tree and the state

| | Dev and debug | Ship |
|---|---|---|
| Output tree | `build/assets/<game>/<module>/...` | `dist/<game>/assets/<module>/...` |
| State | `build/goldberg/<game>/dev/state.json` | `build/goldberg/<game>/ship/state.json` |
| Work | `build/goldberg/<game>/work/` | the same |

- **State** holds, per source: size, mtime, hash, the solver's answer. Per step: its
  key and its outputs with their hashes. Written atomically when the graph goes stable
  and on `quit`. A missing or unreadable state file means a full cook.
- **The startup scan of the output tree** lists it and, for each file, finds the step
  the current rules would have write it. No such step, or the step's source is gone:
  the file is removed and a line printed. A file that is there and whose step's key
  matches the state is clean.
- **`<game>`** is any executable with assets in its closure: the games, and the test
  executables (`asset_tests`, `text_tests`), which are cooked the same way.

### kormake

- **No more copy edges.** `emit.lua` writes, per executable with assets in its closure,
  one edge that runs `goldberg cook <game> <config> --roots <file>` and always runs.
  Its output is a stamp the executable's default target and its tests depend on.
- **The roots file**, `build/<target>/goldberg/<exe>.roots`, written by `gen`: the game,
  the output tree, and each module's name and `assets/` directory. `goldberg watch`
  reads the same file, so the two cannot disagree. goldberg never reads `module.lua`.
- **goldberg is a host tool** and is built before anything that cooks, as kpack is.
- **Finding the tree.** kormake writes `bin/<exe>.assets`, one line, the output tree's
  path. The platform module mounts that directory where it mounts `<base>assets` today.
  `--assets DIR` still wins.
- **Packs.** Cooks are loose in this slice, in ship too. kpack's rule leaves the default
  ship build and is still buildable by name, over the cooked tree. `package`,
  `package_apk` and `ios.lua` take loose assets from the output tree.
- **`clean`** removes `build/assets/` and `build/goldberg/` with the targets.

### Commands

`cook`, `watch`, `explain`, `status`, `clean`, as the master PRD lists them, plus
`--log`, `--replay`, `--watch inotify|poll|none`, `--roots FILE` and `-v` (print each
step). While `watch` runs the same verbs answer on stdin and on `--cli-port`, with
`touch PATH` to post a `file-changed` by hand.

Output of a cook is quiet: one line per failed or warned asset, and a last line.

```
$ goldberg cook arcfire
goldberg: arcfire: N files, 0 cooked, N clean, 0 failed (41 ms)
```

## Out of scope

Lua converters, protocol converters, `exe` converters, discovered edges, fan-in, and
porting arcfire's asset scripts (slice 2). The game's connection and reload. The window.
Profiles, the lockfile, tags that do anything, packs made from the graph. `env`
metadata and `.env` files. A `cook` that defers to a running `watch`: in this slice the
second one to start on a game waits on a lock file beside the state (Decided, 7). Native
watchers on macOS and Windows; process spawning on Windows. Shaders, which stay a
kormake tool rule.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| S1 | `fs_watch` in `platform`: inotify and poll backends, tests for add, change, remove, rename, new directory, overflow | P0 |
| S2 | `process` in `platform`: spawn, poll, stderr, kill; posix | P0 |
| S3 | `modules/script`: kvscript's `kv` libraries as a library; kvscript unchanged in behavior | P0 |
| S4 | JSON nodes carry a line; an opt-in flag for `//` line comments; JSON to Lua tables with key order and lines kept, `"//"` keys dropped for `.berg` | P0 |
| S5 | Glob matching (`*`, `?`, `**`, character classes) in `core`, with tests | P0 |
| S6 | Engine pools readable and writable from Lua by field name | P0 |
| S7 | The event loop, coalescing, hash-decides, the stability test, `--log` and `--replay` | P0 |
| S8 | `.berg` loading, the five layers, merging, `tags`, per-key provenance, parse errors that keep the last good version | P0 |
| S9 | `conventions.berg` covering every file in the repository's asset roots | P0 |
| S10 | The solver: type, target, path search, terminal, output, as a record | P0 |
| S11 | The graph: rule-file nodes, re-solve on rule change, cascade, removal, collisions | P0 |
| S12 | `copy`, atomic writes, step keys, saved state, the no-op second cook | P0 |
| S13 | The startup scan of the output tree | P0 |
| S14 | Shell converters: registration, substitution, declared outputs at exit, undeclared sweep, script as input | P0 |
| S15 | Scanned outputs and `settle`; withdrawal on failure; the streaming test | P0 |
| S16 | kormake: the cook edge, the roots file, `<exe>.assets`, package from the output tree, no copy edges | P0 |
| S17 | The platform mount from `<exe>.assets` | P0 |
| S18 | `cook`, `watch`, `explain`, `status`, `clean`; the lock | P0 |
| S19 | `tools/goldberg/README.md` as a man page; `docs/design/goldberg-notes.md` with what was measured | P1 |
| S20 | Soft warnings for fallback matches, counted in the last line and listed by `status` | P1 |

## Milestones

Each leaves the tree building and every example running.

1. **Ground** (S1 to S6). The engine pieces, each with its own tests and no goldberg
   yet: the watcher, processes, `modules/script`, JSON lines, glob, pools in Lua.
2. **The loop and the rules** (S7 to S10, S18 in part). The module, the pump, the scan
   and the watchers as sources, `.berg` layers, the conventions, the solver. `explain`
   works; nothing is cooked. The replay harness and its first logs.
3. **The graph and the copy** (S11 to S13, S18). Steps, `copy`, the output tree, state,
   the startup scan, removal, `cook`, `status`, `clean`. Run beside kormake's copy, not
   in place of it: the exit is `diff -r` clean against `bin/assets/` for every root, and
   a no-op second cook.
4. **The handover** (S16, S17). kormake stops copying and calls goldberg; executables
   find the tree; package reads it. Every example runs, the tests pass, the replays
   match. Android and iOS packages are checked in the emulator and the Simulator.
5. **Scripts** (S14, S15). Shell converters, the work directory, declared and scanned
   outputs, `settle`, the terminal rule exercised by a chain. The streaming test.
6. **Live, and close** (S8's live half, S18, S19, S20). `watch` as a session: rule
   edits under a running graph, the bad-file case, the CLI on stdin and TCP, `touch`.
   The README, the notes, the numbers. The master PRD corrected where this slice
   proved it wrong.

## Decided

Reviewed Nov 14; each was a question in the first draft.

1. **Pools from the start.** The graph's records are engine pools bound into Lua (S6),
   not Lua tables. The split between C++ and Lua is what gives inspection (`/kinds`),
   hashing of goldberg's own state, and performance past a few thousand assets, so it
   is not something to add when a profile asks. Four kinds, and the binding kept to
   field access.
2. **The cook edge always runs.** Ninja does not know goldberg's inputs; goldberg's own
   state decides, and the 100 ms no-op is what makes that acceptable. Listing every
   asset as a Ninja input would bring the copy's bookkeeping back and could not see a
   `.berg` file that does not exist yet.
3. **Test executables are games.** Each one with assets in its closure gets its own
   output tree. Tools cook their assets, and engine modules' assets, exactly the way a
   game's are cooked; there is no second kind of asset.
4. **`<exe>.assets`** is how a dev build finds its tree: a file kormake writes, read by
   the platform module. Good enough for now. A game will eventually want a better way
   to name its sources and its targets than a roots file and a path file; that is its
   own piece of work.
5. **No pack in the default ship build**, for now. `kormake package arcfire ship` ships
   loose files where it shipped `arcfire.kpak`.
6. **The poll watcher** is the whole of macOS and Windows for now.
7. **Dev and debug share one output tree**, and a second cook of a game waits on the
   lock, up to a timeout. With nontrivial assets nobody wants a recook for switching
   between dev and debug. Someday a project's `.berg` file can split them (or any
   configs) into trees of their own; and if goldberg is fast and reliable enough, it
   may not matter much either way.

## As built

**Milestone 1, Ground** (Nov 14). S1 to S6, each with tests, on Linux in debug, dev and
ship; `core`, `platform` and `script` also cross-build for Android.

- **Glob** (`korvid/core/glob.h`). `*`, `?`, classes and `**` as a whole segment. A rule's
  pattern with no slash names a file at any depth (`glob_match_path`), so `"*.png"` in a
  `goldberg.berg` means every PNG under it and `"ui/*.json"` means the ones in `ui/`.
- **JSON.** Nodes and errors carry lines. `KV_JSON_LINE_COMMENTS` is the opt-in flag for
  `//` comments; only `.berg` loading will pass it.
- **SHA-256** moved from kvscript to `korvid/core/sha256.h`, with a streaming form, so a
  job can hash without Lua.
- **The watcher** (`korvid/platform/fs_watch.h`). inotify and poll pass the same test.
  A directory moved away is one `DirRemoved`; its files are not reported one by one, on
  either backend. A poll with `now` walks at once: goldberg's "has every watcher
  delivered what it has".
- **Processes** (`korvid/platform/process.h`). fork and exec, since `posix_spawn` cannot
  change directory portably (Android before API 34). The child gets its own process
  group. Windows is a stub that fails with a message.
- **`modules/script`.** kvscript's `kv.fs`, `kv.zip`, `kv.json`, `kv.sha256` and
  `kv.random`, plus `kv.json.decode` (ordered objects, lines, `comments`, `drop`) and
  the kind binding. kvscript keeps `kv.image` and `kv.font`.
- **Kinds in Lua** (`script_push_kind`): `spawn`, `destroy`, `get`, `set`, `row`, `each`.
  Mistakes are Lua errors. Access is the calling system's to declare.
- **Not done here:** the native watchers for macOS and Windows, and Windows processes,
  as the PRD says. Nothing was run on macOS or Windows.

**Milestone 2, The loop and the rules** (Nov 14). S7 to S10 and `explain`; nothing is
cooked yet. `goldberg cook --root NAME=DIR` scans, watches, reads the rule files, solves
every file and prints a line; the 183 files of the eight asset roots solve from the
conventions alone, with no fallback warning.

- **The host** (`tools/goldberg/src/goldberg.cc`) is one system, `goldberg.pump`. Three
  pools: `goldberg.event`, `goldberg.file`, `goldberg.berg`. Sources append to a pending
  list, which is folded (one event per path) and moved into event rows; rows go to Lua
  in sequence order. Commands leave their words for the pump, since the CLI runs them
  in its own system.
- **Hashing** is one batch a tick on the job pool. The tick waits for its batch; the
  loop does not interleave with it. Rule files come back with their text.
- **The application** is Lua in `tools/goldberg/lua/`: `berg` (reading and layering),
  `rules` (the chain for a file), `solve`, `session`, `explain`, `main`.
- **A session reads nothing itself.** Everything it learns is an event and everything it
  wants goes out through `io`. `--log FILE` records the events and where the session
  settled; `goldberg replay LOG` feeds them to a fresh session with an `io` that does
  nothing, and `--dump` of both is byte-identical, with the tree moved away. The test
  for rules changing under a running graph is a log with events added by hand.
- **Solving waits for the rules.** A file is solved at a settle, and there is no settle
  while a rule file is being read, so the first scan solves each file once.
- **One step per file.** A file's answer is `copy` (it lands as it is), `convert` (the
  next converter, found by the path search), `keep` (an ingredient), `ignore` or
  `error`. A chain is each output being solved in its turn, not a list of steps made
  up front; `explain` shows the whole path found.
- **Scope.** Types and converters are in scope wherever the file that declares them
  speaks: the solver sees what the file's chain of rule files declared.
- **The conventions** (`tools/goldberg/conventions.berg`) as the PRD's table, with the
  types declared there.

Different from the PRD, or decided here:

- **The Lua is read from the source tree at start**, found from the host's own
  `__FILE__`, or `GOLDBERG_LUA`. An edit to it needs no build. Embedding it in the
  binary wants a kormake feature (a generated source) and is not done.
- **goldberg's tests run the built binary** (`tools/goldberg/tests`, through
  `Process`). A test executable does not link an executable module's objects, so
  kormake now builds the executable before its tests and reruns them when it changes.
  kormake was bootstrapped again for that; its binary had been behind its scripts.
- **The project's `goldberg.berg` is read once at start**, not watched. Watching one
  file's directory without its tree is for the live milestone.
- **`explain` with no `--root`** finds the asset root itself: the nearest `assets`
  directory above the file that sits beside a `module.lua`, whose `module "name"` line
  it reads for the name. A cook never reads `module.lua`.
- **`status`** has nothing to ask yet: it answers only inside a running session.
- **`io_stat` and `io_current_dir`** joined the platform's file calls.

**Milestone 3, The graph and the copy** (Nov 14). S11 to S13, with `cook` and `clean`.
Run beside kormake's copy, not yet in place of it.

- **The exit:** all eight asset roots cooked with `goldberg cook --root ... --out DIR`
  and compared with `build/linux-dev/bin/assets/` by `diff -r`: identical, 183 files.
  Arcfire's 103 files cook in about 50 ms; the second cook runs no step, reads no source
  and takes about 4 ms.
- **Steps** are rows of `goldberg.step`: an output, its source, the key of what made it.
  A step's key is the hash of the converter, its version, the source's content hash and
  the output's path. It is clean when the saved state has that key for the output and
  the file in the tree has the size and time the state recorded.
- **`copy`** is one batch a tick on the job pool, like hashing: read, hash, write to
  `<name>.tmp-<n>`, rename. Its result is a `cook-done` or `cook-failed` event, so a
  replay has the same graph without copying anything.
- **The output tree is a root that is scanned and never watched.** Its files arrive as
  `file-seen` events. Once every source is hashed and solved, what no step accounts for
  is removed with a line saying why, and the directories that leaves empty go too.
- **State** is one JSON file: per source its size, time and hash; per output its key,
  size, time and source. A source seen with the size and time the state has is not
  read. A missing or unreadable state is a full cook. Without `--state` it is
  `<out>/../.goldberg-state/<name>.json`; kormake will name the PRD's path.
- **Removal:** a source gone, an answer that no longer lands, or an `out` that moved
  takes the old output away. Two sources for one output is an error naming both.

Different from the PRD, or decided here:

- **The output tree carries a marker, `.goldberg`.** goldberg removes what it did not
  cook from its output tree, so it takes only a directory that is new, empty, or
  already marked, and `clean` removes only a marked one. A wrong `--out` cannot cost
  anyone a directory. The marker is a hidden file, so nothing lists or packs it.
- **A file whose rules are broken has no step**, so its old output is removed like any
  other that nothing accounts for. In a live session a rule file that stops parsing
  keeps its last good version, so this bites only when a rule file never parsed.
- **`--out` is optional.** Without it a cook solves, reports and writes nothing, which
  is what milestone 2 did.
- **`io_rename`** joined the platform's file calls.

**The attic** (Nov 14, after milestone 3, asked for in review). A file goldberg takes
out of the output tree is moved to `<out>/.goldberg.attic/<YYYYMMDD-HHMMSS>/<its path>`,
one folder per run, and the directories it leaves empty go. A file about to be written
again is written over, with no trip to the attic. If the move fails the file stays
where it was and the cook says so. A cook warns on its way out, every time, while the
attic exists ("goldberg found files in the attic"); `goldberg clean attic --out DIR`
empties it. The attic is hidden, so the scan of the output tree does not see it and
nothing packs it.

**Milestone 4, The handover** (Nov 14). S16 and S17. kormake copies no asset; goldberg
cooks every program's.

Proved on Linux, in debug, dev and ship, with the old `bin/assets/` directories moved
away so nothing could fall back to them:

- Every example builds and every test passes. arcfire's recorded runs replay and match:
  Normal on the dev build (tick 14,122, 236 hashes), Hard on the ship build reading
  `dist/arcfire/assets/` (tick 15,264, 255 hashes).
- A build with nothing to do runs the cook edges and nothing else, in about 40 ms for
  the whole tree.
- `kormake package arcfire linux-ship` leaves the tree cooked in place and replaces the
  rest of `dist/arcfire/`; `kormake package rocks linux-dev` copies its tree in; the
  packaged `rocks` runs from `/tmp` and loads its assets.
- `ninja bin/arcfire.kpak` packs the cooked tree (106 entries, nothing of goldberg's own).
- `androidemu-dev` builds and `kormake package arcfire androidemu-dev` makes an `.apk`
  with the cooked assets in it.

How it is put together:

- **One cook edge per program** with assets in its closure, executables and tests alike
  (`goldberg cook --roots FILE --stamp FILE --quiet`). It depends on a phony that is
  never a file, so it runs at every build; `restat` and a stamp goldberg rewrites only
  when the tree changed keep that from rerunning anything else.
- **`<target>/goldberg/<program>.roots`**, written by `gen`: the program, its tree, its
  state file, the project directory, and each module's name and `assets/`.
- **`<target>/bin/<program>.assets`**: one line, the tree. `platform_assets_dir` reads it
  (the platform module's first mount, and the two tests that read a file directly);
  with no such file it is `assets/` beside the executable, as before. Each OS gained
  `os_exe_name` for it.
- **Trees.** `build/assets/<program>/` in every config but ship. In ship,
  `dist/<program>/assets/` for a game that packages as a plain directory, and
  `build/assets-ship/<program>/` for a test or a game that packages as a bundle or an
  `.apk`: the PRD's table had no place for those, and a test has no business in `dist/`.
- **`package`** takes the tree's module folders (so not goldberg's marker or attic). It
  ships no pack. A tree cooked in place stays; otherwise `dist/<exe>/` is replaced, as
  it always was, so packaging another target over a ship tree costs the next ship build
  a recook.
- **kpack's rule** takes `%{assets}` (the tree) and names no config.
- **`kormake clean`** with no target also removes `build/assets/`, `build/assets-ship/`
  and `build/goldberg/`.

Written and not run, each waiting for its machine:

- **The Android emulator:** `tools/android/emu_test.sh` now pushes the suite's tree
  (from `<suite>.assets`) module by module into `assets/` on the device. No emulator was
  running here. The `.apk` was built and listed, not installed.
- **macOS and iOS:** `os_exe_name` in `paths_apple.cc`; the `.app` and the iOS package
  take the tree through the same `package.lua` path as Linux; `ios.lua`'s device test
  bundle reads `<test>.assets`. The Simulator's tests run from the build directory and
  should find their tree through the `.assets` file. None of it compiled here.
- **Windows:** `os_exe_name`, `os_rename`, `os_current_dir` and `os_path_stat` in the
  win32 sources, and goldberg itself as a host tool (the polling watcher; the process
  stub). None of it compiled here. A Windows build now needs goldberg to build and
  cook before any game runs.

Not done:

- **The lock** (S18, Decided 7). Two cooks of one program at once are not kept apart.
  Two targets built at the same moment could both write one tree; each file is still
  written whole.
- **The attic in a ship tree.** A tree cooked in place is the shipped directory, so an
  attic in it would ship. `package` says so when it sees one; it does not move it.
- **An old package in the way.** A `dist/<game>/assets/` left by a package made before
  this milestone has no marker, so a ship cook refuses it and says why. Removing
  `dist/<game>/` is the fix. A package made now carries the marker, so a later ship
  cook takes the directory over.

