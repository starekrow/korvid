# kormake(1)

## NAME

kormake — build Korvid modules and the programs and tools made from them

## SYNOPSIS

    kormake [build] [NAME...] [TARGET] [OPTIONS]
    kormake test    [NAME...] [TARGET] [OPTIONS] [--heavy [-- ARGS]]
    kormake build|test|gen ... --sanitize address,undefined|thread
    kormake package NAME      [TARGET] [OPTIONS]
    kormake gen     [TARGET] [OPTIONS]
    kormake check   [TARGET] [OPTIONS]
    kormake graph   [NAME...] [TARGET] [--dot] [--why MODULE] [OPTIONS]
    kormake clean   [TARGET] [OPTIONS]
    kormake run     SCRIPT.lua [ARGS...]
    kormake help | --help | -h
    kormake --version | -v

## DESCRIPTION

kormake is a single executable: Lua 5.5.1 with kormake's scripts embedded in it. It finds
the engine root and the module paths. It evaluates the master file `kormake.lua` and every
`module.lua`, and resolves them into a graph of (module, platform, config) instances. It
then writes one Ninja file per target, `build/<target>/build.ninja`, along with
`compile_commands.json`, and finally runs Ninja on the part of that graph that you asked
for.

Everything is a module: engine libraries, backends, games, build-time tools, and platform
toolchains. A tool that a module uses at build time (a shader compiler, the packer) is
built for the machine running the build, in the same Ninja graph, ahead of the module
that uses it. The model is laid out in `docs/PRDs/Kormake-v1-Dev-PRD.md`, and this
version's changes in `docs/PRDs/Kormake-1.1-Dev-PRD.md` and, for 1.2, `docs/PRDs/Engine-Slice-10-PRD.md`. See `BUILDING.md` beside this
file for how to bootstrap kormake and how it works inside.

Each command line is made of a command, some module names, and one target. These can come
in any order, except that the command has to be the first word.

## COMMANDS

**build** (the default)
: Builds the named modules (see NAMES): each one's executable or library, everything it
  links, its shaders, and its assets, cooked by goldberg into its output tree (see Assets).
  A module's tests are not built. With no NAME and nothing chosen by the current directory, kormake builds
  Ninja's default, `all`: every root of the master file and the tests those roots ask for.

**test**
: Builds and runs the tests of the named modules. Each module's tests are one executable,
  `bin/<module>_tests`, built from `tests/*.cc`. A test passes when it exits 0, and kormake
  then writes a stamp, `test/<module>.ok`, so a test that passed is not run again until
  its executable, or a file staged for it (an asset, a shader), changes. Naming a module that has no tests is an error. When the modules
  come from a directory, the ones without tests are skipped. With nothing selected,
  kormake runs `check`: every test that the roots ask for.

  `--heavy` runs the modules' heavy tests instead: `bin/<module>_heavy`, built from
  `tests/heavy/*.cc` like the tests are (and built wherever they are, so it keeps
  compiling). They are for what takes minutes and gigabytes, and for measurements, so
  Ninja never runs them: kormake builds `heavy/<module>` (the executable and what it
  runs), then runs each one itself, in turn, on this machine, every time, with no stamp.
  The words after `--` are passed to each: `kormake test --heavy goldberg -- --size 2G`.
  A target whose tests run under a runner (a device) has no heavy tests.

**package**
: Builds one executable and puts it in `dist/<exe>/` together with what it needs at run
  time: its cooked assets, loose (a pack is built by name and not packaged, for now), its shaders, the repository's
  `LICENSE` and `NOTICE`, and every license file named by a module in its closure (see
  `licenses`). When the toolchain packages applications, the result is an `.app` instead:
  macOS gets `Contents/MacOS` and `Contents/Resources` with SDL3 bundled and the app
  signed, and iOS gets a flat bundle signed for development. The directory is replaced
  each time. The usual config for this is ship.

**gen**
: Resolves the graph and writes `build.ninja` and `compile_commands.json` without running
  Ninja.

