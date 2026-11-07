module "example_rocks" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "collision", "frameloop" },
}
