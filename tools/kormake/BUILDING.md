# Building kormake, and how it works

`README.md` covers using kormake. This file covers building kormake itself and how it is
put together, which is what you need when a build fails in a way the error message does
not explain.

## Bootstrap

kormake is built by a script that needs only the host's C compiler. You run the script
once after cloning, and again whenever `tools/kormake/scripts/` or `tools/kormake/src/`
changes. kormake warns about stale scripts (see below), but it cannot tell when `src/`
has changed.

    tools/kormake/bootstrap.sh       # Linux, macOS: cc (or $CC) -> tools/kormake/bin/<host>/kormake
    tools\kormake\bootstrap.bat      # Windows: cl -> tools\kormake\bin\windows\kormake.exe

Both scripts can be run from any directory, and both do the same three steps:

1. **embed.** They compile `src/embed.c`, a small program, and run it over
   `scripts/*.lua`. The result is `bin/.build/scripts.inc`: a C array of
   `{name, text}` entries, one per script, where the name is the file name without
   `.lua`.
2. **Lua.** They compile every `vendor/lua/*.c` except `lua.c` and `luac.c` (the
   standalone interpreter and compiler). This is Lua 5.5.1, unchanged.
   `vendor/UPGRADE.md` explains how to move to a newer release.
3. **The host.** They compile `src/kormake.c` with the OS layer (`src/os_posix.c` or
   `src/os_win32.c`) and link everything into one executable.

`bootstrap.sh` builds with `-O2 -std=gnu99 -DLUA_USE_POSIX`. `bootstrap.bat` builds with
`/O2 /std:c17 /W3 /utf-8`. If there is no `cl` on `PATH`, `bootstrap.bat` looks for the
newest Visual Studio, or the Build Tools with the C++ workload, through `vswhere`, and runs
its `vcvarsall` for that run only. That means any command prompt works.

The output in `bin/` is ignored by git. Nothing beyond the compiler is needed to bootstrap;
building the engine needs Ninja on `PATH`.

**A stale binary.** Because the scripts are compiled into the binary, editing a script
changes nothing until you bootstrap again. On every run, kormake compares its embedded
scripts with `tools/kormake/scripts/` under the engine root and names any that differ:

    kormake: warning: this binary's emit.lua differs from .../tools/kormake/scripts; run .../bootstrap.sh

A stale binary may stop at `gen` on a module field it does not recognize
(`module 'x' has an unknown field 'y'`). That error comes from an old binary, not from a
bad module file.

## The pieces

```
tools/kormake/
  bootstrap.sh, bootstrap.bat
  src/kormake.c      the host: a Lua state, the kfs table, the embedded scripts, main
  src/os.h           the OS layer the host calls (stat, listdir, mkdir, copy, exec, ...)
  src/os_posix.c     ... on Linux and macOS
  src/os_win32.c     ... on Windows: UTF-16 at the API, UTF-8 and '/' everywhere else
  src/embed.c        the script embedder
  scripts/main.lua   the command line: parse, discover, resolve, emit, run Ninja
  scripts/util.lua   paths, globs, quoting, Ninja escaping
  scripts/sandbox.lua    evaluates kormake.lua, korvid.config and module.lua
  scripts/collect.lua    finds the engine root and the modules
  scripts/resolve.lua    builds the instance graph
  scripts/emit.lua       writes build.ninja and compile_commands.json
  scripts/probe_msvc.lua, probe_xcode.lua    toolchain probes: find the compiler, SDK and Ninja
  scripts/package_app.lua                    the .app bundle for `kormake package` on Apple platforms
  tests/run.sh       kormake's own test (see Testing kormake)
  tests/fixture/     ... an engine root of its own that the test generates
  tests/expected/    ... and what it must generate
  vendor/lua/        Lua 5.5.1
```

**The host** (`kormake.c`) gives the scripts a small API. The global `kfs` holds
`listdir`, `isdir`, `isfile`, `mtime`, `read`, `write`, `mkdirp`, `rmtree`, `copy`, `cwd`,
`realpath`, `getenv`, `exec` and `self`. The other globals are `kormake_script(name)`,
which returns an embedded script's source, along with `KORMAKE_VERSION`, `KORMAKE_HOST`
(`linux`, `macos` or `windows`, fixed at compile time) and `arg`. `kfs.write` writes a
file only if its content has changed, so regeneration never touches a file Ninja depends
on unless something really changed. The one exception is `build.ninja`, which is always
touched; the regenerate loop below explains why. Paths given to the scripts always use
`/`, on Windows too.

**The sandbox.** `module.lua`, `kormake.lua` and `korvid.config` run with only the
declaration functions, `string`, `table`, `math`, `utf8` and the basic functions. There is
no `io`, `os`, `require` or `load`. What they declare is copied out as plain data
(strings, numbers, booleans and tables), and an unknown field is an error. As a result,
no user Lua runs after collection.

## The pipeline

Every command except `clean`, `help` and `--version` runs the same pipeline.

