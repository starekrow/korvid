# goldberg

Korvid's asset orchestrator: it cooks a program's assets into an output tree and keeps
that tree right while sources and rules change.

## Synopsis

```
goldberg cook    [GAME] --roots FILE | --root NAME=DIR... [--out DIR] [options]
goldberg watch   [GAME] --roots FILE | --root NAME=DIR... [--out DIR] [options]
goldberg explain PATH... [--root NAME=DIR]
goldberg learn   FILE [--root NAME=DIR] [--dry-run] [--scan] [--keep]
goldberg clean   --out DIR [--state FILE]
goldberg clean attic --out DIR
goldberg replay  LOG [--dump FILE]
goldberg [--cli-port N] [--threads N] [--verbose] <command> ...
goldberg [--demo] [--shots DIR] watch ... --window
```

## Description

goldberg reads every file under a set of asset roots, decides for each what it is and
what is done with it, and does it: a file the game reads as it is, is copied into the
output tree; a file a converter takes is handed to the converter, and what that makes
is treated the same way in its turn. What a file is comes from `.berg` files, layered
from goldberg's own conventions down to one file's sidecar.

Hashing, copying and converter runs are I/O: tasks on goldberg's own I/O queue, run on
worker threads in three lanes (a converter a core at once, and at least eight; hashes
and copies by the number of cores), each started the moment a worker is free. The main
thread only turns what finished into events for the session, so the converters stay
busy however often it ticks. A converter's outputs are hashed as they are moved into the work tree and arrive
as one event each.

A cook and a watch are one program on one path. Both scan, both run the watchers, both
react to events until nothing is left to do. A cook then exits. A watch says so and
stays. Content hashes decide what changed: a file saved with the same bytes cooks
nothing, and a second cook of an unchanged tree runs no step.

The output tree is goldberg's. It holds what the current sources and rules cook to and
nothing else; what no source accounts for is moved to the attic. goldberg only takes a
directory that is new, empty, or already marked as one of its own (`.goldberg` in it).

Under kormake nobody types most of this: every build runs `goldberg cook --roots FILE`
for each program with assets, and the program finds the tree through
`<target>/bin/<program>.assets`.

## Commands

**cook** scans, cooks until stable, prints one line, and exits. Without `--out` it
solves and reports and writes nothing.

```
$ goldberg cook --roots build/linux-dev/goldberg/arcfire.roots
goldberg: arcfire: 106 files, 0 cooked, 106 clean, 0 failed (4 ms)
```

**watch** does the same and stays up, cooking what changes: a source saved, added or
removed; a `goldberg.berg` or a sidecar saved, added or removed (what it speaks for is
solved again, and only what got a different answer is cooked); the project's
`goldberg.berg`; a converter's script. A rule file that stops parsing is reported with
its line and keeps its last good version in force. While it runs it takes commands on
stdin, and on a TCP port with `--cli-port N` (loopback; one line in, lines out, then
`ok` or `err: why`):

| Command | Answer |
|---|---|
| `status` | stable or working; counts; each step running, each failure, each soft warning |
| `explain PATH` | as below, from the session, with the step's state. `PATH` is a path on disk or a key (`arcfire:ships/ship.png`) |
| `touch PATH` | looks at the file again as if it had been saved |
| `cook` | looks at every file again (every root scanned, every step checked against the output tree, what nothing accounts for taken out) with the whole of each tick until stable, then says `cooked again` |
| `berg FILE` | a rule file's values, each with its path |
| `berg-preview FILE PATH... VALUE` | what setting the value at `PATH` (`rules 1 meta filter`) would change, file by file; nothing saved. A `VALUE` that is not JSON is a string |
| `berg-set FILE PATH... VALUE` | sets it in the file, every other byte as it was; the watch cooks what that changes |
| `quit` | saves the state and exits |
| `ls /ktables/goldberg`, `help`, ... | the engine's own CLI: goldberg's records are pools like any other |

