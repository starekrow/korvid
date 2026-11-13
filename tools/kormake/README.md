# kormake(1)

## NAME

kormake — build Korvid modules and the programs and tools made from them

## SYNOPSIS

    kormake [build] [NAME...] [TARGET] [OPTIONS]
    kormake test    [NAME...] [TARGET] [OPTIONS]
    kormake package NAME      [TARGET] [OPTIONS]
    kormake gen     [TARGET] [OPTIONS]
    kormake check   [TARGET] [OPTIONS]
    kormake graph   [NAME...] [TARGET] [--dot] [--why MODULE] [OPTIONS]
    kormake clean   [TARGET] [OPTIONS]
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
version's changes in `docs/PRDs/Kormake-1.1-Dev-PRD.md`. See `BUILDING.md` beside this
file for how to bootstrap kormake and how it works inside.

Each command line is made of a command, some module names, and one target. These can come
in any order, except that the command has to be the first word.

## COMMANDS

**build** (the default)
: Builds the named modules (see NAMES): each one's executable or library, everything it
  links, its staged assets and shaders, and in the ship config its pack (see Tool rules).
  A module's tests are not built. With no NAME and nothing chosen by the current directory, kormake builds
  Ninja's default, `all`: every root of the master file and the tests those roots ask for.

**test**
: Builds and runs the tests of the named modules. Each module's tests are one executable,
  `bin/<module>_tests`, built from `tests/*.cc`. A test passes when it exits 0, and kormake
  then writes a stamp, `test/<module>.ok`, so a test that passed is not run again until
  its executable, or a file staged for it (an asset, a shader), changes. Naming a module that has no tests is an error. When the modules
  come from a directory, the ones without tests are skipped. With nothing selected,
  kormake runs `check`: every test that the roots ask for.

**package**
: Builds one executable and puts it in `dist/<exe>/` together with what it needs at run
  time: its pack (or its loose assets, if it has no pack), its shaders, the repository's
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
  the tools it uses, and whether a root asks for its tests. With NAMEs (or a current
  directory that selects modules), it prints only those modules and what they reach.
  Without them it prints what the roots reach. `--dot` prints Graphviz input instead, with tool edges drawn dashed. `--why MODULE`
  prints the chain of instances that pulled MODULE into the graph.

**clean**
: Removes the target's build directory. With no TARGET, it removes every target's
  directory and `compile_commands.json`, and then the build directory itself if that
  leaves it empty. It removes nothing else it finds there. This command takes no NAMEs.

**help**, **--version**
: Print the usage summary, or the version (`kormake 1.1 (Lua 5.5)`).

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
    platforms { "linux", "windows", "macos", "ios", "iossim" }
    configs   { "debug", "dev", "ship" }    -- the first is the default config
    host_config "dev"                       -- what tools build in; the default is dev
    bind "render" { linux = "renderer-vulkan" }   -- overrides a toolchain's default binding
    override "audio" { path = "../audio-fork" }   -- this copy wins over any other of that name
    root "examples" { paths = "examples", platforms = { "linux" } }
    root "tests"    { modules = "*", tests = true, platforms = { "linux" } }

