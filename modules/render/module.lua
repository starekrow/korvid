-- The render interface: the packet the outer engine writes, the API the game calls, and the
-- shaders every backend draws with. A backend (renderer-vulkan, renderer-metal) provides it;
-- the binding is per platform in the toolchains and kormake.lua. shaders/ is the engine's
-- sprite, sort and post shaders and the interface a game's shaders include by name
-- (sprite_fragment.slangh, post_fragment.slangh): one set of sources, compiled by the
-- bound shader compiler for the platform's backend (SPIR-V, or a Metal library), into
-- bin/shaders/. Header-only: it compiles no C++, but its shaders build like any module's.
module "render" {
  output = "headers",
  deps   = { "core", "math" },
  tools  = { ["shader-compile"] = { install = "shaders" } },
}