A watch also listens for **programs** that run from its output tree (loopback, a port the OS
picks unless `--listen-port N` says; the port is written to `<out>/.goldberg-port` and removed
at exit). A dev build that links `asset_live` finds it there, says `hello NAME CONFIG`, and is
then told `changed PATH` after each output lands in the tree (renamed into place whole) and
`removed PATH` after one leaves it; it reloads what it holds (`asset_reload`). `status` lists
them, one `program` line each.

**watch --window** is the same watch with a window over it (Krome; `docs/PRDs/Krome-Slice-6-PRD.md`,
"goldberg's window"). The session runs in the frame loop's Work cycle, between frames, and a
still window over a stable session sleeps. Its pages:

- **Status**: stable or working, the counts, every step with its converter, state and output
  (a file a converter made is named by its source), the programs connected, Cook again.
- **Inspect**: any file, found by typing; its answer as `explain` gives it, each value with the
  rule that set it; what made it (and the preludes a screen uses), what it makes, where it lands.
- **Reports**: failures and soft warnings, filtered and sorted; a right press on one inspects it,
  looks at it again, or copies its key.

- **Rules**: a rule file (`.berg`) as a form: its `meta` and each rule's `match` and `meta`, a
  value each, as JSON; a key added to the scope picked; Preview (Ctrl+S) or Revert.
- **Diff**: what the edit would change before it is saved: every file the rule file speaks for
  solved again with the edit in place, each answer that changes (its type, path, lands, output,
  parameters) old beside new. Accept saves the file, its comments, spacing and order kept, and
  the watch cooks what it changes; Reject drops the edit.

A press on a file anywhere inspects it. `status`, `explain` and the rest still answer on stdin
and on `--cli-port`. The window's screens are goldberg's own assets (`assets/ui/window.kim`,
out of `krome/std`), which kormake cooks after goldberg is built. While the window is up a
headless `goldberg watch` on goldberg's own roots runs beside it (unless the window's session
is that watch), and the window reloads them (`asset_live`): saving `window.kim` changes the
open window. Options before the command go to the frame loop (`--screenshot FILE`,
`--size WxH`, `--renderer cpu`); `--demo` drives every page by pointer and keys and checks
each against the session ("goldberg: window demo passed", exit 1 otherwise), and `--shots DIR`
keeps a picture of each:

```
goldberg watch --roots build/linux-dev/goldberg/arcfire.roots --window
goldberg --demo --shots /tmp/shots watch --roots build/linux-dev/goldberg/arcfire.roots --window
```

`tools/goldberg/tests/window/run.sh` runs the demo over a small fixture that has something on
every page (a rule file, a failure, a warning), in a copy of its own, since the demo edits and
saves the rule file. The demo opens a real window that takes the keyboard: keys typed elsewhere
while it runs can land in it.

Without `--window` none of the window's modules (platform_hid, the renderers, Krome, the frame
loop) is started, so a cook runs where there is no display.

**explain** solves files on the spot and says, for each, its type, the metadata a
converter would be given, the path of converters, whether it lands and where, and for
each of those the file and the rule that decided. It finds the asset root by itself:
the nearest `assets` directory above the file that sits beside a `module.lua`.

```
$ goldberg explain examples/arcfire/assets/kenney/ships.png
kenney/ships.png
  type    texture                conventions: rule "*.png"
  path    copy                   texture is already what the game reads
  lands   yes                    texture is a runtime type
  output  <output tree>/arcfire/kenney/ships.png
```

**learn** runs the converter `FILE` is handed to, once, into a directory of its own,
watches what it writes, and sets that converter's `"outputs"` in the `.berg` file that
registers it. Only that value changes; the file's comments, spacing and order stay. See
Converters.

**clean** removes an output tree and its state (only a tree goldberg marked).
**clean attic** empties the tree's attic.

**replay** feeds a log written by `--log` to a fresh session with no file system and no
processes under it, and can dump what it decided. The same log gives the same dump.

## Options

