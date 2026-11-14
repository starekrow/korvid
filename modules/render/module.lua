-- The render interface: the packet the outer engine writes, the API the game calls, and the
-- shaders every backend draws with. A backend (renderer-vulkan, renderer-metal) provides it;
-- the binding is per platform in the toolchains and kormake.lua. shaders/ is the engine's
-- sprite, sort and post shaders and the interface a game's shaders include by name
-- (sprite_fragment.slangh, post_fragment.slangh): one set of sources, compiled by the
-- bound shader compiler for the platform's backend (SPIR-V, or a Metal library), into
-- bin/shaders/.
--
-- It also holds what the backends share (docs/PRDs/Engine-Slice-9-PRD.md). src/ is the
-- linked half: the render world (korvid/render/world.h), which knows no GPU, links with any
-- backend and is tested here without one. include/korvid/render/core/ is the compiled-in
-- half: sources (.inl) that each backend builds against its own types, through its
-- src/render_core.cc.
module "render" {
  deps   = { "core", "math" },
  tools  = { ["shader-compile"] = { install = "shaders" } },
}
