-- Tiled maps (JSON) as assets in the asset module's scopes, drawn as cosmetic sprite rows
-- in layer order, with properties, objects and solids to query.
module "tilemap" {
  deps          = { "core", "math", "platform", "render", "asset", "sprite", "inflate", "pack" },
  engine_module = true,
}
