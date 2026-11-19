-- Outer engine: text. A Text component on any kind with a Transform2 is laid out as glyph
-- sprites, children of the label through the scene module, from a font asset's atlas.
-- Ships DejaVu Sans as the default font (assets/, with its license).
module "text" {
  deps          = { "core", "math", "render", "asset", "sprite", "scene", "typeset" },
  engine_module = true,
  licenses = { "assets/DejaVuSans-LICENSE.txt" },
}
