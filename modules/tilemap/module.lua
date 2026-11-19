-- Tiled maps (JSON) as assets in the asset module's scopes, drawn as cosmetic sprite rows
-- in layer order, with properties, objects and solids to query.
module "tilemap" {
  deps          = { "core", "math", "render", "asset", "asset_render", "sprite", "inflate", "pack", "anim", "collision" },
  test_deps     = { "platform" }, -- the tests mount a directory
  engine_module = true,
}
