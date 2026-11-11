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
                "example_cpukiller", "example_arcfire", "example_gallery" },
  tests     = true,
  platforms = { "windows" },
}

-- macOS is slice 7 (docs/PRDs/Engine-Slice-7-PRD.md), brought up the same way: "render" is
-- renderer-metal there, and the root holds every module the windows root does, and
-- example_sockit2m.
root "macos" {
  modules   = { "core", "math", "inflate", "pack", "xml", "platform", "cli", "tool", "tool_kpack", "save",
                "bulletml", "platform_hid", "platform_audio", "render", "asset", "sprite", "scene", "text", "anim",
                "collision", "tilemap", "level", "audio", "ui", "frameloop", "renderer-metal",
                "example_sockit2m", "example_window", "example_testbed", "example_stones", "example_cpukiller",
                "example_rocks", "example_arcanum", "example_arcfire", "example_gallery" },
  tests     = true,
  platforms = { "macos" },
}
