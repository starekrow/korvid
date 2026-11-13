-- TrueType fonts: stb_truetype (vendor/, unchanged) compiled once, with the engine's
-- allocator and kv's math, so a glyph rasterizes to the same pixels on every platform.
-- The asset module's font assets and kvscript's kv.font use it; so may a game.
module "truetype" {
  deps = { "core", "math" },
}
