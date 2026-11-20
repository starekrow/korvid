-- Outer engine: text drawn as sprites. A Text component on any ktable with a Transform2 is
-- laid out as glyph sprites, children of the label through the scene module, from a font
-- asset's atlas.
module "sprite_text" {
  deps          = { "core", "math", "render", "asset", "asset_render", "sprite", "scene", "typeset" },
  engine_module = true,
}
