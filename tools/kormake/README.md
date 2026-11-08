# kormake

The build tool: one binary that discovers modules, resolves them into a graph of
(module, platform, config) instances, and writes one Ninja build plus
`compile_commands.json`. The model is in docs/PRDs/Kormake-v1-Dev-PRD.md: everything is a
module, including backends, build-time tools and platform toolchains.

    tools/kormake/bootstrap.sh                 # once: builds bin/<host>/kormake with the host C compiler
    tools/kormake/bin/linux/kormake build linux-debug   # resolve, write build/linux-debug/build.ninja, run ninja
    kormake build linux-dev                    # the dev config; ship likewise
    kormake gen linux-debug                    # write the Ninja file and compile_commands.json only
    kormake check                              # resolve only: cycles, bindings, duplicates
    kormake graph [--dot] [--why MODULE]       # the instance graph; --why shows what pulled a module in
    kormake test linux-debug [MODULE]          # build and run the tests
    kormake package linux-ship EXE             # dist/EXE/: the executable, its pack, shaders, licenses
    kormake clean [linux-debug]                # remove the build directory
    kormake --version

Flags after the target: `--no-unity` compiles every source on its own instead of one
unity file per module; `--cxx PATH` overrides the platform's compiler.

Layout: `src/kormake.c` is the host (a Lua state, the embedded scripts, and the file
system calls the scripts need), `scripts/*.lua` are kormake itself, `vendor/lua/` is Lua
5.5.1 unchanged (see `vendor/UPGRADE.md`). Nothing else is required beyond a C compiler
to bootstrap and Ninja on the path to build.

Module files (`module.lua`) and the master file (`kormake.lua`) run in a sandbox with no
file, OS or `require` access; their output is plain data. The fields a module may set:
`output` (lib, exe, headers), `deps`, `provides`, `platforms`, `external`, `tools`,
`exports` (rules), `sources`, `defines`, `prebuilt`, `system`, `bindings`, `unity`,
`engine_module` (the module registers a `ModuleDesc`; the build then generates the
executable's module list in dependency order), `module_list = false` (the executable
writes its own list) and `licenses` (files under the module that `kormake package` copies
into `dist/<exe>/licenses/`).

**Assets.** Everything under a module's `assets/`, subdirectories included, is copied to
`bin/assets/<module>/` with the same layout (hidden files skipped).

**Tool rules.** A tool module exports `rule "name" { inputs, outputs, command, deps }`; a
module naming the tool in `tools` gets an edge per file of its own that matches `inputs`
(the shader rule: `shaders/*.slang` to `%{stem}.spv`). A rule with `per = "exe"` and
`inputs = "assets"` runs once per executable instead, over every asset file the
executable's closure stages under `bin/assets/`, passed through a Ninja response file
(`%{rsp}`), with `%{exe}` in `outputs` (kpack's pack rule: `bin/<exe>.kpak`). Per-exe
outputs are in the default build in the ship config and buildable by name in the others.
A tool built from source is the target config's own instance on a native build (the
command runs `bin/<tool>`); cross builds of source tools are not supported yet.