**1. Discover** (`collect.discover`). Starting from the current directory, or from `-C`,
kormake walks up to the first `korvid.config` or `kormake.lua` and settles on the engine
root plus an *anchor*, which is the directory where it found the file. A `korvid.config`'s
`engine` must name a directory that contains a `kormake.lua`.

**2. Collect** (`collect.run`). kormake evaluates the master file, then builds the list of
module paths: the master file's (relative to the root), then the config's (relative to the
anchor; the anchor itself by default), then `KORMAKE_MODULE_PATH`. Each path is
deduplicated by its real path. It lists every directory directly under each path, sorted,
evaluates each `module.lua` it finds, and applies the directory convention, which records
`include/`, `src/`, `tests/`, `shaders/` and `assets/` if they exist. A config that adds a
path makes the build a *project*: its build base is the anchor rather than the root, and
its own modules are the roots. Duplicate module names, and names reserved for targets, are
errors at this stage.

**3. Resolve** (`resolve.run`). This step turns modules into instances, keyed
`module@platform-config`.

- *Toolchains.* There is one per platform, chosen by `provides = "toolchain"` and
  `platforms`, with `per_platform[<platform>]` laid over it. Bindings come from the
  toolchain's defaults, then from the master file's `bind` lines.
- *Roots.* Every module that matches a root's patterns (on a platform the root applies
  to) is instanced for the target. Link edges follow `deps`, and a dependency on an
  interface also links the backend bound to it. Tool edges switch to the host platform in
  `host_config`. A module with `platforms = { "host" }` is always instanced that way.
  Cycles, missing modules, unsupported platforms, unknown externals and unbound interfaces
  fail here, and the message names the module that asked. The last three are *soft*
  failures (`util.soft_fail`): they say a module cannot build here, not that anything is
  wrong.
- *Everything the roots reach* is marked `in_roots`: that is Ninja's `all`.
- *Every other module* the platform supports is then tried. If one fails softly, the
  instances it added are taken back out and the reason is kept in `g.unavailable`, which is
  what `kormake NAME` prints when you ask for that module and what `check` lists as left
  out. Any other failure (a cycle, a missing module, a bug in kormake) stops the run, as it
  would for a root.
- *Closures.* Each instance gets its closure: the link dependencies, in dependency order.
  This drives include paths, link lines and module lists.

An instance is *selectable* (it gets a phony and can be named) when it is the target's own
instance, or the only instance of a host module, and it is something kormake builds (not a
toolchain, a `system` tool or a `prebuilt` one).

**4. Emit** (`emit.run`). kormake writes one Ninja file in dependency order.

- *Toolchain rules.* Each toolchain's `cxx`, `ar` and `link` rules are suffixed with its
  tag (`cxx_linux`, `link_windows`), so target and host rules can coexist. The file's
  variables (`cxx`, `cxxflags`, ...) are the target's, and a host instance overrides them
  on each of its build statements.
- *kormake's own rules.* `copy` (`kormake _copy IN OUT`), `run_test`
  (`kormake _test EXE STAMP [runner...]`) and `regen` all run the kormake binary itself,
  so no rule needs a shell.
- *Tool rules.* There is one Ninja rule per (tool, rule, platform variant), named
  `tool_<module>_<rule>[_<platform>]`.
- *Left out.* First, for each instance outside the roots, kormake checks what would stop
  the generation: its externals' package directories, its private includes, its tools'
  binaries. If one fails, that instance and every instance built on it are left out
  (`inst.dropped`, with the reason in `g.unavailable`) and get no edges. For the roots'
  instances the same checks fail the run, with the toolchain's instructions.
- *Per instance.* Each instance gets its sources: `src/*.cc`, then each of the toolchain's
  `srcdirs`, then the same again for `.mm`. With unity on and more than one source, these
  go into `gen/<module>/unity.cc`. An executable also gets `modules.gen.cc`. After that
  come the compile edges with include flags (the closure's `include/`, the module's own,
  `private_includes`, and externals as system includes), then the tool edges for its
  files, the asset copies, and an archive or a link. An executable's link takes everything
  it stages (assets, shaders, DLLs) as order-only inputs, so building the executable also
  builds what it reads.
- *Tests.* Every selectable module with a `tests/` directory gets
  `bin/<module>_tests` and `test/<module>.ok`. Like an executable's link, the test's link
  takes what the module and its closure stage (assets, shaders, DLLs) as order-only
  inputs. The stamp takes the same files as implicit inputs, so a changed asset re-copies
  and reruns the test without relinking it, and a module's test only reruns for its own
  closure's files. A host instance's edges (`host/bin/...`, `host/test/...`, with
  `run_test_host` when the target has a runner) work the same way.
- *Phonies.* kormake writes `tests`, `check`, `all` (with `default all`), and one phony
  per selectable module.
- *Regeneration.* The `build.ninja` edge depends on the master file, every `module.lua`
  collected (built here or not, so an edit can let a left-out module in) and, for a project
  or a config that sets `build`, its `korvid.config`. A new module directory is not an
  input: run kormake (any command) to pick it up.