| Option | Meaning |
|---|---|
| `--root NAME=DIR` | an asset root: `DIR`, cooked under `NAME/` in the output tree. Repeatable |
| `--roots FILE` | a roots file, as kormake writes: `{ "game", "out", "state", "project", "roots": [ { "name", "dir" } ] }` |
| `--out DIR` | the output tree |
| `--state FILE` | the saved state. Default: `<out>/../.goldberg-state/<name>.json` |
| `--project DIR` | where the project's `goldberg.berg` is. Default: the nearest directory at or above the current one with a `korvid.config` or `kormake.lua` |
| `--watch MODE` | `auto` (default: the native watcher where there is one), `native`, the native one by its name (`inotify` on Linux, `readdirectorychanges` on Windows), `poll`, or `none` |
| `--lock-wait S` | how long a cook waits for another cook on the same tree. Default 60 |
| `--stamp FILE` | a file rewritten only when the tree changed, for a build system |
| `--share PERCENT` | how much of the main thread the session's own work may take: the events of each tick, then a sleep in proportion. Default 100 for a cook, 40 for a watch (100 while it is asked to `cook`). Hashing, copying and converters are not held back by it: they run on worker threads |
| `--log FILE` | every event, one JSON object a line, for `replay` |
| `--dump FILE` | every file, its answer, every step and everything asked of the host, as text |
| `--quiet` | say nothing when there was nothing to do |
| `-v` | print soft warnings |
| `--listen-port N` | a watch's port for programs (`<out>/.goldberg-port`). Default: one the OS picks |
| `--cli-port N` | before the command: listen for CLI sessions on 127.0.0.1:N |

## .berg files

A `.berg` file is one JSON object. `//` comments are allowed, and a `"//"` member is
ignored wherever it appears.

```json
{
  "meta":  { "tags": ["ships"] },
  "types": { "sheet": { "runtime": false } },
  "converters": {
    "cut": { "from": "sheet", "to": "texture", "shell": "tools/cut", "outputs": ["{stem}_*.png"] }
  },
  "rules": [
    { "match": "*.sheet", "meta": { "type": "sheet" } },
    { "match": "ui/**",   "meta": { "filter": "nearest" } }
  ]
}
```

| File | Speaks for |
|---|---|
| `tools/goldberg/conventions.berg` | everything: what a file is by its name |
| `goldberg.berg` beside `korvid.config` | the project |
| `goldberg.berg` in an asset root or any directory under it | that directory and everything under it |
| `ship.berg` | the files named `ship.*` beside it |

They are applied in that order, outermost first; within a file `meta`, then `rules` in
the order written. Later wins, key by key: objects merge, scalars and lists replace,
`null` removes a key, and `tags` is a set (`"-name"` takes one out). A pattern with no
slash matches a file name at any depth; one with a slash is a path under the file's
directory. `*`, `?`, `[a-z]` and `**`.

Metadata goldberg reads itself:

| Key | Meaning |
|---|---|
| `type` | what the file is: a dotted name, where a prefix is a supertype |
| `cook` | `false`: known, watched and ignored |
| `land` | whether it goes to the output tree, over what its type says |
| `to` | the type it is wanted as, when that is not its type's default |
| `out` | its path under the module in the output tree, when that is not its own |
| `tags` | kept and explained; nothing reads them yet |

Every other key is the converter's. A type is declared with `"runtime": true` (the game
reads it: it lands) or `false` (an ingredient: something must take it somewhere). A file
no rule types is copied as it is, with a soft warning.

## Converters

`copy` is built in. Others are registered in a `.berg` file under an asset root, with a
script (`shell`), a program (`exec`) or one of goldberg's own (`builtin`), one of the three:

