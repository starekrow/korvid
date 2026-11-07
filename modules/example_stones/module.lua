module "example_stones" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "frameloop" },
}
