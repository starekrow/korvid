-- kvscript: Lua 5.5 with the engine's modules as libraries, for the tools that make a
-- game's assets: images (PNG and PPM, crops, composites, resampling, text), zips, files.
-- `kormake run SCRIPT [ARGS...]` builds it for the host and runs the script with it. An
-- engine tool, as kpack and kvimagetool are, built for the host. Lua (modules/lua) and
-- stb_truetype (modules/truetype) are engine modules of their own.
module "kvscript" {
  output           = "exe",
  platforms        = { "host" },
  deps             = { "core", "platform", "cli", "tool", "inflate", "image", "pack", "math", "lua", "truetype", "script", "patherizer", "svg" },
  engine_module    = true,
}
