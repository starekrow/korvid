-- Korvid master file. Read top to bottom. kormake (tools/kormake) reads it, scans the
-- module paths, and writes build/<platform>-<config>/build.ninja.

project "korvid"
requires_kormake "1.0"

modulepath { "modules" }

platforms { "linux", "windows", "macos" }
configs   { "debug", "dev", "ship" }

-- Platform modules supply the defaults (toolchain.linux binds render and shader-compile);
-- a line here would override them for one platform.
-- bind "render" { linux = "renderer-vulkan" }

root "examples" { modules = "example_*", platforms = { "linux" } }
root "tests"    { modules = "*", tests = true, platforms = { "linux" } }

-- Windows is being brought up a module at a time: these build, with their tests. Each
-- module that joins the list here leaves it when the roots above cover Windows too.
root "windows" {
  modules   = { "core", "math", "inflate", "pack", "platform", "platform_hid", "cli", "tool", "tool_kpack",
                "render", "asset", "sprite", "scene", "text", "anim", "collision", "tilemap", "level",
                "platform_audio", "audio", "renderer-vulkan", "frameloop", "save", "ui", "xml", "bulletml",
                "example_window", "example_testbed", "example_stones", "example_rocks", "example_arcanum",
                "example_cpukiller", "example_arcfire" },
  tests     = true,
  platforms = { "windows" },
}

-- macOS is slice 7 (docs/PRDs/Engine-Slice-7-PRD.md), brought up the same way. Milestone 1:
-- everything that does not depend on "render", which has no backend here until the Metal
-- renderer lands. The rest join as their milestones do.
root "macos" {
  modules   = { "core", "math", "inflate", "pack", "xml", "platform", "cli", "tool", "tool_kpack", "save",
                "bulletml", "platform_hid", "platform_audio", "example_sockit2m", "example_window" },
  tests     = true,
  platforms = { "macos" },
}
