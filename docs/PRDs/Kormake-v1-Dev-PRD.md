# Kormake — v1 Dev PRD

Nov 5, 2026 · @starekrow

## Summary

v1 ships `kormake`: a single binary that discovers modules, resolves them into a graph of (module, platform, config) instances, and writes one Ninja build plus `compile_commands.json`.

The problem it solves: existing build systems assume one global toolchain and one target platform. Engines then treat shader compilers, asset tools and backends as external prerequisites, versioned separately from the code that reads their output. That's where engine toolchains break.

Kormake's answer is one model. Everything is a module: engine libraries, backends, build-time tools and platform toolchains. A tool is a module instance built for the host as an executable. Whoever reads a format owns the tool that writes it.

The developer sees three things: the `kormake` binary, a `module.lua` per module, and one master project file that reads top to bottom in an editor.

Rev 2 cuts v1 to the core. No IDE project generation, no Premake, no two-stage build, no tool downloading, no overlay files, and no API versioning machinery. Each of those can come back later as an addition without changing the model.

## Goals and non-goals

**Goals**

- **Check out and build.** Clone a module repo into any module path, and the next `kormake build` picks it up. No edits anywhere else, unless it's a backend that needs a binding.
- **No boilerplate.** A minimal `module.lua` is one line. Source files follow the directory convention unless the module overrides them.
- **A readable master file.** One file lists module paths, roots, platforms and binding overrides. Nothing else is needed to understand what gets built.
- **No global toolchain.** Compilers, shader compilers and asset tools are modules with pinned versions, resolved like any other dependency.
- **One graph for host and target.** Host tools and target code build in a single Ninja invocation, with tools ordered ahead of their consumers by ordinary dependencies.
- **Single binary.** `kormake` is a Lua interpreter with the kormake scripts embedded. There's nothing else to install apart from Ninja.
- **Breaks show up at resolve time.** Cycles, missing bindings and duplicate names fail at generation, before any compiler runs.

**Non-goals for v1**

- **IDE project generation.** VS and Xcode debug any executable, and `compile_commands.json` covers code navigation. Generated vcxproj is needed for console integration, so the VS emitter is v2, started alongside console SDK work.
- **Downloading anything.** Pinned tool binaries are vendored in their module repos. Any download step is a script in that repo, not part of kormake.
- **API discipline for third-party modules.** Rule-interface semver, contract tests and the engine-API handshake wait until external modules exist.
- A package manager or registry.
- Building the game. The game consumes the engine through its C ABI, in whatever language and build it likes.
- An asset pipeline. Live import and packaging run in the engine.
- Hermetic or remote builds, and a distributed cache.

## Core model

The build graph is made of **instances**, not modules. An instance is a tuple of (module, platform, config). One module can appear as several instances: `core` for the target and `core` for the host, if a tool links it.

**One module shape**

There are no module kinds. A module has outputs (`lib`, the default; `exe`; or `headers`), an optional `provides`, and optionally `prebuilt` binaries in its own directory. The roles from rev 1 are combinations of those:

| Role | How it's expressed |
| --- | --- |
| Engine library | `output = "lib"` (the default) |
| Interface | `output = "headers"`, plus a dispatch mode (link-time symbols or a function table) |
| Backend | Any module with `provides = "<interface>"` and the platforms it supports |
| Tool | A module with `output = "exe"` that exports rules. What makes it a tool is being reached through a tool edge |
| Platform | A module with `provides = "toolchain"` that also declares default bindings for its platform |
| Prebuilt | A module with `prebuilt` binaries per platform and no sources |

Toolchain components use the same interfaces. `shader-compile` is an interface; Slang and each console vendor's compiler wrapper are backends for it.

**Two edge types**

- **link**: the child takes the parent's platform and config.
- **tool**: the child switches to the host platform and release config. The consumer gets the child's rules, not a library to link.

Explicit transitions to a named platform (DSP, coprocessor, WASM) are deferred. They'd be a third edge type and change nothing else.

**Bindings**

- Everything is bound per platform, and the host is just a platform. Shaders for a host-side preview come from the host's `shader-compile` binding.
- Platform modules declare default bindings for their platform. The master file only overrides them.
- If an interface's dispatch mode is a function table, several backends can be bound at once, with the choice made at runtime (d3d12 and vulkan on Windows).

**Rules for the graph**

- No cycles in the instance graph.
- A module only sees the `include/` directories of its declared dependencies. Undeclared includes fail to compile.
- A tool rule's tool binary hash is an implicit input to every output it produces.

## Modules, discovery and the master file

**Discovery**

- Module paths are searched in order: the ones the master file lists, then `KORMAKE_MODULE_PATH`. Each entry is a directory that holds module directories, and `~` is expanded.
- A module is any directory directly under a path entry that contains `module.lua`. No recursive scan.
- A module's name comes from its file, not its directory, so `korvid-audio-fmod/` can declare `audio.fmod`.
- The same name in two places is an error that names both paths. You resolve it with `override "name" { path = ... }` in the master file.
- Discovered isn't built. Only instances reachable from the roots get built.
- A console port is just another module path entry, pointing at a private repo that holds its platform module, backends and compiler wrapper.