| Key | Meaning |
|---|---|
| `from`, `to` | the type it takes and the type of what it makes |
| `shell` | its script, without the extension, relative to the `.berg` file. It runs as `/bin/sh <shell>.sh <args>`; on Windows `<shell>.bat` through `cmd /c` when there is one, else `<shell>.sh` through the `sh` on `PATH` (Git's, MSYS2's) |
| `exec` | a program, run directly as `<exec> <args>` with no shell between: one process a run, which on Windows is half the cost of a `.bat` (a `cmd.exe` less). A path relative to the `.berg` file (`tools/cut`; on Windows `tools/cut.exe` is looked for first) is an input as a script is: a new build of it runs its steps again. It is kept out of the output tree by convention when it ends in `.exe`; give one without an extension a rule (`"cook": false`). A name with no slash is found on `PATH`, and an absolute path is run as it is; neither is hashed, so their steps run again only when the registration or the sources change |
| `builtin` | a converter goldberg runs itself, in process on the run's worker, with no child process and nothing to find on the host: `kim` (`.kim` markup to `.kimb`, below). It takes no `args`; its version is in every step's key, so a new compiler cooks its files again |
| `args` | the arguments, one string split at spaces. Default `{in} {work}` |
| `outputs` | names or patterns of what it writes under `{work}`; or `"scan"`; or left out |
| `settle` | seconds. With `"scan"`, how long a file must be quiet to be taken. Otherwise how long the step is held open after the script ends |

In `args` and `outputs`: `{in}` the source's full path, `{work}` the directory to write
into, `{name}` the source's file name, `{stem}` that name up to its first dot, and any
metadata key (`{filter}`, `{atlas.pad}`). The script also has them as `GOLDBERG_IN`,
`GOLDBERG_WORK`, `GOLDBERG_NAME`, `GOLDBERG_STEM` and `GOLDBERG_META_<KEY>`, and starts
in `{work}`.

- A converter writes into `{work}` and nowhere else, and writes everything it makes on
  every run. What it makes is typed `to` (a rule may narrow that), is matched by rules
  as if it sat beside the source, and is cooked in its turn: landed, converted again, or
  kept.
- With declared outputs, nothing is taken until the script ends well; a failed run
  contributes nothing. A declared pattern nothing matched fails the step. A file nothing
  declared is taken in, with a line.
- With `"outputs": "scan"`, each file is taken once it has been quiet for `settle`, and
  what follows it cooks while the script still runs. If the script fails, what it
  delivered is taken back.
- The script is an input: edit it and every step that runs it is looked at again.
- A non-zero exit fails the step, with the end of what the script wrote to stderr.

**`kim`**, registered in `conventions.berg`: a `*.kim` (type `kim`, an ingredient) is
compiled (`kim_compile`, `modules/kim`) to `<name>.kimb` (type `kimb`, which lands), recording
its asset path (`<root>/<path>`). A mistake fails the step with `file:line:col`. It is then
checked against the preludes it uses (`kimb_check`, `modules/kimb`) when their roots are in
this cook: a `use "krome/std"` is the `std.kim` source under the root named `krome`, as
kormake's cook of a game has it. A misspelled style, a token no theme has, a word that is
neither the kernel's nor a definition fails the cook with its line, as it would fail at
load. A use whose root is not in the cook is not checked against. Every use the check asked for
(whose root is in the cook) is an edge from the screen to that prelude's source, and the
prelude's hash is in the screen's step key: a prelude saved checks again every screen that
uses it (a screen it breaks fails with its line), and the edges are kept in the state, so a
cook after a restart runs no step for them. Only `kim` reports edges so far.

`goldberg learn FILE` writes `outputs` for you. It turns the source's stem into
`{stem}` and names that differ only in a number into one pattern. `--dry-run` shows the
edit; `--scan` declares a streaming converter, with a settle time from the run;
`--keep` leaves what the script wrote. It also says when files were finished far apart,
when an output's name says one type and the converter another, and when anything under
the asset root changed while the script ran.

## Files

