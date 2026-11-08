# kormake

The build tool: one binary that discovers modules, resolves them into a graph of
(module, platform, config) instances, and writes one Ninja build plus
`compile_commands.json`. The model is in docs/PRDs/Kormake-v1-Dev-PRD.md: everything is a
module, including backends, build-time tools and platform toolchains.

    tools/kormake/bootstrap.sh                 # once: builds bin/<host>/kormake with the host C compiler
    tools/kormake/bin/linux/kormake build linux-debug   # resolve, write build/linux-debug/build.ninja, run ninja
    kormake build linux-dev                    # the dev config; ship likewise
    kormake build host                         # the machine kormake runs on, in the host config (host-debug: another)
    kormake gen linux-debug                    # write the Ninja file and compile_commands.json only
    kormake check                              # resolve only: cycles, bindings, duplicates
    kormake graph [--dot] [--why MODULE]       # the instance graph; --why shows what pulled a module in
    kormake test linux-debug [MODULE]          # build and run the tests
    kormake package linux-ship EXE             # dist/EXE/: the executable, its pack, shaders, licenses
    kormake clean [linux-debug]                # remove the build directory
    kormake --version

A target is `<platform>-<config>`. **`host`** is the machine kormake runs on: `host` alone
is the host platform in the master file's `host_config` (dev unless it says otherwise),
and `host-<config>` picks the config.

Flags after the target: `--no-unity` compiles every source on its own instead of one
unity file per module; `--cxx PATH` overrides the platform's compiler.

Layout: `src/kormake.c` is the host (a Lua state, the embedded scripts, and the file
system calls the scripts need), `scripts/*.lua` are kormake itself, `vendor/lua/` is Lua
5.5.1 unchanged (see `vendor/UPGRADE.md`). Nothing else is required beyond a C compiler
to bootstrap and Ninja on the path to build.

Module files (`module.lua`) and the master file (`kormake.lua`) run in a sandbox with no
file, OS or `require` access; their output is plain data. The fields a module may set:
`output` (lib, exe, headers), `deps`, `provides`, `platforms` (`"host"`: see below), `external`, `tools`,
`exports` (rules), `sources`, `defines`, `prebuilt`, `system`, `bindings`, `unity`,
`engine_module` (the module registers a `ModuleDesc`; the build then generates the
executable's module list in dependency order), `module_list = false` (the executable
writes its own list), `exe` (the executable's file name, when it is not the module's:
`tool_kpack` writes `bin/kpack`) and `licenses` (files under the module that `kormake
package` copies into `dist/<exe>/licenses/`).

**Assets.** Everything under a module's `assets/`, subdirectories included, is copied to
`bin/assets/<module>/` with the same layout (hidden files skipped).

**Tool rules.** A tool module exports `rule "name" { inputs, outputs, command, deps }`; a
module naming the tool in `tools` gets an edge per file of its own that matches `inputs`
(the shader rule: `shaders/*.slang` to `%{stem}.spv`). A rule with `per = "exe"` and
`inputs = "assets"` runs once per executable instead, over every asset file the
executable's closure stages under `bin/assets/`, passed through a Ninja response file
(`%{rsp}`), with `%{exe}` in `outputs` (kpack's pack rule: `bin/<exe>.kpak`). Per-exe
outputs are in the default build in the ship config and buildable by name in the others.

**Host builds.** Tools run on the machine doing the build, so a tool edge always resolves
to a host instance: the tool and its whole closure, built for the host platform in the host
config with the host toolchain's compiler and flags. A module with `platforms = { "host" }`
(`tool_kpack`) is built only that way, whoever asks: as a root of any build its host
supports, and as a tool; linking it from a target module is an error (name it in `tools`).
When the host is the target (`linux-dev` on Linux) host and target share instances and
outputs. When it is not (`linux-ship`, `linux-debug`, a cross build), the host instances are
a second set in the same Ninja graph, under `host/` in the build directory
(`host/bin/kpack`, `host/obj/`, `host/lib/`), and a tool rule's command runs the host
binary. `--cxx` applies to the target only. After writing the Ninja file kormake prints
the host tools the build runs and the system libraries each links, so a tool that pulls in
SDL or a GPU says so. Tools may link anything; keeping one lean is a matter of its deps
(`platform` links no external library; `platform_hid` and `platform_audio` do).
