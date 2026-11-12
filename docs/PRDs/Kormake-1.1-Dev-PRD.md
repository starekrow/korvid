# Kormake 1.1 — Dev PRD: targets, layout, and the manual

Nov 12, 2026 · @starekrow

## Summary

kormake 1.0 works, but it shows its first-draft edges as the repository grows:

- **Examples and tools get lost.** Thirty-odd engine modules, nine examples and two tools
  sit side by side in `modules/`.
- **kormake builds everything, every time.** `kormake build linux-dev` builds every
  library, example, tool and test, because the only thing a command can name is a
  platform and a config.
- **The documentation is thin.** One README mixes usage, the module file reference and
  internals. Nothing explains how kormake is put together, which is what you need when a
  build fails in an unexpected way.

1.1 fixes these with three changes and a manual. It adds no new build model. Instances,
edges, bindings and the Ninja graph stay exactly as they are.

1. **Layout.** Examples move to `examples/<name>/` and tool modules to `tools/<name>/`.
   `modules/` keeps the engine.
2. **Targets on the command line.** You name what you want built (`kormake kpack`), or
   name nothing inside a module's directory and get that module. The platform and config
   move to an optional argument that defaults to the host.
3. **`korvid.config`.** A small Lua file placed above a directory of modules. It says
   where the engine is, so kormake works from inside an examples or tools directory, or
   from a game that lives outside the engine's repository.
4. **The manual.** `tools/kormake/README.md` is reorganized as a man page.
   `tools/kormake/BUILDING.md` covers the bootstrap and how kormake works inside.

## Goals and non-goals

**Goals**

- `kormake arcfire` builds arcfire and what it links, and nothing else.
- Running `kormake` with no arguments in `examples/arcfire/` does the same.
- A game repository with one `korvid.config` (`engine "../korvid"`) builds against the
  engine with no edits to the engine.
- Every command, argument, option, file and environment variable is documented in one
  place.
- Existing command lines keep working: `kormake build linux-debug`,
  `kormake test linux-dev collision` and `kormake package linux-ship <game>`.
- Module names lose their `example_` and `tool_` prefixes: each module is named for its
  directory (`arcfire`, `kpack`). This is the one change that reaches the games (see
  Renaming).

**Non-goals**

- Moving the toolchain modules or `slang`. Those are platform plumbing, not developer
  tools, so they stay in `modules/`.
- A `run` command, IDE projects, or a kormake binary that rebuilds itself.
- Building a game as an engine library behind the C ABI. That is still R15 of the v1 PRD.

## Layout

```
kormake.lua            master file; modulepath { "modules", "examples", "tools" }
modules/               engine modules, toolchains, slang
examples/korvid.config engine ".."
examples/arcfire/      module "arcfire" (and arcanum, rocks, testbed, ...)
tools/korvid.config    engine ".."
tools/kpack/           module "kpack"
tools/picture/         module "picture"
tools/kormake/         kormake itself (not a module)
tools/ios/             scripts (not a module)
```

Each module is named for its directory: `example_arcfire` becomes `arcfire`, `tool_kpack`
becomes `kpack` (its `exe` field goes, being the same), and so on. `tool` is the library that tools link, so it stays in `modules/`. Directories under
a module path that have no `module.lua` are ignored, as they are today, so `tools/kormake`
and `tools/ios` can sit beside the tool modules.

**Renaming.** A module's name is in more places than its directory, so the rename reaches
the games:

- assets stage to `bin/assets/arcfire/`, and every asset path in code, JSON and maps reads
  `arcfire/...`;
- headers move to `include/korvid/arcfire/`, and module symbols become `kv::arcfire_module`;
- kind, column and system names become `arcfire.ship`, `arcanum.player.Actor` and so on;
- executables, packs and `dist/` follow (`bin/arcfire`, `bin/arcfire.kpak`), as do iOS
  bundle identifiers (`org.korvid.arcfire`).

