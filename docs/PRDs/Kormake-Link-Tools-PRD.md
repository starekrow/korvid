# Kormake link tools — PRD: one command name, the right engine's tool

Nov 27, 2026 · @starekrow

## Summary

Today a developer gets the engine's tools onto their path by hand. The root README says to
add `tools/kormake/bin/<platform>` to `PATH`, which covers kormake. The tools kormake
builds (`kpack`, `kvimagetool`, `kbor`, `kimc`, `kvscript`, `goldberg`) sit in
`build/<target>/bin/`, and you reach them by full path or through `kormake run`. This
breaks down as soon as there are two of anything:

- **Two engines.** With two checkouts (`korvid/` and `korvid-B/`), `PATH` points at one
  of them. Running `kormake` in the other checkout uses the wrong scripts, and kormake's
  stale-script warning is the only sign of it. The same goes for every tool: the `kpack` on
  your path may not write the format the engine in front of you reads.
- **A game with its own tools.** A game can build a host tool (an atlas packer, a level
  compiler) as a module of its own, but nothing puts it on the path, and nothing makes
  `kpack` in that game's directory mean the game's fork of the packer, if it has one.
- **Stale binaries.** A tool on the path is whatever was built last. Nothing rebuilds it
  when the engine's sources change.

This PRD adds **linked tools**. `kormake link tools` installs one small program, the
**shim**, and makes a link to it in a directory on your path for every tool the engine
publishes, kormake included. When you type `kpack`, the shim starts in the current
directory and walks up to the `korvid.config` or `kormake.lua` that kormake would find. From
there it learns which engine and which project you are in, and so which `kpack` this
directory's build uses. It brings that binary up to date with Ninja and then runs it. A
project publishes its own tools the same way, and a project tool that has the name of an
engine tool wins it, so in a game's directory `kpack` can mean the game's packer.

The name on your path never points at a tool binary. It is always a forwarding stub, and
which binary it forwards to depends on where you are standing.

## Goals and non-goals

**Goals**

- `kpack`, `kormake` and every other published tool resolve to the engine and project of
  the current directory, with no `PATH` editing beyond one directory added once.
- What you run is what the build runs: the binary in this directory's build, rebuilt if
  its sources changed.
- A project publishes host tools of its own with one field in `module.lua`, and can take
  over an engine tool's name.
- One shim serves every engine checkout and every engine version installed on the
  machine. Installing it from an older engine never breaks a newer one.
- Outside any Korvid tree, a linked name stays out of the way: it runs the tool you chose as
  its default (`kormake link default`), or else the next program of that name on `PATH`.
- Linux, macOS and Windows.

**Non-goals**

- Installing tool binaries. Nothing is copied out of a build: links lead to the shim, and
  the shim leads to the build.
- A package manager, fetching engines, or picking an engine version by number. The engine
  is the one `korvid.config` names, as it is for kormake.
- Shell completion, shell functions, or changing shell startup files.
- Running tools for a target other than the host. Linked tools are host tools.

## What the user sees

```
$ ~/code/korvid/tools/kormake/bin/linux/kormake link tools
kormake: shim 1 installed in ~/.local/share/korvid-link/
kormake: linked kormake goldberg kbor kimc kpack kvimagetool kvscript in ~/.local/bin
$ cd ~/code/mygame/levels
$ kpack list ../build/linux-dev/bin/mygame.kpak    # the engine mygame's korvid.config names
$ cd ~/code/korvid-B/examples/arcfire
$ kormake dev                                      # korvid-B's kormake, not korvid's
$ kormake link which kpack
kpack: ~/code/korvid-B/build/linux-dev/bin/kpack (engine ~/code/korvid-B, host-dev)
```

From inside a game that publishes a tool:

```
$ cd ~/code/mygame && kormake link tools
kormake: linked mkatlas in ~/.local/bin (published by mygame)
$ mkatlas sprites/ --out build/atlas
```

## Pieces

| piece | what it is | where it lives |
| --- | --- | --- |
| the shim | a small C program, `kvbinlink`, with no Lua and no knowledge of module files | the link home |
| links | one per published name, each leading to the shim | the bin directory |
| the tool table | per anchor: its engine, its kormake, and each published name's binary and how to build it | the cache |
| the registry | the engines and projects that `link tools` was run from, and the names it linked | the link home |