**check**
: Resolves the graph only. Cycles, missing modules, unknown externals, unbound interfaces,
  duplicate names and sandbox violations are reported here, before any compiler runs. A
  cycle or a missing module is an error wherever it is. A module outside the roots that
  only cannot build for this target (a platform it does not support, an external or an
  interface the platform lacks, a package that is not there) is not an error: `check`
  lists it as left out, with why. A root that cannot build on this machine (its package
  is not fetched) is a problem: `check` names it, with the toolchain's instructions, and
  exits 1, and `gen` and `build` stop on it. `graph` still prints, marking it.

**graph**
: Prints the instance graph in dependency order: each instance's output, what it links,
  the tools it uses, its externals (a project's with the file that defines it), and whether
  a root asks for its tests. With NAMEs (or a current
  directory that selects modules), it prints only those modules and what they reach.
  Without them it prints what the roots reach. `--dot` prints Graphviz input instead, with tool edges drawn dashed. `--why MODULE`
  prints the chain of instances that pulled MODULE into the graph.

**run**
: Runs a Lua script with the engine's script runner, `kvscript` (`tools/kvscript`):
  Lua 5.5 with `kv.image`, `kv.font`, `kv.zip` and `kv.fs`, for the tools that make a
  game's assets. kormake builds `kvscript` for the host first (quietly; a built one costs
  a Ninja run that does nothing) and runs the script from the current directory. Every
  word after the script is the script's, options and targets alike. The exit status is
  the script's.

**clean**
: Removes the target's build directory, and its sanitized builds (`build/<target>+...`). With no TARGET, it removes every target's
  directory, `compile_commands.json`, and what goldberg cooked and remembers (`assets/`,
  `assets-ship/`, `goldberg/`), and then the build directory itself if that leaves it
  empty. It removes nothing else it finds there, and never `dist/`. This command takes no NAMEs.

**help**, **--version**
: Print the usage summary, or the version (`kormake 1.2 (Lua 5.5)`).

## TARGETS

A target is a platform plus a config, written `<platform>-<config>`. Both names have to be
in the master file's `platforms` and `configs` lists (`linux-dev`, `windows-ship`,
`iossim-debug`). Three shorter forms are also accepted:

- a config alone (`dev`) means the host platform in that config;
- `host` means the platform kormake runs on, in the master file's `host_config` (which is
  dev unless the file says otherwise);
- `host-<config>` means the host platform in that config.

With no TARGET, kormake uses the `target` from the `korvid.config` if one sets it.
Otherwise it uses the host platform in the first config the master file lists (debug).

The host is the platform kormake was compiled for: linux, macos or windows. A tool that a
build runs is always built for the host, in `host_config`. When the target is the host in
that config (`linux-dev` on Linux), the tool and the target share one set of instances and
outputs. Otherwise, such as with `linux-debug`, `linux-ship` or a cross build, the host
instances are a second set in the same Ninja file, under `host/` in the build directory
(`host/bin/kpack`, `host/obj/`, `host/lib/`).

## NAMES

A NAME picks out one module. kormake tries three things in order and takes the first that
matches:

1. the module's name (`kpack`, `arcfire`);
2. its executable's name, when its `exe` field sets one;
3. its directory's name, when that differs from the module's (`korvid-audio-fmod/`
   declaring `audio.fmod`).

The modules in this repository are named for their directories, so the first step is the
one that matters here.

If two modules match at the same step, kormake stops and lists them both. Name the module
to settle it.

A NAME that contains a slash, or is `.` or `..`, is a directory instead. It selects the
module the directory is inside, or every module under it. For example, `kormake examples/`
builds every example.

When there is no NAME, the current directory decides (`-C` changes which directory counts
as current):