Kind names feed the state hash, so demo hashes change, and so do the recorded replays,
which also name their executable. Recording the replays again and noting the new hashes
waits until the other changes that affect them have landed. Arcanum's saves written before
the rename name the old kinds and the old game, so they no longer load. The save
directories (`~/.local/share/korvid/<game>`) were never named for the module, so they stay.

## Command line

```
kormake [COMMAND] [NAME...] [TARGET] [OPTIONS]
```

- **COMMAND** is one of `build` (the default when the first word is not a command), `test`,
  `gen`, `check`, `graph`, `package`, `clean`, `help` or `--version`.
- **TARGET** is `<platform>-<config>` with both names in the master file, `host`,
  `host-<config>`, or a bare config name (which means the host platform in that config).
  It can appear anywhere among the arguments. When it is omitted, the target is the
  config's `target` if it sets one, otherwise the host platform in the master file's
  first config. (1.0 used the master's first platform, which was wrong on macOS.)
- **NAME** is a module, matched in this order: its module name (`kpack`), its
  executable name (an `exe` field), then its directory name (for a module whose
  directory is named differently). A name that matches
  two modules at the same step is an error that lists both. Names that collide with a
  config or a platform-config are refused when modules are collected. A NAME with a
  slash in it, or `.`, is a directory instead: it selects every module under it, so
  `kormake examples/` builds just the examples.
- **No NAME** means the current directory decides, and `-C DIR` changes which directory
  that is:
  - inside a module's directory: that module;
  - in a directory that holds modules, such as `examples/`: every module under it;
  - anywhere else, including the root: the master file's roots, as in 1.0.
- **`build NAME`** builds the module's outputs and everything they need: the executable
  or library, its closure, its staged assets and shaders, and in ship its pack. Its tests
  are not built.
- **`test NAME`** builds and runs that module's tests. Naming a module that has no tests
  is an error, but a directory-wide `test` skips modules without tests.
- **`package NAME`** no longer needs a target, which defaults as above.
- **`graph NAME`** prints only the named modules and their closures.
- **`clean`** still takes only a target.

**The graph behind it.** Each target still has one `build.ninja`, and naming modules does
not change its contents. The graph holds the roots' instances plus every other module the
platform supports. A module outside the roots that fails to resolve is dropped from the
graph and its reason is recorded. That reason is printed only if someone names the
module. Every selectable instance gets a Ninja phony named after its module: the target's
instances, plus host-only modules. `kormake NAME` runs `ninja NAME`, and so does
`ninja -C build/linux-dev arcfire`. Test edges are emitted for every selectable
module that has tests. `tests` and `check` still cover only the modules whose roots ask
for tests. `default all` is unchanged.

## korvid.config

kormake starts in the current directory, or in the directory given with `-C`, and walks up
it. At each level it looks for `korvid.config` first, then `kormake.lua`. The first one it
finds settles the question:

- **`kormake.lua`**: that directory is the engine root. This is 1.0's behavior.
- **`korvid.config`**: it runs in the same sandbox as the master file and declares the
  following:

```lua
engine ".."                -- required: the engine root (holds kormake.lua), relative to this file
modulepath { "." }         -- optional: module paths added after the master's, relative to this file; default "."
build "build"              -- optional: the directory that holds <target>/, relative to this file
target "linux-dev"         -- optional: the default target
```

The module paths are the master's, then the config's, then `KORMAKE_MODULE_PATH`.
Duplicates are dropped by real path.

**Where outputs go.** When the module paths come out the same as the master's (the
in-tree `examples/` and `tools/` configs), the build base is the engine root. Every
directory of the repository then shares one `build/`. When a config adds a module path,
as an out-of-tree game does, it is a project of its own. Its build base is the config's
directory, so it gets `build/<target>/`, `build/compile_commands.json` and `dist/` beside
the config, and the engine's tree is never written to. Its own modules are also the only
roots, with their tests, which makes Ninja's default the game and not the whole engine;
engine modules can still be named. `build` moves the build directory in either case. For a
project, or a config that sets `build`, the regenerate rule runs
`kormake -C <the config's directory> gen <target>` and lists the config among its inputs.
Otherwise it runs from the root, so the file written from `examples/` matches the one
written from the root.

