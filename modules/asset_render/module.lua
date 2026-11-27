-- asset_render: the asset kinds that are the renderer's. Textures (PNG decoded, uploaded) and
-- materials (JSON files as renderer rows) as asset kinds registered with the asset module, and
-- the renderer textures of the asset module's glyph pages. A program that draws links it; a
-- tool that only reads files links asset alone and no renderer.
module "asset_render" {
  deps          = { "core", "platform", "math", "render", "asset", "image" },
  engine_module = true,
  test_deps     = { "cli" },
  test_files    = { "modules/dejavu/assets/DejaVuSans.ttf" },
}
