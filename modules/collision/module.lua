-- Outer engine: the Collider2 component and the broadphase that turns overlaps between
-- entities of any kind into OverlapBegin and OverlapEnd events.
module "collision" {
  deps          = { "core", "math", "render", "sprite" },
  engine_module = true,
}
