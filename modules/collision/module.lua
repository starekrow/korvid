-- Outer engine: colliders, bodies that move by intent and are resolved in the same tick
-- against each other and a static world, and overlaps between entities of any ktable as
-- OverlapBegin and OverlapEnd events.
module "collision" {
  deps          = { "core", "math", "render", "sprite", "scene" },
  engine_module = true,
}