- inside a module's directory: that module;
- in a directory that holds modules (`examples/`, `tools/`): every module under it (in a
  project, only the project's own, when the engine is checked out below it);
- at the engine root, at a project's own directory, or anywhere else with no module under
  it: the roots (Ninja's default).

kormake builds and tests only the modules the target's platform supports. A module that
was named but cannot be built for the target is an error, and kormake says why: it is a
toolchain, it is a tool kormake finds on the system, it lists other platforms, or it
fails to resolve. A module that came from a directory and cannot be built is skipped, with
a note saying so.

Module names may not collide with a target (`dev`, `linux-dev`, `host`) or with Ninja's
own names (`all`, `tests`, `check`, `build.ninja`). kormake refuses such a module when it
collects the modules.

## OPTIONS

**-C DIR**
: Starts in DIR instead of the current directory, both for finding the engine and for
  choosing modules. The regenerate rule uses it.

**--no-unity**, **--unity**
: `--no-unity` compiles every source file on its own, instead of one unity file per module
  per language. Use it to find a missing include that the unity build hides. A module's
  `unity` field overrides this either way. Tests are never unity builds.

**--cxx PATH**
: Replaces the compiler of the target's toolchain. It does not apply to host instances.

**--dot**, **--why MODULE**
: Options for `graph`.

**--sanitize LIST**
: For `build`, `test` and `gen`: builds every instance of the target with the sanitizers
  named, a comma-separated list of the toolchain's `sanitizers` (`address`, `undefined`
  and `thread` on Linux). The engine's modules are built so too, and every module's tests,
  with `KV_SANITIZE_<NAME>=1` defined for each name. The build directory is its own,
  `build/<target>+<names>/` with the names in order (`build/linux-dev+address+undefined/`),
  so the plain target's objects and stamps are not touched; `compile_commands.json` at
  the build root is left as the plain targets wrote it. A finding fails the test that
  made it: the address and thread sanitizers stop at their first report, and Linux's
  `undefined` is built with `-fno-sanitize-recover`. `address` with `thread` is refused,
  as the compilers refuse it, and so is a toolchain without the table. A tool the build
  runs (goldberg cooking, a shader compiler) is sanitized when it is one of the target's
  instances, which it is when the target is the host in the tools' config (`linux-dev`);
  `linux-debug`'s tools are built plain. `core`'s arenas are not poisoned: the address
  sanitizer sees the general allocator's blocks and the stack, not a use after an arena's
  reset. To keep going past a failing test, run Ninja yourself:
  `ninja -C build/linux-dev+address+undefined -k 0 check`.

Unknown options are an error. Ninja's own options (`-j`, `-k`, `-n`) are not passed through:
run `ninja -C build/<target> ...` yourself, since every module is also a Ninja target (see
FILES).

## FILES

### Finding the engine

kormake starts in the current directory and walks up through its parents. At each level it
looks for `korvid.config` first, then for `kormake.lua`, and the first file it finds
settles the matter:

- `kormake.lua` means this directory is the engine root;
- `korvid.config` names the engine root.

If neither file is found anywhere, kormake reports an error.

### korvid.config

A small Lua file placed above one or more module directories. It runs in the same sandbox
as the master file, and its paths are relative to the file's own directory.

    engine ".."                -- required: the engine root, the directory that holds kormake.lua
    modulepath { "." }         -- module paths to add; the default is this directory
    build "build"              -- the directory that holds <target>/ (see below)
    target "linux-dev"         -- the default TARGET
    cook "copy_assets {in} {out}"   -- what cooks assets (see Assets); the default is goldberg
    requires_kormake "1.2"     -- a kormake older than this, down to the minor version, is an error
    external "sqlite" {        -- an external the project defines, per platform (see Externals)
      linux = { pkg_config = "sqlite3", fetch = "sudo apt install libsqlite3-dev" },
    }

It is best to write `engine` relative to your development directory, so the file works in
any checkout that has the same layout. `examples/korvid.config` and `tools/korvid.config`
both say `engine ".."`. A game kept beside the engine (`~/code/mygame/` next to
`~/code/korvid/`, its modules in `~/code/mygame/<module>/`) says `engine "../korvid"` in
`~/code/mygame/korvid.config`. An engine checked out inside the game
(`mygame/korvid/`) is `engine "korvid"`. A
leading `~` means the home directory.

**Projects.** The module paths are the master file's, then the config's, then
`KORMAKE_MODULE_PATH`'s. Each directory is counted once, by its real path. If the config's
paths bring in no module that the master file's do not (the in-tree configs, or one at the
engine root that only sets a `target`), it shares everything with the engine root: the
same `build/`, `dist/` and Ninja files. If they do, it is a project of its own:

- its outputs go beside it, in `build/<target>/`, `build/compile_commands.json` and
  `dist/`, and nothing is written into the engine's tree;
- its own modules are the roots, with their tests, so Ninja's default builds the project
  rather than the engine;
- engine modules can still be named (`kormake arcfire` builds arcfire into the project's
  build directory).