kormake then writes `compile_commands.json`, both beside `build.ninja` and one level up.

**5. Run.** kormake runs `ninja -C build/<target>` with the selection's phonies (for
`build`), its test stamps (for `test`), Ninja's default (for `build` with nothing
selected), or `check` (for `test` with nothing selected). If the
toolchain probe found a Ninja, that one is used.

## Testing kormake

    tools/kormake/tests/run.sh             # bootstrap, then compare
    tools/kormake/tests/run.sh --update    # bootstrap, then write tests/expected/

`tests/fixture/` is a small engine root that reaches every kind of edge kormake writes:
a unity library with a platform source directory, a package with a runtime file, assets
(one with a name Ninja has to escape), an interface bound to a backend, a header-only
module that stages a tool's outputs, two host tools (one per-file rule, one per-exe pack),
tests, a cross platform with a test runner, and a module left out for a missing package.
Its toolchains name compilers that do not exist, since nothing is compiled.

The script copies the fixture to a temporary directory, runs `gen` for `host-dev`,
`host-ship` and `cross-dev`, and then `check`, `graph`, `graph --why` and a `build` of
the left-out module. It compares each `build.ninja` and the commands' output with
`tests/expected/`. Paths are written as `{root}` and `{kormake}`, and the host's
platform as `{host}`, so the expected files are the same on Linux and macOS. When Ninja
is on `PATH`, it also parses each file with a dry run.

Run it after any change to `scripts/`. A difference is either a bug or a change you
meant; for the second, run it with `--update` and commit `tests/expected/` with the
change, so the diff shows what the change does to the Ninja files. The test runs on
Linux and macOS only: Windows quotes paths its own way, so its files would need expected
files of their own, and there is no `run.bat`.

## Diagnosing a failed build

**Who failed, kormake or Ninja?** Lines that begin `kormake:` come from discovery,
collection, resolution or emission, before anything is compiled. Lines that begin `[n/m]`
or `FAILED:` come from Ninja and the compiler.

**Errors from kormake.**

- *`module 'x' is needed by y but does not exist`*: a typo in `deps`, or the module's path
  is not on the module path. Run `kormake graph --why y` to see who wanted `y`.
- *`dependency cycle: a -> b -> a`*: the path printed is the cycle.
- *`'x' depends on interface 'render' but no backend is bound for <platform>`*: the
  toolchain has no binding for that interface and the master file has no `bind` line for
  it.
- *`module 'x' (needed by y) does not support platform 'p'`*: `x` has a `platforms` list
  that leaves `p` out, and something on `p` links it.
- *`'x' links 'kpack', which is built for the host only; name it in tools`*: a host
  tool was listed in `deps`.
- *`external 'sdl3' (toolchain ...) is not at ...`*: the package is missing. The message
  includes the toolchain's `fetch` instructions.
- *`x: nothing to build for <target>: ...`*: you named a module that cannot be built for
  the target. The part after the colon is the reason resolution recorded.
- *`unknown field`*: a typo in a module file, or a stale binary (see Bootstrap).

**Errors from Ninja.**

- `kormake gen <target>` writes the Ninja file without building, and
  `build/<target>/build.ninja` is readable text. Search it for the output that failed.
- `ninja -C build/<target> -t commands <output>` prints every command needed to build an
  output, and `-v` prints each command as it runs. `-t query <output>` shows an edge's
  inputs and outputs.
- *An error inside `gen/<module>/unity.cc`* points into one of the sources it includes,
  but two files in one unity build share a translation unit: a `static` function or a
  `using` in one is visible in the next. Rebuild with `--no-unity` to see which file is
  really at fault, or to find an include that the unity build was hiding. Watch for local
  names that clash between files (`stage`, `patterns`) and for missing includes.
- *An undefined symbol at link time* usually means a missing `deps` entry. The module
  compiled against a header it found in some other module's closure, but its own closure
  does not link the library. Check `kormake graph NAME`.
- *An include not found*: a module sees only its own `include/` and those of its closure.
  Add the dependency; do not add a path.
- *Host instances.* In `linux-debug`, `linux-ship` and cross builds, tools build a second
  time under `host/`, with the host toolchain's flags. An error in `host/obj/...` is that
  build. `kormake` prints the host tools it will run, together with the system libraries
  each one links.
- *compile_commands.json* contains the target's commands first, which is what clangd uses
  for a file. `build/compile_commands.json` is whichever target was generated last.

**The regenerate loop.** Ninja reruns `kormake -C <dir> gen <target>` whenever the master
file, a `module.lua` or a project's `korvid.config` is newer than `build.ninja`. kormake
always touches `build.ninja`, even when its content is unchanged. If it did not, an edit
that changed nothing would leave `build.ninja` older than its inputs, and Ninja would
regenerate it forever. If you see `KORMAKE gen` on every build, check for an input with a
modification time in the future.

**A clean slate.** `kormake clean <target>` removes `build/<target>/`. Everything kormake
writes under a target's directory is generated, so removing one is always safe.
