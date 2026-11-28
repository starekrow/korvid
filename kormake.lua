-- Korvid master file. Read top to bottom. kormake (tools/kormake) reads it, scans the
-- module paths, and writes build/<platform>-<config>/build.ninja.

project "korvid"
requires_kormake "1.3"

-- The engine, the examples, the tools. Each of examples/ and tools/ has a korvid.config
-- that points back here, so kormake works from inside them. modules/ is public: a game
-- beside the engine builds on it. examples/ and tools/ are the engine's own: seen from
-- inside this tree, and hidden from a project of its own (whose modules may then take any
-- of their names), all but the cook's program, goldberg.
modulepath { "modules" }
modulepath { "examples", "tools", private = true }

platforms { "linux", "windows", "macos", "ios", "iossim", "android", "androidemu" }
configs   { "debug", "dev", "ship" }

-- Platform modules supply the defaults (toolchain.linux binds render and shader-compile);
-- a line here would override them for one platform.
-- bind "render" { linux = "renderer-vulkan" }

-- Windows draws with Direct3D 12 (renderer-vulkan for the toolchain's default, Vulkan; the
-- shaders follow the binding, slang's rule). On every platform, the CPU renderer is linked
-- beside the GPU backend as its fallback (on iOS, it is what the Simulator's GPU draws with) (docs/PRDs/CPU-Renderer-PRD.md): render's dispatcher takes
-- the first in the list that can draw here, so the GPU comes first.
bind "render" { linux = { "renderer-vulkan", "renderer-cpu" }, windows = { "renderer-d3d12", "renderer-cpu" }, macos = { "renderer-metal", "renderer-cpu" },
                ios = { "renderer-metal", "renderer-cpu" }, iossim = { "renderer-metal", "renderer-cpu" },
                android = { "renderer-vulkan", "renderer-cpu" }, androidemu = { "renderer-vulkan", "renderer-cpu" } }

root "examples" { paths = "examples", platforms = { "linux" } }
root "tests"    { modules = "*", tests = true, platforms = { "linux" } }

-- Windows is being brought up a module at a time: these build, with their tests. Each
-- module that joins the list here leaves it when the roots above cover Windows too.
root "windows" {
  modules   = { "core", "math", "inflate", "pack", "platform", "platform_hid", "cli", "tool", "kpack",
                "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "anim", "collision", "tilemap", "level",
                "platform_audio", "audio", "renderer-vulkan", "renderer-d3d12", "touchpad", "frameloop", "save", "sprite_ui", "xml", "bulletml", "image", "kvimagetool", "kbor", "kimb", "kim", "kimc",
                "window", "testbed", "stones", "rocks", "arcanum",
                "cpukiller", "arcfire", "gallery", "sockit2m", "lua", "script", "goldberg",
                "truetype", "dejavu", "typeset", "krome", "kromedemo", "kromescreens", "renderer-cpu", "renderer-cpu-raster", "patherizer" },
  tests     = true,
  platforms = { "windows" },
}

-- macOS is slice 7 (docs/PRDs/Engine-Slice-7-PRD.md), brought up the same way: "render" is
-- renderer-metal there, and the root holds every module the windows root does, and
-- sockit2m.
root "macos" {
  modules   = { "lua", "core", "math", "inflate", "image", "pack", "xml", "platform", "cli", "tool", "kpack", "kvimagetool", "kbor", "kimb", "kim", "kimc", "save",
                "bulletml", "platform_hid", "platform_audio", "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "anim",
                "collision", "tilemap", "level", "audio", "sprite_ui", "touchpad", "frameloop", "renderer-metal", "renderer-cpu", "renderer-cpu-raster",
                "sockit2m", "window", "testbed", "stones", "cpukiller",
                "rocks", "arcanum", "arcfire", "gallery", "script", "truetype", "dejavu", "typeset", "krome", "goldberg", "kromedemo", "kromescreens", "patherizer" },
  tests     = true,
  platforms = { "macos" },
}

-- iOS and its Simulator (slice 7, milestones 7 and 8): native UIKit, Core Audio and Metal,
-- no SDL. The modules with their tests, which run in the booted Simulator (kormake test
-- iossim-dev) or on a device (kormake test ios-dev), and the games, which `kormake package
-- ios-dev <game>` makes apps of. The Simulator cannot draw them (no indirect draws).
root "ios" {
  modules   = { "lua", "tool", "core", "math", "inflate", "image", "pack", "xml", "platform", "cli", "save", "bulletml",
                "platform_hid", "platform_audio", "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "anim",
                "collision", "tilemap", "level", "audio", "sprite_ui", "touchpad", "frameloop", "renderer-metal", "renderer-cpu", "renderer-cpu-raster",
                "window", "testbed", "stones", "rocks", "arcanum",
                "arcfire", "gallery", "script", "truetype", "dejavu", "typeset", "kim", "kimb", "krome", "kromedemo", "kromescreens" },
  tests     = true,
  platforms = { "ios", "iossim" },
}

-- Android is slice 8 (docs/PRDs/Engine-Slice-8-PRD.md): a phone's arm64 (`android`) and the
-- emulator's x86-64 (`androidemu`), native as iOS is: the modules the iOS root holds, with
-- their tests, which run inside the running emulator (kormake test androidemu-dev).
root "android" {
  modules   = { "lua", "tool", "core", "math", "inflate", "image", "pack", "xml", "platform", "cli", "save", "bulletml",
                "platform_hid", "window",
                "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "touchpad", "frameloop", "renderer-vulkan",
                "platform_audio", "audio", "anim", "collision", "tilemap", "level", "sprite_ui",
                "testbed", "gallery", "stones", "rocks", "arcanum", "arcfire", "cpukiller", "sockit2m",
                "renderer-cpu", "renderer-cpu-raster", "script", "truetype", "dejavu", "typeset", "kim", "kimb", "patherizer",
                "krome", "kromedemo", "kromescreens" },
  tests     = true,
  platforms = { "android", "androidemu" },
}