| Path | What |
|---|---|
| `<out>/` | the output tree: `<root name>/<path>` for each file that lands |
| `<out>/.goldberg` | the mark that the tree is goldberg's |
| `<out>/.goldberg.attic/<YYYYMMDD-HHMMSS>/` | what a run took out of the tree. Never deleted by a cook; every cook says so while it exists |
| `<state>.json` | per source its size, time and hash; per output what made it. Deleting it is always safe: everything is cooked again. A cook writes it at its end; a watch each time it is stable and something changed, a slice of it a tick |
| `<state>.work/` | what converters made, a directory per step. Scratch |
| `<state>.run/` | where a converter with declared outputs writes until it ends |
| `<state>.lock` | held by the goldberg that has the tree |
| `<out>/.goldberg-port` | a watch's port for programs, while it runs |

Under kormake: the tree is `build/assets/<program>/` (ship: `dist/<program>/assets/`,
or `build/assets-ship/<program>/` for a test or a bundle), and the state is
`build/goldberg/<program>/<dev|ship>/state.json`.

## Exit status

`0` cooked, or nothing to do. `1` a file, a rule file or a step failed; each is named on
stderr. `2` the request was refused: bad arguments, a root that is not there, an output
tree that is not goldberg's, another cook that held the tree past `--lock-wait`.

A cook that finds a watch on its tree does nothing and exits `0`: the watch is already
cooking what changes.

## Environment

`GOLDBERG_LUA`: the directory holding goldberg's Lua (`main.lua`), when it is not
`tools/goldberg/lua` in the source tree the binary was built from.

## Measuring

```
kormake test --heavy goldberg [TARGET] [-- OPTIONS]
build/<target>/bin/goldberg_heavy [OPTIONS]
```

The heavy test (`tests/heavy/main.cc`) is a synthetic cook big enough to act like a real
one, for putting one machine's numbers beside another's. It writes a tree of seeded noise
(1 GiB by default) in size classes, each a file of 1/N of the tree and each holding about
the same share of it: by default one file of a fifth, two of a tenth, three of a
twentieth, 17 of a hundredth, 1,667 of a ten-thousandth and 16,667 of a hundred-thousandth
(about 11 KB). It drops those files from the OS's cache, then has the goldberg beside it
cook the tree through a shell converter that replaces every 32 bytes with their SHA-256
(`--work swap`: swaps every pair of bytes instead), which nothing can skip. Then it checks
the output tree, cooks again with nothing changed, and again with one file per class
rewritten. Last, it starts a watch on the cooked tree, at a watch's own share: it must be
stable having cooked nothing, and a small source saved must reach the output tree.

It prints each phase's time, the cold cook split into before the first converter ran,
while they ran (and how busy goldberg kept its eight), and after the last one, and the CPU
of goldberg against its converters. Then a line per size class from what the converter
recorded of each of its runs: the time per run (mean, p50, p95, max), a run's rate, the
share spent reading and writing, and when in the cook the class's runs started and ended.
The tree, its outputs and the digest at the end are the same on every machine for the same
options; a different digest means a machine did different work.

| Option | Meaning |
|---|---|
| `--size N` | about how big the tree is: `512M`, `2G` |
| `--classes 5,10,...` | the size classes, as divisors of `--size` |
| `--work hash\|swap` | what the converter does to each file |
| `--dir DIR` | where the tree goes. Default `build/<target>/bench/goldberg`, never the OS's temporary directory (memory, on some systems). It needs about three times `--size` free: sources, the converters' outputs in the work tree, and the output tree |
| `--seed N`, `--touch N` | the noise; how many files per class the last cook finds changed |
| `--watch MODE` | goldberg's `--watch` |
| `--no-evict`, `--keep` | leave the cache warm; leave the tree |

Dropping the cache: Linux writes the files back and advises the kernel it does not need
them (and says how much stayed in memory); macOS invalidates a mapping of each; Windows
opens each without buffering. None of it needs privileges, and only Linux and macOS can
say how well it worked. The test fails when a cook fails or an output is wrong, never on a
time.

## See also

`docs/PRDs/Goldberg-Master-PRD.md` for the design, `Goldberg-Slice-1-PRD.md` for what is
built, `docs/design/goldberg-notes.md` for what was measured.