**Directory convention** (used whenever `module.lua` doesn't override it)

```
<module>/
  module.lua
  include/korvid/<name>/   public headers; the path is the namespace
  src/                     private sources
  src/<platform>/          picked by platform tag (win32, posix, apple, ...)
  shaders/                 handed to whatever rule the module's shader dependency provides
  tests/                   one test executable per module
  bin/<platform>/          prebuilt binaries, when the module vendors any
```

Generated sources go in the build directory, never the source tree.

**module.lua**

- It runs in a sandboxed Lua environment that exposes only the module API. No `os`, `io` or `require`, and reads are limited to the module's own directory.
- Its output is plain data. Declaring the same module twice, or using an unknown key, is an error.
- Fields: `output`, `deps`, `provides`, `platforms`, `tools`, `generate`, `exports` (rules), `sources` (overrides the convention), `defines`, `external` (system dependencies), `prebuilt`, `bindings` (defaults, for platform modules).
- Per-platform settings go in a `platform` table keyed by platform tag. There is no implicit filter scope.

```lua
module "core"
```

```lua
module "gpu.vulkan" {
  provides  = "gpu",
  platforms = { "win64", "linux", "android" },
  deps      = { "core" },
  external  = { vulkan = ">=1.3" },
}
```

**Master file** (`kormake.lua` at the project root)

```lua
project "mygame-engine"
requires_kormake "1.0"

modulepath { "modules", "../mymodules", "~/korvidExternal" }

platforms { "win64", "linux", "macos" }
configs   { "debug", "dev", "ship" }

-- platform modules supply defaults; these lines only override them
bind "gpu" { win64 = { "gpu.d3d12", "gpu.vulkan" } }

root "engine"  { link = "all", abi = "c" }
root "tests"   { modules = "*" }

override "render" { path = "../render-fork" }
```

The master file runs in the same sandbox. There are no overlay files: everything a platform needs arrives through its modules.

## Toolchain as modules

Toolchain components are API surface. v1 makes that explicit: a tool is a module, its rules are its interface, and the module that consumes a format pins the tool that produces it.

**Tools**

- A tool is any module with an `exe` output reached through a tool edge. It's either built from source for the host, or vendored: binaries per host under `bin/<host>/`, pinned by being checked in (Git LFS where size matters).
- Consumers can't tell a built tool from a vendored one.
- A tool module can also export a library. Slang is the example: `slangc` for offline builds, `libslang` linked into dev configs for in-process shader compiles during live import.

**Exported rules**

A rule is the tool's contract with consumers:

- `inputs`: glob patterns relative to the consuming module.
- `outputs`: output names templated on the input stem, under the instance's generated directory.
- `deps`: how the tool reports dependencies (`depfile` in gcc format, `msvc` showIncludes style, or `scanner`, a second tool that emits a depfile).
- `params`: a small, typed set of flags. Anything richer stays inside the tool.
- Outputs whose suffix the consumer's compile rule recognizes are added to that instance's sources automatically.

```lua
module "slang" {
  output   = "exe",
  provides = "shader-compile",
  prebuilt = { win64 = "bin/win64/slangc.exe", linux = "bin/linux/slangc", macos = "bin/macos/slangc" },
  exports  = {
    rule "shader" {
      inputs  = "shaders/*.slang",
      outputs = "%{stem}.%{target_ext}",
      deps    = "depfile",
      params  = { target = "enum", profile = "string" },
    },
  },
}
```

**Platform modules**

- A platform module defines its tag, its compiler driver (a tool module or a system lookup), flags per config, sysroot, linker, archive and executable conventions, and default bindings for its interfaces.
- The host platform is detected at startup and bound to the `host` tag. The default host compiler is a system lookup.
- On Windows the platform module captures the MSVC environment (the vcvars equivalent) so Ninja can call `cl` and `link` directly.

## Pipeline

Every invocation runs collect, resolve and emit, then hands one graph to Ninja. No user Lua runs after collect.

&#91;embedded content: kormake pipeline · 3 phases, one Ninja graph\]

1. **Collect.** Evaluate the master file, scan module paths, and evaluate each `module.lua` in the sandbox. The output is a list of module records.
2. **Resolve.** Expand roots into instances by applying link and tool edges. Bind interfaces per platform, taking platform defaults and then master-file overrides. Check for cycles, duplicates and include visibility. The output is the instance graph, cached as `build/.kormake/graph.json` and keyed by a hash of every input file plus the `kormake` version.
3. **Emit.** Write one Ninja build covering host and target instances, plus `compile_commands.json`. Tool rules point at their host instance outputs (or vendored binaries), so Ninja builds tools ahead of their consumers by ordinary dependency order. There is no separate tool stage.

The Ninja build includes a regenerate rule: if any `module.lua` or the master file changes, `kormake gen` runs again before the build continues.

## CLI and distribution

`kormake` is one executable: a Lua 5.4 interpreter with the module API, resolver, rules and Ninja emitter embedded as scripts.

`kormake` is the tool's name for now. Later, `korvid make ...` may become a synonym for it.

| Command | What it does |
| --- | --- |
| `kormake build <platform>-<config> [roots]` | Resolve, write Ninja files if inputs changed, run Ninja |
| `kormake gen [platform]` | Resolve and write Ninja files and `compile_commands.json` only |
| `kormake graph [--dot] [--why <module>]` | Print the instance graph; `--why` shows the path that pulled a module in |
| `kormake check` | Resolve only: cycles, bindings, duplicates, sandbox violations |
| `kormake test [module]` | Build and run module tests |
| `kormake clean` | Remove build outputs |

**Distribution**

- Release binaries for win64, linux-x64, linux-arm64 and macos-universal, under the same Apache 2 license as the engine.
- Ninja must be on PATH. Bundling it is an open question.
- `kormake --version` prints the tool version. The master file can require a minimum version.

## Requirements

P0 ships in v1, P1 should, P2 is designed for but comes later.

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Discover modules from ordered module paths plus `KORMAKE_MODULE_PATH`; error on duplicate names, naming both paths | P0 |
| R2 | Sandboxed evaluation of `module.lua` and master files; the output is plain data | P0 |
| R3 | Directory-convention defaults for sources, public includes, platform subdirectories, tests and `bin/<platform>/` | P0 |
| R4 | One module record shape: outputs, `provides`, `prebuilt` | P0 |
| R5 | Instance graph over (module, platform, config) with link and tool edges | P0 |
| R6 | Cycle detection and resolve-time errors that name the path through the graph | P0 |
| R7 | Per-platform bindings, with defaults from platform modules and overrides in the master file; host treated as a platform | P0 |
| R8 | Include isolation: each instance sees only its declared dependencies' public headers | P0 |
| R9 | Tool rules with inputs, outputs, depfile handling and the tool hash as an implicit input | P0 |
| R10 | Vendored per-host tool binaries, used as if built | P0 |
| R11 | One Ninja build covering host and target instances, with a regenerate rule | P0 |
| R12 | `compile_commands.json` for every target instance | P0 |
| R13 | Per-module unity builds on by default, switchable per module and globally | P0 |
| R14 | Single `kormake` binary for win64, linux and macOS | P0 |
| R15 | Engine root links all engine instances into one library behind the C ABI, with a generated header bundle | P0 |
| R16 | MSVC environment capture in the Windows platform module | P0 |
| R17 | `kormake graph --why` and `--dot` | P1 |
| R18 | Minimum `kormake` version check from the master file | P1 |
| R19 | Visual Studio emitter (vcxproj per instance) for console integration | P2 |
| R20 | Xcode emitter | P2 |
| R21 | Explicit platform transitions for heterogeneous processors | P2 |
| R22 | Pinned compiler modules replacing the system lookup for the host | P2 |
| R23 | Rule-interface semver, contract tests and engine-API version ranges | P2 |

## Open questions, risks and milestones

**Open questions**

- Should Ninja be bundled in the `kormake` binary or required on PATH?
- Does each interface fix its dispatch mode, or can a backend offer both?
- Is a generated directory per instance always correct, or do some platform-independent codegen tools want one shared output?
- How are `src/<platform>/` tags structured: a fixed set (win32, posix, apple) or a hierarchy (posix > linux > android)?
- Should module tests be one executable per module or one per root?
- Should vendored binaries require Git LFS, or is that left to each module repo?

**Risks**

| Risk | Effect | Mitigation |
| --- | --- | --- |
| Ninja with MSVC needs the right environment and showIncludes parsing | Windows builds are fragile | The Windows platform module captures the environment once; Ninja's `deps = msvc` handles headers |
| No generated IDE projects | Debugging setup is manual on PC | VS Open Folder and Xcode debug any executable; `compile_commands.json` for navigation |
| A VS emitter later needs data the graph doesn't hold | v2 work stalls | The instance graph keeps everything an emitter needs: sources, flags, deps, rules, tool paths |
| Vendored binaries bloat module repos | Slow clones | Git LFS, or a download script in the module repo |
| Rule interfaces turn into a broad API | Modules break on tool changes | Keep rule params narrow; add semver and contract tests when third-party modules appear |
| Unity builds hide include mistakes | Breaks appear when unity is turned off | CI builds with unity off at least once per change set |

**Milestones**

1. **Resolve**: discovery, sandbox, instance graph, bindings, `kormake check` and `kormake graph`. No emission.
2. **Ninja end to end**: the convention plus unity builds, an engine root with `core` and one backend, building on linux and on win64 with MSVC.
3. **Tools**: vendored Slang, the shader rule with depfiles, generated sources feeding compiles, all in one Ninja graph.
4. **Single binary**: embedded scripts, release builds for all hosts.
5. **P1 set**: `graph --why` and `--dot`, minimum version check.

The VS emitter is v2 and starts alongside console SDK work.
