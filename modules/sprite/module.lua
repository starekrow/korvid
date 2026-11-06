-- Outer engine: sprite entities (Transform2 + Sprite), their world, and the extract that
-- turns them into render packets. Never includes a graphics API.
module "sprite" {
  deps = { "core", "math", "render" },
}
