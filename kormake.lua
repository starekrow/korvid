-- Korvid master file. Read top to bottom. kormake (tools/kormake) reads it, scans the
-- module paths, and writes build/<platform>-<config>/build.ninja.

project "korvid"
requires_kormake "1.0"

modulepath { "modules" }

platforms { "linux" }
configs   { "debug", "dev", "ship" }

-- Platform modules supply the defaults (toolchain.linux binds render and shader-compile);
-- a line here would override them for one platform.
-- bind "render" { linux = "renderer-vulkan" }

root "examples" { modules = "example_*" }
root "tests"    { modules = "*", tests = true }