The shim is deliberately simple. Everything that depends on the engine's version (reading
`korvid.config`, collecting modules, deciding which build a tool comes from) stays in the
engine's own kormake. kormake writes the answer down as the tool table, and the shim reads
it. So the shim's only contracts are the walk, the table's format and how to run a
program. Those change rarely, and when they do the change is versioned (see Versions).

### Locations

Three places, chosen per platform, each with an override:

| | bin directory | link home | cache |
| --- | --- | --- | --- |
| Linux | `~/.local/bin` | `$XDG_DATA_HOME/korvid-link/` (`~/.local/share/korvid-link/`) | `$XDG_CACHE_HOME/korvid-link/` (`~/.cache/korvid-link/`) |
| macOS | `~/.local/bin` | `~/Library/Application Support/korvid-link/` | `~/Library/Caches/korvid-link/` |
| Windows | `%LOCALAPPDATA%\korvid-link\bin\` | `%LOCALAPPDATA%\korvid-link\` | `%LOCALAPPDATA%\korvid-link\cache\` |
| `--system` (Linux, macOS) | `/usr/local/bin` | `/usr/local/lib/korvid-link/` | per user, as above |
| override | `--bin DIR`, `KORVID_LINK_BIN` | `KORVID_LINK_HOME` | `KORVID_LINK_CACHE` |

- The name is `korvid-link`, not `korvid/`. A game's saves live in `<data>/korvid/<game>/`
  (`platform_user_dir`, with org `korvid`), so no game's name can collide with the link
  home.
- The cache is always per user, even with `--system`. It is written on every `kormake gen`,
  so it has to be writable by the user who builds. Deleting it costs one slow first run per
  anchor and nothing else.
- `~/.local/bin` is on `PATH` by default in most Linux distributions. On macOS it usually is
  not. After linking, kormake checks `PATH` and, if the bin directory is not on it, prints
  the one line to add, for the user's shell. It never edits a startup file.
- On Windows, `--path` adds the bin directory to the user's `PATH` (the `HKCU\Environment`
  key, then a settings-change broadcast), and only with that flag. Without it, kormake
  prints the command that would do it.

### Links

- **Linux and macOS**: a symbolic link, `~/.local/bin/kpack` to `<home>/kvbinlink`. The shim
  takes the tool's name from the base name of `argv[0]`.
- **Windows**: symbolic links need developer mode, and `.bat` and `.cmd` wrappers mangle
  arguments and turn Ctrl-C into "Terminate batch job?". So each name is a hard link to
  `kvbinlink.exe` named `kpack.exe`. Where a hard link cannot be made (a different volume),
  it is a copy. The shim takes the name from its own module file name.
- `kvbinlink TOOL [ARGS]` is the same as running the link named TOOL, for scripts and for
  debugging.
- `link tools` never overwrites a file in the bin directory that is not one of its links
  (a symbolic link to the shim, or on Windows a file that the registry records). It names
  the file and skips it, and `--force` replaces it.

### The shim

When it is run as NAME with ARGS, the shim does the following:

1. **Find the anchor.** It starts in the current directory and walks up, looking at each
   level for `korvid.config` and then `kormake.lua`, the same walk kormake does. The first
   file it finds marks the anchor. `KORVID_LINK_DIR` starts the walk somewhere else. For
   `kormake` alone, a `-C DIR` among the arguments starts the walk at DIR, so
   `kormake -C ~/code/mygame` from anywhere finds mygame's engine.
2. **No anchor: the default, or get out of the way.** If the registry holds a default for
   NAME (see `link default`), the shim goes on from step 3 with the default's anchor in
   place of the one it did not find. Otherwise it searches `PATH` for another program named
   NAME, skipping every entry that resolves to the shim itself. If it finds one, it runs that
   program with ARGS, unchanged. If not, it exits 127 with
   `kpack: not inside a Korvid engine or project (no korvid.config or kormake.lua above /home/me/notes)`.
3. **Read the tool table** for the anchor's real path from the cache, and check that it is
   current: every file it records (the anchor's config, the master file) has the same size
   and modification time, and the kormake it names exists. If the table is missing, out of
   date, or has no row for NAME, the shim asks kormake to write it (see Resolving) and reads
   it again, once.
4. **Check trust** (see Trust).
5. **Bring the tool up to date.** It runs
   `ninja --quiet -C <build> <target>` from the table, with Ninja's standard output sent to
   the shim's standard error, so a tool piped into another program never has build output
   in its stream. The run holds the build directory's lock (see Locking). A build that
   fails ends the run with Ninja's exit status and
   `kpack: rebuilding failed (KORVID_LINK_BUILD=never runs the binary as it is)`. If
   `build.ninja` was regenerated during the run, the table was rewritten too, and the shim
   reads its row again.
6. **Run it.** On Linux and macOS this is `execv`: the tool replaces the shim, so signals,
   standard input and output, the terminal and the exit status are the tool's own. On Windows,
   which has no `exec`, the shim starts the tool with `CreateProcess` in a job object with
   kill-on-close, passes Ctrl-C through, waits, and exits with the tool's exit code. `argv[0]`
   is the binary's real path, so a tool that finds files beside itself still finds them,
   and `KORVID_LINK_NAME` holds the name it was called by. The current directory is not
   changed.

`KORVID_LINK_DEBUG=1` prints each step to standard error during a real run: the anchor, the
table used, why it was rewritten, the Ninja command, and the binary.

### Diagnosing a lookup

When the first argument is `--diagnose-tool-lookup`, the shim runs nothing. It does steps 1
to 4, then prints a report to standard output and exits 0, or 127 if no candidate would run.
The report covers:

- the current working directory, and where the walk started if `KORVID_LINK_DIR` or `-C`
  moved it;
- each directory the walk looked in and what it found there, ending at the anchor, or
  "no anchor";
- the engine context: the anchor and its kind (engine root, in-tree config or project), the
  engine root, the project if there is one, the kormake that wrote the table, the table's
  path, and whether the table was current, rewritten (and why), or missing;
- trust: whether the engine and the project are in the registry;
- every candidate executable for the name, in the order they are considered: the project's
  module, the engine's module, then each other program of that name on `PATH`. Each comes
  with its path, where it came from (`project mygame`, `engine ~/code/korvid`, `PATH entry 3`),
  whether it exists, and, for the built ones, whether a rebuild is due (a dry Ninja run,
  `ninja -n`, which builds nothing);
- the chosen candidate, and why it won and the others lost. For example: "project module
  kpack publishes this name and wins over the engine's", "not inside a Korvid tree: first
  other kpack on PATH", "project ~/src/stranger is not linked, so its tools are skipped".

```
$ kpack --diagnose-tool-lookup
cwd        /home/me/code/mygame/levels
walk       /home/me/code/mygame/levels: nothing
           /home/me/code/mygame: korvid.config  <- anchor
