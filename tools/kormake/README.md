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
executable's module list in dependency order) and `module_list = false` (the executable
writes its own list).
