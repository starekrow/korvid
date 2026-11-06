# Korvid

Step 1: HostWindow and a minimally bootstrapped Core. No renderer yet.

Modules follow the kormake directory convention (`module.lua`, `include/korvid/<name>/`,
`src/`, `tests/`). `build.ninja` is a hand-written stand-in until kormake exists.

    ninja          # builds build/bin/hello and the module test executables
    ninja check    # runs the tests
    build/bin/hello                 # opens a window; Esc or close quits
    build/bin/hello --run-seconds 2 # exits by itself (smoke test)

Needs g++ (or clang++), ninja and libsdl3-dev.