A module path is a directory of module directories. A module is any directory directly
under a path entry that has a `module.lua`. kormake does not search deeper, and directories
without one (`tools/kormake`, `tools/ios`) are ignored. If two modules have the same name,
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
    tests/*.cc               the module's test executable, with the module and its closure linked
    shaders/                 inputs for the tool rules the module uses (the shader rule takes shaders/*.slang)
    assets/                  copied to bin/assets/<module>/, with subdirectories and without hidden files

Vendored C code is compiled through a `.cc` file that includes it. Generated sources go in
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
| `external` | System libraries by name and version: `{ sdl3 = ">=3.2" }`, or `{ sdl3 = { ">=3.2", platforms = { "linux" } } }`. The toolchain defines what each name means |
| `sources` | Replaces the convention with this list of files, relative to the module |
| `defines` | Preprocessor definitions for the module's own sources and tests |
| `private_includes` | Directories under the module that are on the include path of its own sources and tests but not of the modules that link it (a vendored library that includes its own headers by `<name>`) |
| `unity` | `false` compiles each source on its own; this overrides `--no-unity` and `--unity` |
| `engine_module` | The module registers a `ModuleDesc`. Each executable gets a generated `gen/<exe>/modules.gen.cc` listing every engine module in its closure, in dependency order |
| `module_list` | `false`: the executable writes its own module list |
| `exe` | The executable's file name, when it differs from the module's (`exe = "mygame"` for a module called `mygame.core`) |
| `licenses` | Files under the module that `package` copies into `licenses/` as `<module>-<file>` |
| `system` | A tool that is found on `PATH` rather than built (`slang`: `system = "slangc"`) |
| `prebuilt` | A tool binary vendored per host: `{ linux = "bin/linux/slangc" }` |

A toolchain module (`provides = "toolchain"`, one per platform) has fields of its own:
`tag`, `srcdirs`, `cxx`, `ar`, `link`, `cxxflags`, `ldflags`, `configs` (flags per
config), `rules` (`cxx`, `ar` and `link`, as Ninja commands), `obj_suffix`, `exe_suffix`,
`lib_file`, `flag_include`, `flag_include_system`, `flag_define`, `flag_lib`,
`flag_libdir`, `flag_framework`, `externals`, `bindings` (the default backend for each
interface), and `probe` (`"msvc"` or `"xcode"`: a script that finds the compiler, the SDK
and Ninja). For a toolchain that serves several platforms there are also `per_platform`,
`triple`, `family`, `sdk` and `os_min`, along with `package` (`"app"` or `"iosapp"`),
`test_runner` and `test_jobs`. `modules/toolchain-linux/module.lua` is the plain case.
Another module that sets one of these is an error, as is a `per_platform` section that
sets anything else.

An external in a toolchain's `externals` lists `libs` to link, and Apple `frameworks`.
For a package kept in a directory (an SDK, an unpacked download), it also lists `dir`
(relative to the engine root) or `env` (a variable an installer sets), along with
`include`, `libdirs`, `runtime` (files copied beside each executable that links it),
`check` (the file that proves the package is there), and `fetch` (instructions printed
when the package is missing).

**Tool rules.** A tool exports `rule "name" { inputs, outputs, command, deps }`. A module
that names the tool in `tools` gets one Ninja edge for each of its own files that matches
`inputs`. For example, the shader rule maps `shaders/*.slang` to `%{stem}.spv`. A command
can use these placeholders:

- `%{tool}`, `%{in}`, `%{out}`, `%{depfile}`;
- `%{shader_includes}`: an `-I` for the `shaders/` directory of every module in the
  build, so a game's shader can include the renderer's `sprite_fragment.slangh` by name;
- `%{triple}`: the consuming toolchain's target triple.

`deps = "depfile"` reads a gcc-style depfile. `platforms = { apple = { ... } }` replaces
fields for one platform, or for one toolchain family. A rule with `per = "exe"` and
`inputs = "assets"` runs once per executable instead, over every asset that executable
stages, passed in a response file (`%{rsp}`) and with `%{exe}` available in `outputs`.
This is how kpack's pack rule writes `bin/<exe>.kpak`. A per-exe rule's `configs` names
the configs whose default build holds its outputs (kpack's says `{ "ship" }`); in the
others they are built by name, or by `package`, which ships them beside the executable.
A rule with any other field is an error, as is a per-exe rule whose inputs are not
`"assets"`. The tool's
binary is an implicit input to every edge it runs.

### The build directory

Everything below is under `build/<target>/` (or `<build>/<target>/` when a config moves
it). Ninja runs there.

    build.ninja               the whole graph for the target; regenerated when kormake.lua or any module.lua changes, or a project's korvid.config
    compile_commands.json     every compile; build/compile_commands.json, beside the targets, is the last one written (where clangd looks)
    bin/<exe>                 executables, and the files they need beside them (SDL3.dll on Windows)
    bin/<module>_tests        test executables
    bin/assets/<module>/      staged assets
    bin/shaders/              compiled shaders
    bin/<exe>.kpak            a pack (in ship by default; by name in other configs)
    obj/<module>/             objects
    lib/                      libraries (libkv_<module>.a, kv_<module>.lib)
    gen/<module>/             generated sources: unity.cc, modules.gen.cc, tool outputs that are not installed
    test/<module>.ok          a test's stamp
    host/...                  the same layout, for host instances that the target does not share

Ninja targets: `all` (the default: the roots, plus `tests`), `tests` (the roots' test
executables), `check` (the roots' test stamps), and one phony per module, named for the
module (`ninja -C build/linux-dev arcfire`).

## ENVIRONMENT

**KORMAKE_MODULE_PATH**
: More module paths, searched after the master file's and the config's. Entries are
  separated by `:`, or by `;` on Windows.

**KORMAKE_<NAME>_DIR**
: Where a toolchain's directory external is, for example `KORMAKE_SDL3_DIR` or
  `KORMAKE_VULKAN_DIR`. This takes priority over the external's `env` and `dir`.

**KORMAKE_SDKROOT**
: The Apple SDK to use instead of the one `xcrun` reports.

**KORMAKE_CODESIGN_IDENTITY**
: The identity `package` signs with on macOS and iOS. The default is ad hoc on macOS and
  the development identity on iOS.

**KORMAKE_IOS_TEST_BUNDLE_ID**, **KORMAKE_IOS_DEVICE**, **KORMAKE_IOS_PROFILE**
: Settings for running tests on an iOS device and signing for it (see `tools/ios/`).

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
`docs/PRDs/Kormake-1.1-Dev-PRD.md`, `modules/core/MODULES.md` (writing an engine module),
the root `README.md` (platform prerequisites), ninja(1).
