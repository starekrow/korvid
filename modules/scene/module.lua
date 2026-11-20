-- Outer engine: the transform hierarchy. Parent and Local2 make an entity in any ktable a
-- child of any entity with a Transform2; "scene.compose" writes the children's world
-- transforms in depth order at the end of every tick, before the sprite extract.
module "scene" {
  deps          = { "core", "math", "render", "sprite" },
  engine_module = true,
}