context    project mygame (/home/me/code/mygame), engine /home/me/code/korvid
table      ~/.cache/korvid-link/roots/3f9c0a1b7d22e4c5.txt (current)
kormake    /home/me/code/korvid/tools/kormake/bin/linux/kormake
trust      engine linked, project linked
candidates
  1 project  /home/me/code/mygame/build/linux-dev/bin/kpack   built, up to date
  2 engine   /home/me/code/mygame/build/linux-dev/bin/kpack   (module kpack of the engine: shadowed)
  3 PATH     /usr/bin/kpack                                    exists
chosen     1: project module kpack publishes "kpack", and a project's tool wins over the
           engine's; PATH programs are used only outside a linked tree
```

The flag is the shim's own, and it is read only as the first argument, so a tool can still
take `--diagnose-tool-lookup` anywhere later in its arguments. `kormake link which NAME`
prints the same report through the shim (`kvbinlink NAME --diagnose-tool-lookup`), so there is
one implementation of the report.

For the shim to name the candidates that lost, the table holds every candidate for each name,
not just the winner (see Format).

The shim's own exit statuses: 127 when NAME is not published here or there is no anchor and
no fallback, 126 when the binary exists but cannot be run, and 1 for any other error of its
own, printed as `<name>: <message>`. In every other case it exits with the status of Ninja
(if the rebuild failed) or of the tool.

### Rebuilding

Rebuilding before every run is the point: the binary you run is the binary that matches the
engine's sources. On a built tree it costs a Ninja run that does nothing, which is Ninja
loading `build.ninja` and checking the modification times of the tool's closure. That cost
is measured as part of the work (R14). `KORVID_LINK_BUILD` changes the policy:

- `always` (the default): run Ninja first.
- `never`: run the binary as it is, or fail with exit status 126 if it has not been built.
  This is for tight loops and for read-only trees.

No tool is ever built from inside a build: kormake's rules run a tool by its path in the
build directory (`g.tool_path`), never through a link, so linking changes nothing in any
Ninja graph.

### Locking

Ninja takes no lock on its build directory, so two Ninja runs in one directory can corrupt
`.ninja_log` and `.ninja_deps`. A `make -j8` that calls `kpack` eight times would start
eight such runs. The shim therefore holds an exclusive lock on `<build>/.korvid.lock` while
Ninja runs (`flock` on Linux and macOS, `LockFileEx` on Windows) and releases it before
starting the tool, so many copies of a tool can still run at once. kormake takes the same
lock around its own Ninja runs (R11), so a shim and `kormake build` in another terminal take
turns instead of racing.

A build can run a linked tool itself: a cook script that calls `kpack` by name, for example.
That shim would wait forever for the lock its own build holds. So while kormake runs Ninja it
sets `KORVID_LINK_HELD` to the build directory. A shim that finds its tool's build directory
there skips the Ninja step and runs the binary as it is, since the build around it is already
building that binary.

## Publishing

### What a module publishes

A host executable module publishes a command with one field in `module.lua`:

```lua
module "kpack" {
  output    = "exe",
  platforms = { "host" },
  publish   = true,             -- the command is the executable's name: kpack
  ...
}
```

- `publish = true` publishes the executable under its own name (its `exe` field, otherwise
  the module's name). `publish = "kvimage"` publishes it under another name, and a list
  publishes it under several.
- Only an `exe` that is built for the host can publish. Anything else is refused when the
  modules are collected, with the reason.
- Two modules of one root that publish the same name are an error, and so is publishing the
  name `kormake` or a name that is not a plain file name (letters, digits, `-`, `_`, `.`,
  starting with a letter).

The engine's master file gains nothing for this: each tool says it is published where the
tool is defined. In this repository, `kpack`, `kvimagetool`, `kbor`, `kimc`, `kvscript` and
`goldberg` set `publish = true`.

**kormake** is always published by an engine root. It is not a module. Its row in the
table is the engine's own `tools/kormake/bin/<host>/kormake`, with no build step, since
bootstrapping is how it gets built. If that binary is missing, the shim says to run
`tools/kormake/bootstrap.sh` (or `.bat`) in that engine.

### What a project publishes

A project publishes a tool the same way, in its own module's `module.lua`. A project module
that publishes a name an engine tool also publishes wins it, in that project's directories
only. This matches the rule from the last change, where a project module with an engine
module's name wins over it. A game keeps its fork of the packer as a module named `kpack`
with `publish = true`, and then `kpack` inside the game is the fork, while elsewhere it is
the engine's.

**Scripts as commands (P1).** Many game tools are Lua scripts for `kvscript`. A
`korvid.config` publishes one without making it a module:

```lua
command "mkatlas" { script = "tools/mkatlas.lua" }   -- path relative to this file
```

Its row runs the root's `kvscript` with `run <script> ARGS`, exactly as `kormake run` does,
from the current directory. The build step is kvscript's. In the table it is a `tool` row
for kvscript's binary, with `config=` (the file that published it) and `script=` (the
script's absolute path) added.

### Which binary a name means

kormake decides this, at `gen` time, for one anchor. The shim never does:

1. The anchor's project, if there is one: a name a project module publishes, built in the
   project's build base, `<build>/<host>-<host_config>/bin/<exe>`.
2. Otherwise the engine: a name an engine module publishes, built in **this anchor's**
   build base. For a project, that means the engine's tool is built in the project's own
   `build/`, not in the engine's tree.

The second point follows from 1.1's rule that a project writes nothing into the engine's
tree, and it is what lets an engine live in a read-only place (`/opt/korvid`). It also
means the `goldberg` you run in a game's directory is the very one that cooks the game's
assets. The cost is that each project builds the tools it runs once, in its own `build/`.

That requires one change to private module paths. Today a project sees none of the
engine's private modules except the cook's program. Under this PRD, **a private module that
publishes a command is visible to a project as a tool**: it can be named (`kormake kpack`
builds it into the project's build directory), and it gets a row in the table. No project
module may depend on it, as now. The cook's program stays the one private module that a
project's graph uses.

The config is the root's `host_config` (dev in this repository), so a linked tool is the
same binary the build runs, and the same objects are shared with it.

## The tool table and resolving

### Format

The table is one plain text file per anchor, `<cache>/roots/<hash>.txt`, where `<hash>` is
the 64-bit FNV-1a hash of the anchor's real path, as 16 hex digits. The hash only spreads the
files out: the table repeats the anchor's path, and the shim checks it. kormake writes the
table on every `gen` of the host target in `host_config`, including the regenerate rule's,
to a temporary file that is then renamed over the table, so a shim never reads half of one.
A cache that cannot be written is a warning, and the build goes on.

Each line is a keyword and its fields, separated by tabs, so paths may hold spaces. A
`tool` or `shadowed` row is the name, then `key=value` fields:

```
korvid-link 1
anchor      /home/me/code/mygame
engine      /home/me/code/korvid
project     /home/me/code/mygame
kormake     /home/me/code/korvid/tools/kormake/bin/linux/kormake
ninja       ninja
watch       /home/me/code/mygame/korvid.config    412    1795826653
watch       /home/me/code/korvid/kormake.lua      5910   1795826600
tool        kormake  from=engine   exe=/home/me/code/korvid/tools/kormake/bin/linux/kormake
tool        kpack    from=project  module=mypack  build=/home/me/code/mygame/build/linux-dev  target=mypack  exe=/home/me/code/mygame/build/linux-dev/bin/mypack
shadowed    kpack    from=engine   module=kpack   by=project:mypack  build=/home/me/code/mygame/build/linux-dev  target=kpack  exe=/home/me/code/mygame/build/linux-dev/bin/kpack
tool        kbor     from=engine   module=kbor    build=/home/me/code/mygame/build/linux-dev  target=kbor  exe=/home/me/code/mygame/build/linux-dev/bin/kbor
tool        kimc     from=engine   module=kimc    unavailable=<why it cannot build here>
```

`ninja` is the toolchain probe's Ninja when it found one, otherwise `ninja`, which the shim
looks up on `PATH`. A `watch` line is a file, its size and its modification time in seconds.
A candidate that lost is written as a `shadowed` row after its name's `tool` row, with
`by=` naming the side and module that won. When the losing module is still in the graph
(another module published its command), the row says where it is built. When it lost its
module name to a project module, it is not built here at all, and the row has no build
fields. The shim never runs a shadowed row. It reads those rows only for
`--diagnose-tool-lookup`.

The first line is the format's major version. A shim skips any key or field it does not
know, so later engines can add some. A table whose major version is higher than the shim
reads counts as missing, and the shim's message says to run `kormake link tools` from that
engine, which installs a newer shim.

Each project or in-tree directory under the engine (`examples/`, `tools/`) is an anchor of
its own, so each gets its own table. The in-tree ones say the same thing as the root's,
since they share its build.

### Resolving

When the table is missing or out of date, a kormake has to read the anchor's config. The
shim cannot, because the config is Lua. Which kormake it asks:

1. If the anchor has `kormake.lua`, the anchor is an engine root, so the shim runs that
   engine's `tools/kormake/bin/<host>/kormake _link write <anchor>`.
2. Otherwise the anchor has a `korvid.config`, and its engine is not yet known. The shim runs
   any registered engine's kormake (the one most recently linked) with
   `_link engine <anchor>`. This reads the config **leniently**: only `engine` is
   evaluated, unknown directives are ignored, and `requires_kormake` is not enforced, so an
   older kormake can read a newer engine's config. It prints the engine root, and the shim
   then runs that engine's kormake with `_link write`, as in 1.

`_link write` does what `gen` does for the host target in `host_config`: it collects,
resolves and emits, so `build.ninja` exists too. Then it writes the table. If there is no
registered engine yet, or the engine's kormake is not bootstrapped, the shim says which
command to run.

## Trust

A link with a generic name changes what typing that name does in any directory. Without a
check, cloning an unknown repository, `cd`-ing into it and typing `kpack` would build and
run that repository's code, or the code of whatever engine its `korvid.config` names. Typing
`make` in a cloned repository has the same risk, but there you knew you were running the
repository's build. So the shim runs only what you have vouched for:

- In an engine's own tree, a tool runs only when that engine's real path is in the registry,
  which is to say you ran `kormake link tools` from it once.
- In a project, every tool, the engine's included, runs only when the project is in the
  registry too. The engine's tools are built in the project's graph there, where a project
  module that takes an engine module's name (`core`, say) is compiled into them. So running
  even the engine's `kpack` in a project means running the project's code.
- `kormake` itself needs only its engine to be linked. It is the engine's own binary, and
  running it in a project is asking for that project's build.
- Anything else is treated like no anchor (shim step 2): the next program on `PATH` if there
  is one. If there is none, the shim exits 127 with
  `kpack: /home/me/src/stranger is not linked (run kormake link tools there to use its tools)`.

`kormake link tools` is the only thing that adds to the registry. `gen` never does. The
registry is `<home>/registry.txt`, one entry a line, tab-separated: `engine PATH`,
`project PATH` (most recently linked last), `link NAME BINDIR`, `default NAME ANCHOR`, and
`noted NAME ANCHOR` for a name `gen` has already said is not linked.
The shim's home is the directory the shim itself is in (`KORVID_LINK_HOME` overrides it),
so a `--system` shim reads the system's registry. The
lenient `_link engine` step evaluates a `korvid.config` from a directory you have not
vouched for, but it runs in kormake's sandbox, which has no I/O and no `exec`, and it
reads the config only to find the engine path before the trust check.

## Commands

```
kormake link tools  [NAME...] [--bin DIR] [--system] [--path] [--force]
kormake link list
kormake link which  NAME...
kormake link default [NAME...] [--clear]
kormake link remove [NAME...] [--forget]
```

**link tools**
: Installs or updates the shim in the link home (see Versions), registers this engine and,
  when run inside a project, the project, and makes a link in the bin directory for every
  name the anchor's table holds, or only the NAMEs given. It writes the anchor's table. Links
  that already exist are left as they are. It prints what it linked, what it skipped and why,
  and the `PATH` line if the bin directory is not on `PATH`. Running it again is harmless.

**link list**
: Prints the registry: the shim's version and place, each linked name, and each
  registered engine and project, marking any whose directory is gone.

**link which**
: For each NAME, prints the shim's `--diagnose-tool-lookup` report from the current
  directory (see Diagnosing a lookup). It writes the table if needed, but builds and runs
  nothing.

**link default**
: Makes the current context's tool each NAME's out-of-tree default (every name the context
  publishes, with no NAME). From then on, typing NAME where no `korvid.config` or
  `kormake.lua` is above you runs the tool from this context: this anchor's engine or
  project, with the same table, rebuild and run as inside it. A NAME this context does not
  publish is an error, and nothing is changed: `kormake: link default: kimc is not published
  here (engine ~/code/korvid publishes kbor, kpack, ...)`. The default is stored as the
  anchor and the name, not as a binary, so it follows the anchor's sources. Running this in
  a context that has not been linked links it first, as `link tools` would, since a default
  is a choice to trust it. `--clear` removes the NAMEs' defaults (all of them with no NAME),
  so they fall through to `PATH` again. `link list` shows each default, and
  `--diagnose-tool-lookup` outside a tree names the default it chose.

**link remove**
: Removes the links for the NAMEs (all of them with no NAME) and their registry entries.
  With `--forget`, it also removes the current engine or project from the registry, and every
  default that pointed at it. The
  shim and the link home go when no link is left. The cache is left alone, since it is
  only a cache.

`gen` warns once, with the command to run, when the root publishes a name that has no link,
and only once you use linked tools there (its engine, and its project, are linked):
`kormake: mkatlas is published but not linked (kormake link tools)`. It never makes links
itself, since writing into a directory on your path should only happen when you ask for it.

## Versions

The shim has a version, `<protocol>.<revision>` (`kvbinlink --version`): 1.0 in this PRD.
The protocol is a single integer. It covers the walk, the
table's major version, the environment variables and the link layout. Each engine
bootstraps its own copy of `kvbinlink` beside its kormake (`tools/kormake/bin/<host>/kvbinlink`),
and `link tools` copies it into the link home, recording its version beside it
(`kvbinlink.version`). It never replaces a newer shim. It replaces one of the same version
only when the two are different builds, which happens with a fix within a revision. So linking from an old checkout never downgrades the shim
a newer engine needs. Within a version, any engine's shim does the job for every engine.

On Windows a running `.exe` cannot be replaced. `link tools` renames the old shim and its
hard links aside (`kvbinlink.exe.old`), puts the new one in place, and deletes the old files
the next time it runs.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `kvbinlink`, the shim: C beside kormake's host (sharing `os.h`), no Lua, built by both bootstrap scripts | P0 |
| R2 | The shim's walk (same order as kormake's), `KORVID_LINK_DIR`, `-C` for kormake only | P0 |
| R3 | Fall-through to the next program of that name on `PATH` when there is no anchor or no trust, skipping the shim's own copies | P0 |
| R4 | `publish` in `module.lua` (true, a name, a list), checked when modules are collected; kormake always published by an engine root | P0 |
| R5 | The tool table: format 1, written on every `gen` by temporary file and rename, `watch` entries, rows by the rule in "Which binary a name means" | P0 |
| R6 | A private module that publishes is visible to a project as a tool, built in the project's build base; still not a dependency | P0 |
| R7 | Resolving: `_link write`, and `_link engine` with lenient config evaluation | P0 |
| R8 | Rebuilding through Ninja before each run, Ninja's output on standard error, `KORVID_LINK_BUILD` | P0 |
| R9 | Running the tool: `execv` on Linux and macOS; `CreateProcess`, job object, Ctrl-C and exit code on Windows; `argv[0]` the real path; `KORVID_LINK_NAME` | P0 |
| R10 | `kormake link tools`, `list`, `which` and `remove`, with the locations, overrides and `--system` as tabled | P0 |
| R11 | The build directory's lock, taken by the shim and by kormake around every Ninja run | P0 |
| R12 | Trust: the registry of engines and projects; untrusted anchors fall through | P0 |
| R13 | Shim protocol version; no downgrade; Windows replace-aside | P0 |
| R21 | `--diagnose-tool-lookup` as the first argument: cwd, walk, engine context, table, trust, every candidate with its state, the chosen one and why; `shadowed` rows in the table; `link which` prints the same report | P0 |
| R14 | The cost of a linked tool's no-op run measured on this repository (median of 20, hot cache) and recorded in Status; above 150 ms, `KORVID_LINK_BUILD=never` becomes the documented route for loops | P1 |
| R15 | `command "NAME" { script = ... }` in `korvid.config` | P1 |
| R16 | The unlinked-name warning in `gen`; the `PATH` check and hint after linking | P1 |
| R17 | `--path` on Windows | P1 |
| R18 | `KORVID_LINK_DEBUG` | P1 |
| R19 | `KORMAKE_VERSION` 1.3; the tools of this repository set `publish = true` | P1 |
| R22 | `kormake link default [NAME...] [--clear]`: out-of-tree defaults stored as anchor and name, used by the shim's step 2, shown by `list` and the diagnosis | P1 |
| R23 | `picture` renamed `kvimagetool`: module, directory, executable, and every script and document that runs it | P0 |
| R20 | `KORVID_ENGINE` and `KORVID_PROJECT` set for the tool, so a script knows where it was run from | P2 |

## Documentation

- `tools/kormake/README.md`: `link` under COMMANDS; `publish` under `module.lua`;
  `command` under `korvid.config`; the link home, bin directory, cache and the table under
  FILES; every `KORVID_LINK_*` variable under ENVIRONMENT; the shim's exit statuses under
  EXIT STATUS.
- `tools/kormake/BUILDING.md`: how the shim resolves (the two-hop `_link` step), the lock,
  and diagnosing with `KORVID_LINK_DEBUG` and `kormake link which`.
- `docs/guides/introduction.md`: the setup step becomes running `link tools` once, in place
  of putting kormake on your path.
- The root README is the author's. Its line "Add `tools/kormake/bin/<platform>` to your
  PATH" is a platform build instruction, so it may change to
  `tools/kormake/bin/<platform>/kormake link tools`, but only once the author agrees.

## Milestones

0. **Rename.** `picture` to `kvimagetool`, on its own, before anything else.
1. **Publish and the table.** `publish`, its checks, private tools seen by projects, the
   table written by `gen`, `_link write` and `_link engine`. Verified by reading tables:
   no shim yet.
2. **The shim.** `kvbinlink` on Linux and macOS: the walk, the table, fall-through, the Ninja
   step with the lock, `execv`, `--diagnose-tool-lookup`. `kormake link tools`, `list`, `which`, `remove`, the
   registry and trust. kormake takes the lock.
3. **Windows.** Hard links, `CreateProcess` and the job object, Ctrl-C, replace-aside,
   `--path`.
4. **Projects, scripts and defaults.** A project's own tools, a project tool winning an engine
   tool's name, `command` scripts, the unlinked warning, `link default`.
5. **Manual and measurement.** The documentation above, R14's numbers, the version bump.

**Verification.** `tools/kormake/tests/run.sh` gains a link half. It works in a temporary
`HOME` with every `KORVID_LINK_*` location pointed into it, so it never touches the real
bin directory, and checks:

- linking from the fixture engine, then `which` and a run from the engine root, from
  in-tree and from the fixture project;
- a project tool winning an engine name in the project and not outside it;
- a source change rebuilt before the run, and `KORVID_LINK_BUILD=never` not rebuilding;
- a stale table rewritten after the config is edited;
- fall-through to a stub of the same name later on `PATH`, and exit 127 when there is none;
- an unregistered project refused;
- `link default` outside a tree running the chosen context's tool, refusing a name the
  context does not publish, and `--clear` falling through to `PATH` again;
- exit statuses and standard input passed through, and Ninja's output kept off standard
  output (`kpack list x | wc -l`);
- eight concurrent runs on a cold build that do not corrupt `.ninja_log`;
- a newer shim not replaced by an older one;
- `--diagnose-tool-lookup` reports, compared with expected files (paths written as
  `{home}`, `{root}` and `{project}`), from the engine, the project, an untrusted project
  and outside any tree. Each report names the shadowed engine candidate where there is one,
  and none of them builds or runs anything, which the test checks with Ninja's log and a
  stub tool that would write a file.

On this repository: two checkouts side by side, each `kormake` going to its own engine's
binary and each `kpack` to its own build. Windows and macOS run the same half.

## Decisions

Settled Nov 27, 2026, before the work began:

- **Generic names.** `picture` was a poor name for a tool, so the module, its directory and
  its executable are renamed `kvimagetool` (`tools/kvimagetool/`). Nothing publishes
  `picture`.
- **Tools outside every tree.** Off by default: outside a tree, a linked name runs the next
  program of that name on `PATH`. `kormake link default` is the opt-in, one name at a time
  or every name a context publishes.
- **Building engine tools in each project.** Kept as designed: each project builds the
  engine's tools it runs in its own `build/`. One central build of the tools that every
  project shared would be more surprising than one more build of kpack.

## Status

Built Nov 27, 2026 on branch `toolpaths`, milestones 0, 1, 2, 4 and 5, on Linux. Kormake is
1.3, and the master file requires it (module files use `publish`).

- **Not done: milestone 3, Windows.** There was no Windows compiler on the machine the work
  was done on, so none of it was compiled. `os_win32.c`'s new functions (hard links,
  `LockFileEx`, `SetEnvironmentVariableW`) are written but not built. In the shim,
  `run_child`, `run_instead` and `lock_build` are stubs on Windows, marked "milestone 3".
  Still to do: `CreateProcess` with a job object, Ctrl-C and the exit code, the shim's
  replace-aside, `--path` (R17), and the test's link half under Git Bash (it is skipped on
  Windows).
- **macOS** is written for (`_NSGetExecutablePath`, the Library locations) but not run.
- **R14.** On this repository, median of 20 runs on a hot cache: `kbor help` directly
  1.8 ms; through the shim 9.6 ms, of which Ninja's no-op run is 4.4 ms; through the shim
  with `KORVID_LINK_BUILD=never` 5.0 ms. That is far below 150 ms, so `always` stays the
  default everywhere.
- **Settled while building.** The shim is `kvbinlink` (more specific than kvlink). Its home
  is the directory it is in. The table's file name is an FNV-1a hash, and its fields are
  tab-separated. `_link write` and `_link engine` take the anchor as an argument. In a
  project every tool needs the project linked (see Trust). A build that holds the lock sets
  `KORVID_LINK_HELD`, which keeps a linked tool called from inside a build from deadlocking.
- **Not yet in the root README.** Its line about adding `tools/kormake/bin/<platform>` to
  `PATH` is the author's to change.
