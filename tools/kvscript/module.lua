-- kvscript: Lua 5.5 with the engine's modules as libraries, for the tools that make a
-- game's assets: images (PNG and PPM, crops, composites, resampling, text), zips, files.
-- `kormake run SCRIPT [ARGS...]` builds it for the host and runs the script with it. An
-- engine tool, as kpack and picture are, built for the host. Lua is kormake's own copy
-- (tools/kormake/vendor/lua), compiled here as C++; stb_truetype is the asset module's.
-- Not a unity build: Lua's sources define short macros that must stay in their file.
module "kvscript" {
  output           = "exe",
  platforms        = { "host" },
  deps             = { "core", "platform", "cli", "tool", "inflate", "image", "pack", "math" },
  engine_module    = true,
  unity            = false,
  private_includes = { "../kormake/vendor/lua", "../../modules/asset/src/vendor" },
}