The engine root is still where the master file's relative paths, external `dir`s (for
example `vendor/`), `%{root}` and `LICENSE`/`NOTICE` resolve.

## Documentation

- **`tools/kormake/README.md`**, as a man page: NAME, SYNOPSIS, DESCRIPTION, COMMANDS,
  TARGETS, NAMES, OPTIONS, FILES (`kormake.lua`, `korvid.config`, `module.lua` with
  every field, the build directory's layout), ENVIRONMENT, EXIT STATUS, EXAMPLES, NOTES
  and SEE ALSO.
- **`tools/kormake/BUILDING.md`**: the bootstrap (`bootstrap.sh` and `bootstrap.bat`, what
  they compile, when to run them again), how kormake is put together (the C host, the
  embedded scripts, collect, resolve and emit, the generated files), and how to diagnose a
  failed build (gen versus Ninja failures, reading `build.ninja`, `ninja -t commands`,
  `--no-unity`, `graph --why`, host instances, the regenerate loop, a stale binary).
- The root README, the example and tool READMEs, and the arcfire scripts are updated to
  the new paths and command lines.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Examples under `examples/<name>/`, tool modules under `tools/<name>/`, master modulepath updated, `korvid.config` in both | P0 |
| R2 | Modules named for their directories (no `example_`/`tool_`); assets, headers, kinds and outputs follow; replays recorded again later | P0 |
| R3 | Commands take module names (by module, executable or directory name), with ambiguity reported | P0 |
| R4 | No name means the current directory decides: a module, the modules under a directory, or the roots | P0 |
| R5 | The target is optional and positional, defaulting to the config's `target` or host plus first config; a bare config name means the host in that config | P0 |
| R6 | One `build.ninja` per target that holds every module the platform supports; a phony per selectable module; per-module tests | P0 |
| R7 | `korvid.config` discovery, `engine`, `modulepath`, `build` and `target`; `-C DIR` | P0 |
| R8 | An out-of-tree project builds into its own build base; the regenerate rule reproduces the same view | P0 |
| R9 | 1.0 command lines keep working | P0 |
| R10 | `tools/kormake/README.md` as a man page; `tools/kormake/BUILDING.md` | P0 |
| R11 | A warning when the running binary's embedded scripts differ from `tools/kormake/scripts/` under the engine root | P1 |
| R12 | `KORMAKE_VERSION` 1.1; the master file requires 1.1 | P1 |

## Milestones

1. **Layout**: the `git mv`s, the master's modulepath, both `korvid.config` files (read
   by 1.1, harmless to 1.0), and path fixes in docs and scripts. Builds and hashes are
   unchanged.
2. **Discovery**: `korvid.config`, `-C`, the build base, and the regenerate rule.
3. **Targets**: the new argument parser, name matching, the current-directory default,
   available instances, phonies, per-module tests, and `package`/`graph` with names.
4. **Manual**: the README, BUILDING.md, the stale-binary warning, and the version bump.

**Verification.** `kormake build` and `kormake test` pass for linux-debug, linux-dev and
linux-ship. `kormake kpack` and `kormake` run in `examples/arcfire` build only their
closures, which is checked with `ninja -n`. A scratch project outside the repository,
with one `korvid.config` and a hello module that links `core`, builds into its own
`build/`. Before the rename, arcfire's Shoreline demo hash was unchanged
(a85e2f7fa02412e5 at tick 14171). After it, the games run their demos with every asset
found, and the hashes and replays are left for later. Windows (`bootstrap.bat`, `cl`) and
macOS are not verified on this branch. The code has no new OS calls, but Windows path case
in the current-directory matching is worth a look there.

## Open questions

- Should `kormake` with no arguments at the root keep building the roots with all their
  tests? 1.1 says yes so that 1.0 behavior is unchanged. `kormake examples/` is the short way
  to build just the examples.