`build` moves the build directory, for a project or in-tree. It may not be, or hold, the
config's directory or the engine root, since `clean` removes what kormake writes under it. The engine root is still
where the master file's paths, a toolchain's external `dir`s (such as `vendor/`) and
`%{root}` are resolved, and where `package` gets `LICENSE` and `NOTICE` from.

### kormake.lua

The master file, at the engine root. It runs top to bottom in a sandbox (only `string`,
`table`, `math`, `utf8` and the basic functions are available; there is no file, OS or
`require` access) and produces plain data.

    project "korvid"
    requires_kormake "1.1"                  -- a major version newer than this binary is an error
    modulepath { "modules", "examples", "tools" }
    platforms { "linux", "windows", "macos", "ios", "iossim", "android", "androidemu" }
    configs   { "debug", "dev", "ship" }    -- the first is the default config
    host_config "dev"                       -- what tools build in; the default is dev
    bind "render" { linux = "renderer-vulkan" }   -- overrides a toolchain's default binding
    override "audio" { path = "../audio-fork" }   -- this copy wins over any other of that name
    root "examples" { paths = "examples", platforms = { "linux" } }
    root "tests"    { modules = "*", tests = true, platforms = { "linux" } }
    cook "goldberg cook --roots {roots} --stamp {stamp} --quiet"   -- what cooks assets (see Assets); this is the default

A module path is a directory of module directories. A module is any directory directly
under a path entry that has a `module.lua`. kormake does not search deeper, and directories
without one (`tools/kormake`) are ignored. If two modules have the same name,
that is an error naming both paths, unless an `override` picks one.

A root's `modules` takes names or `*`/`?` globs, and defaults to the root's own name.
`paths` limits the root to the modules found in those module paths (relative to the root),
and then `modules` defaults to `*`.
`platforms` limits the platforms the root applies to. `tests = true` adds the tests of the
root's modules to `tests` and `check`. With no roots at all, every module is a root. A
module that is not in any root is still in the Ninja file if its platform supports it, and
it is built only when named.

### module.lua

There is one per module directory, holding exactly one declaration. It runs in the same
sandbox, and an unknown field is an error. The smallest file that works is a single line:

    module "core"

The module's name comes from this line, not from its directory's name.

