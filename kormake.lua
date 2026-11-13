-- Korvid master file. Read top to bottom. kormake (tools/kormake) reads it, scans the
-- module paths, and writes build/<platform>-<config>/build.ninja.

project "korvid"
requires_kormake "1.1"

-- The engine, the examples, the tools. Each of examples/ and tools/ has a korvid.config
-- that points back here, so kormake works from inside them.
modulepath { "modules", "examples", "tools" }

platforms { "linux", "windows", "macos", "ios", "iossim" }
configs   { "debug", "dev", "ship" }

-- Platform modules supply the defaults (toolchain.linux binds render and shader-compile);
-- a line here would override them for one platform.
-- bind "render" { linux = "renderer-vulkan" }

-- The d3d12 branch: Windows draws with Direct3D 12. Drop this line (or name renderer-vulkan)
-- for the toolchain's default, Vulkan; the shaders follow the binding (slang's rule).
bind "render" { windows = "renderer-d3d12" }

root "examples" { paths = "examples", platforms = { "linux" } }
root "tests"    { modules = "*", tests = true, platforms = { "linux" } }

-- Windows is being brought up a module at a time: these build, with their tests. Each
-- module that joins the list here leaves it when the roots above cover Windows too.
root "windows" {
  modules   = { "core", "math", "inflate", "pack", "platform", "platform_hid", "cli", "tool", "kpack",
                "render", "asset", "sprite", "scene", "text", "anim", "collision", "tilemap", "level",
                "platform_audio", "audio", "renderer-vulkan", "renderer-d3d12", "touchpad", "frameloop", "save", "ui", "xml", "bulletml", "image", "picture",
                "window", "testbed", "stones", "rocks", "arcanum",
                "cpukiller", "arcfire", "gallery", "sockit2m" },
  tests     = true,
  platforms = { "windows" },
}

-- macOS is slice 7 (docs/PRDs/Engine-Slice-7-PRD.md), brought up the same way: "render" is
-- renderer-metal there, and the root holds every module the windows root does, and
-- sockit2m.
root "macos" {
  modules   = { "lua", "core", "math", "inflate", "image", "pack", "xml", "platform", "cli", "tool", "kpack", "picture", "save",
                "bulletml", "platform_hid", "platform_audio", "render", "asset", "sprite", "scene", "text", "anim",
                "collision", "tilemap", "level", "audio", "ui", "touchpad", "frameloop", "renderer-metal",
                "sockit2m", "window", "testbed", "stones", "cpukiller",
                "rocks", "arcanum", "arcfire", "gallery" },
  tests     = true,
  platforms = { "macos" },
}

-- iOS and its Simulator (slice 7, milestones 7 and 8): native UIKit, Core Audio and Metal,
-- no SDL. The modules with their tests, which run in the booted Simulator (kormake test
-- iossim-dev) or on a device (kormake test ios-dev), and the games, which `kormake package
-- ios-dev <game>` makes apps of. The Simulator cannot draw them (no indirect draws).
root "ios" {
  modules   = { "lua", "tool", "core", "math", "inflate", "image", "pack", "xml", "platform", "cli", "save", "bulletml",
                "platform_hid", "platform_audio", "render", "asset", "sprite", "scene", "text", "anim",
                "collision", "tilemap", "level", "audio", "ui", "touchpad", "frameloop", "renderer-metal",
                "window", "testbed", "stones", "rocks", "arcanum",
                "arcfire", "gallery" },
  tests     = true,
  platforms = { "ios", "iossim" },
}