**Directory convention.** These parts are used whenever they exist:

    include/korvid/<name>/   public headers, on the include path of the module and of everything that links it
    src/*.cc                 sources (only src/, not its subdirectories, except the ones below)
    src/<dir>/*.cc           sources for a platform: the toolchain's srcdirs, in its order (posix, linux, sdl3, ...)
    src/*.mm, src/<dir>/*.mm Objective-C++, picked up where a toolchain lists the directory
    src/*.c, src/<dir>/*.c   C, for a vendored library written in it: a unity file of its own, the toolchain's cc and cflags
    tests/*.cc               the module's test executable, with the module and its closure linked (an executable's tests
                             link its closure only and run after the executable is built: they start it, they do not contain it)
    tests/heavy/*.cc         the module's heavy test executable, linked the same way; run only by `kormake test --heavy`
    shaders/                 inputs for the tool rules the module uses (the shader rule takes shaders/*.slang)
    assets/                  cooked by goldberg into the output tree of every program that links the module (see Assets)

Vendored C code that is also C++ is compiled through a `.cc` file that includes it, as
yxml is. C that is not C++ (sqlite: `new` is a name in it) is compiled as C, through a
`.c` file of the module's own in `src/` that sets its options and includes it from
`src/vendor/` (which is not looked in): the toolchain's `cc`, `cflags` (per config too)
and `c` rule, and the module's `codegen` profiles, includes and defines. A toolchain
without them cannot build such a module, and says which file. Generated sources go in
the build directory, never in the source tree.

**Fields.**

| Field | Meaning |
| --- | --- |
| `output` | `"lib"` (the default) is a static library; `"exe"` is an executable; `"headers"` compiles nothing |
| `deps` | Modules this one links, and whose `include/` it sees. A dependency on an interface also links the backend bound for the platform |
| `provides` | The interface this module implements (`"render"`, `"shader-compile"`), or `"toolchain"` |
| `platforms` | The platforms the module builds for; the default is all of them. `"host"` builds it only for the machine running the build, whoever asks for it (`kpack`). Linking such a module is an error; name it in `tools` |
| `tools` | Tool modules or interfaces whose rules apply to this module's files. Each one is built for the host. `["shader-compile"] = { install = "shaders" }` writes the outputs to `bin/shaders/` instead of `gen/` |
| `exports` | The rules the module offers as a tool: `rule "name" { inputs, outputs, command, deps, per, platforms }` (see Tool rules) |
| `external` | System libraries by name and version: `{ sdl3 = ">=3.2" }`, or `{ sdl3 = { ">=3.2", platforms = { "linux" } } }`. The toolchain defines what each name means, or the project does (`externals`, below). The version is checked where pkg-config found the package, and is documentation otherwise |
| `externals` | Externals this module defines, per platform: `{ linux = { pipewire = { pkg_config = "libpipewire-0.3", fetch = "sudo apt install libpipewire-0.3-dev" } } }`. Any module may then name one in `external` (see Externals) |
| `sources` | Replaces the convention with this list of files, relative to the module |
| `defines` | Preprocessor definitions for the module's own sources and tests |
| `private_includes` | Directories under the module that are on the include path of its own sources and tests but not of the modules that link it (a vendored library that includes its own headers by `<name>`) |
| `unity` | `false` compiles each source on its own; this overrides `--no-unity` and `--unity` |
| `test_deps` | Modules the module's tests use and the module does not (`{ "image" }` to read reference pictures): their headers and libraries reach the test executable only. Each must be in the build for the platform |
| `codegen` | A code-generation profile by name (`"fast"`), or a list of them (`{ "fast", "wrapv" }`), as the toolchain's `codegen_profiles` spell them, for the module's own sources (not its tests). Code built so must share no inline function with the rest of the program (`tools/check-fast-objects.sh`) |
| `codegen_sources` | A profile for named sources only: `{ wrapv = { "src/flac.cc" } }`, paths from the module's directory. Each source named leaves the module's unity file and compiles alone, with the module's `codegen` and then the profile's flags; a name that is not one of the module's sources is an error. Every toolchain has `fast` and `wrapv` (signed arithmetic that wraps: `-fwrapv`, and on cl `/d2UndefIntOverflow-`) |
| `isa_sources` | Sources built again for an instruction set the toolchain names in `isa`: `{ avx2 = { "src/raster.cc" } }`. A copy is compiled with the set's flags and `KV_ISA_<SET>=1`; the module's other sources see `KV_HAS_ISA_<SET>=1` where the copy exists, and choose at run time |
| `engine_module` | The module registers a `ModuleDesc`. Each executable gets a generated `gen/<exe>/modules.gen.cc` listing every engine module in its closure, in dependency order |
| `module_list` | `false`: the executable writes its own module list |
| `exe` | The executable's file name, when it differs from the module's (`exe = "mygame"` for a module called `mygame.core`) |
| `licenses` | Files under the module that `package` copies into `licenses/` as `<module>-<file>` |
| `system` | A tool that is found on `PATH` rather than built (`slang`: `system = "slangc"`) |
| `prebuilt` | A tool binary vendored per host: `{ linux = "bin/linux/slangc" }` |

A toolchain module (`provides = "toolchain"`, one per platform) has fields of its own:
`tag`, `srcdirs`, `cxx`, `ar`, `link`, `cxxflags`, `ldflags`, `cc` and `cflags` (C, for
vendored libraries in it; optional), `configs` (flags per config: `cxxflags`, `cflags`,
`ldflags`), `rules` (`cxx`, `ar` and `link`, and `c` with `cc`, as Ninja commands), `obj_suffix`, `exe_suffix`,
`lib_file`, `flag_include`, `flag_include_system`, `flag_define`, `flag_lib`,
`flag_libdir`, `flag_framework`, `flags_generated` (added when compiling code another
program wrote: `-w`), `codegen_profiles` (the flags of each profile a module's `codegen`
or `codegen_sources` names; a profile with a `warn` string, which the toolchain cannot give
in full, is built with the flags it has and warns once a run where it is asked for, as
`toolchain-windows`' `wrapv` does), `isa` (the flags of each instruction set a module's `isa_sources` may be built for),
`sanitizers` (for each name `--sanitize` takes, its `cxxflags` and `ldflags`),
`externals`, `bindings` (the default backend for each
interface), and `probe` (`"msvc"` or `"xcode"`: a script that finds the compiler, the SDK
and Ninja). For a toolchain that serves several platforms there are also `per_platform`,
`triple`, `arch` (for platforms that differ in it: `"aarch64"`, `"x86_64"`), `family`, `sdk`,
`ndk` (the Android NDK version the `android` probe finds) and `os_min`, along with
`package` (`"app"`, `"iosapp"` or `"apk"`), `test_runner` and `test_jobs`. A toolchain
whose applications are libraries the OS loads (Android) says so with `app_module`,
`app_file` and `app_ldflags`, and packages them with `abi`, `build_tools` and `target_api`. `modules/toolchain-linux/module.lua` is the plain case.
Another module that sets one of these is an error, as is a `per_platform` section that
sets anything else.

**Externals.** An external is a library from outside the build, by name. What a name
means on a platform is defined in one of three places: the platform's toolchain
(`externals = { sdl3 = {...} }`), a module (`externals = { linux = { pipewire = {...} } }`),
or a `korvid.config` (`external "sqlite" { linux = {...} }`). The last two are how a
project links a library the engine does not use, without editing a toolchain. The names
are one namespace: one defined twice for a platform is an error that names both files,
so a project cannot redefine the engine's `sdl3`. A module's definition is not private
to it. `kormake graph` lists each instance's externals, a project's with its file.

An external lists `libs` to link, and Apple `frameworks`. For a package kept in a
directory (an SDK, an unpacked download), it also lists `dir` or `env` (a variable an
installer sets), along with `include`, `libdirs`, `runtime` (files copied beside each
executable that links it), `check` (the file that proves the package is there), and
`fetch` (instructions printed when the package is missing). A toolchain's `dir` is
relative to the engine root; a module's or a config's, to the directory of the file that
wrote it. An `include` or `libdirs` entry may be an absolute path, with or without a
`dir`, for a system package that has no directory of its own.

`pkg_config = "<package>"` asks pkg-config instead, at generation time, when the target's
platform is the machine kormake runs on. The `-I`, `-L` and `-l` it reports replace
`include`, `libdirs` and `libs`; its other flags (`-D_REENTRANT`, `-pthread`) are not
taken. The headers are system headers (`flag_include_system`), like a directory
external's. The version a module asks for (`">=0.3.60"`, `"==2"`, `"<=5"`) is checked
with it. A package it does not find is reported with the `fetch` line by `kormake check`
and before any compile. The explicit fields beside `pkg_config` are what is used when
there is no pkg-config on the machine or the target is another platform. The package's
`.pc` file is an input of the build file, so upgrading the package regenerates.

**Tool rules.** A tool exports `rule "name" { inputs, outputs, command, deps }`. A module
that names the tool in `tools` gets one Ninja edge for each of its own files that matches
`inputs`. For example, the shader rule maps `shaders/*.slang` to `%{stem}.spv`. A command
can use these placeholders:

- `%{tool}`, `%{in}`, `%{out}`, `%{depfile}`;
- `%{shader_includes}`: an `-I` for the `shaders/` directory of every module in the
  build, so a game's shader can include the renderer's `sprite_fragment.slangh` by name;
- `%{triple}`: the consuming toolchain's target triple.

`deps = "depfile"` reads a gcc-style depfile. `platforms = { apple = { ... } }` replaces
fields for one platform, or for one toolchain family, or for wherever a module is bound to
an interface (`["renderer-d3d12"] = { ... }`: the shader rule writes DXIL wherever that
backend is bound, whatever the platform); a bound module's variant wins. A rule with `per = "exe"` and
`inputs = "assets"` runs once per executable instead, after that executable's assets are
cooked, with the output tree as `%{assets}` and `%{exe}` available in `outputs`.
This is how kpack's pack rule writes `bin/<exe>.kpak`. A per-exe rule's `configs` names
the configs whose default build holds its outputs (kpack's names none: goldberg cooks
loose, in ship too, until packs are made from its graph); in the others they are built by
name (`ninja -C build/<target> bin/<exe>.kpak`).
A rule with `bound = "<module>"` applies only where that module is bound to an interface
on the consuming platform. A module declines a tool's rule by name with `skip_rules` in its
`tools` entry (`tools = { ["shader-compile"] = { skip_rules = { "shader_cpu" } } }`).

A rule can write code that is compiled into the module that has its inputs (slang's
`shader_cpu`: a game shader as C++ for renderer-cpu). Three fields shape it, each a template
with `%{in}` (the input's path), `%{out}` (the output's path), `%{stem}` and `%{symbol}` (the
module's and the stem's symbols, joined):

- `wrap`, a list of `{ match = "...", text = "..." }`: the file the tool reads instead of the
  input, written into the module's gen directory. It is the first entry whose `match` the input
  contains, or one without a `match`.
- `glue`: a C++ file written beside the output, which includes it. It is compiled into the
  module after the tool has run, with the toolchain's `flags_generated`. An executable's tests
  link these objects too.
- `engine_module`: the engine module the glue defines (`kv::<it>`), listed in each
  executable's generated module list after the module that owns it.
- `codegen`: the profile the glue is compiled with, as a module's `codegen`.

A rule with any other field is an error, as is a per-exe rule whose inputs are not
`"assets"`. The tool's
binary is an implicit input to every edge it runs.

### Assets

Nothing in kormake copies an asset. For every program (an executable, or a module's
tests) with an `assets/` directory in its closure, the graph has one edge that runs the
cook over those directories and into that program's output tree. By default the cook is
`goldberg cook` (tools/goldberg, built for the host). The edge runs at every build:
goldberg's own state decides what is out of date, and a cook with nothing to do costs a
scan. Its stamp changes only when the tree did, so nothing reruns for it.

`cook "..."` in `korvid.config` or the master file (the config's wins) names another, as one
command line. Its first word is the program: an executable module, built for the host and
built before the cook runs (`goldberg`); a script beside the file that names it, without
its extension (`copy_assets` is `copy_assets.bat` on Windows, `copy_assets.sh`, run with
`sh`, elsewhere); or a program on `PATH`. In the rest, these are filled in:

| Word | Is |
|---|---|
| `{roots}` | the roots file (below): the program, its tree, the state, every asset root |
| `{stamp}` | a file to rewrite only when the tree changed |
| `{tree}` | the program's output tree |
| `{program}` | the program's name |
| `{in}`, `{name}`, `{out}` | one asset root, its module's name, and `{tree}/{name}` |

A cook with `{in}`, `{name}` or `{out}` runs once per asset root. One that runs per root,
or names no `{stamp}`, is run by kormake (`kormake _cook`), which touches the stamp after
every run, so what reads the tree (a pack) is rebuilt every time. kormake's own test
fixture cooks with a tree copy, `cook "copy_assets {in} {out}"`, since it has no
goldberg: `tools/kormake/tests/fixture/copy_assets.sh` and `.bat` are a starting point.
Changing the cook of a built tree wants `kormake clean`: goldberg takes only a tree it
made.

    build/assets/<program>/<module>/...        every config but ship (dev and debug share it)
    dist/<program>/assets/<module>/...         ship, for a game that packages as a plain directory: cooked in place
    build/assets-ship/<program>/<module>/...   ship, for anything else: a test, a game packaged as a bundle or an .apk
    build/goldberg/<program>/<dev|ship>/       goldberg's state for that tree
    <target>/goldberg/<program>.roots          what the cook is told: the program, the tree, the state, each module's assets/
    <target>/goldberg/<program>.cook           the command lines kormake runs, for a cook it runs itself
    <target>/bin/<program>.assets              one line, the tree's path: how the program finds it (the platform module reads it)

A packaged program has no `.assets` file and reads `assets/` beside itself, which
`package` fills from the tree, module by module. `docs/PRDs/Goldberg-Master-PRD.md`.

### The build directory

Everything below is under `build/<target>/` (or `<build>/<target>/` when a config moves
it). Ninja runs there.

    build.ninja               the whole graph for the target; regenerated when kormake.lua or any module.lua changes, or a project's korvid.config
    compile_commands.json     every compile; build/compile_commands.json, beside the targets, is the last one written (where clangd looks)
    bin/<exe>                 executables, and the files they need beside them (SDL3.dll on Windows)
    bin/<module>_tests        test executables
    bin/<module>_heavy        heavy test executables (tests/heavy/), run only by kormake test --heavy
    bin/<program>.assets      where that program's cooked assets are (see Assets)
    goldberg/<program>.roots  what its cook is told; goldberg/<program>.cooked, the cook's stamp
    bin/shaders/              compiled shaders
    bin/<exe>.kpak            a pack (by name)
    obj/<module>/             objects
    lib/                      libraries (libkv_<module>.a, kv_<module>.lib)
    gen/<module>/             generated sources: unity.cc, modules.gen.cc, tool outputs that are not installed
    test/<module>.ok          a test's stamp
    host/...                  the same layout, for host instances that the target does not share

Ninja targets: `all` (the default: the roots, plus `tests`), `tests` (the roots' test
executables, heavy ones too), `check` (the roots' test stamps), `heavy/<module>` (a heavy
test and what it runs, built, not run), and one phony per module, named for the module
(`ninja -C build/linux-dev arcfire`).

## ENVIRONMENT

**KORMAKE_MODULE_PATH**
: More module paths, searched after the master file's and the config's. Entries are
  separated by `:`, or by `;` on Windows.

**KORMAKE_<NAME>_DIR**
: Where a toolchain's directory external is, for example `KORMAKE_SDL3_DIR` or
  `KORMAKE_VULKAN_DIR`. This takes priority over the external's `env` and `dir`.

**KORMAKE_PKG_CONFIG**
: The program asked about an external with `pkg_config`. The default is `pkg-config` on
  `PATH`.

**KORMAKE_SDKROOT**
: The Apple SDK to use instead of the one `xcrun` reports.

**KORMAKE_CODESIGN_IDENTITY**
: The identity `package` signs with on macOS and iOS. The default is ad hoc on macOS and
  the development identity on iOS.

**KORMAKE_IOS_TEST_BUNDLE_ID**, **KORMAKE_IOS_DEVICE**, **KORMAKE_IOS_PROFILE**
: Running tests on an iOS device and signing for it (`scripts/ios.lua`): the identifier
  every test installs as (one the profile covers; required), the device by name or
  identifier (the one physical device connected when unset), and the profile (the newest
  of Xcode's that covers the identifier and the device when unset). Variables named
  `DEVICECTL_CHILD_<NAME>` reach a test on the device as `<NAME>`.

**HOME**, **USERPROFILE**
: What a leading `~` in a path expands to.

**CC**
: The C compiler `bootstrap.sh` uses (the default is `cc`).

## EXIT STATUS

0 on success. Otherwise, the exit status of Ninja or of the test that failed, or 1 for an
error of kormake's own, which is printed as `kormake: <message>`.

## EXAMPLES

    kormake                              # at the root: every root, with its tests (linux-debug on Linux)
    kormake dev                          # the same in linux-dev
    kormake arcfire dev                  # one game and what it links
    cd examples/arcfire && kormake dev   # the same, from its directory
    kormake examples/ ship               # every example, with packs
    kormake kpack picture                # both tools, built for the host
    kormake test collision               # one module's tests
    kormake test linux-dev               # every test the roots ask for
    kormake test hifi --sanitize address,undefined   # a module's tests, and its closure, under ASan and UBSan
    kormake package arcfire ship         # dist/arcfire/
    kormake graph --why renderer-vulkan  # who pulls in the Vulkan renderer
    kormake graph arcfire --dot | dot -Tsvg > arcfire.svg
    kormake check windows-dev            # does the Windows graph resolve (on any host)
    kormake -C ~/code/mygame             # a project outside the engine, built from anywhere

The 1.0 forms still work: `kormake build linux-debug`, `kormake test linux-dev collision`,
`kormake package linux-ship arcfire`.

## NOTES

- The scripts are embedded in the binary. After pulling a change to
  `tools/kormake/scripts/`, run the bootstrap again. kormake warns when its embedded
  scripts differ from the ones under the engine root.
- Each target has one `build.ninja` that holds every module its platform supports. Naming
  modules changes what Ninja builds, never the file, so moving between directories does
  not cause regeneration.
- Ninja must be on `PATH`. The exception is Windows, where kormake finds Visual Studio's
  copy itself.

## SEE ALSO

`BUILDING.md` (bootstrap, internals, diagnosing a build), `docs/PRDs/Kormake-v1-Dev-PRD.md`,
`docs/PRDs/Kormake-1.1-Dev-PRD.md`, `docs/concepts/modules.md` (writing an engine module),
the root `README.md` (platform prerequisites), ninja(1).
