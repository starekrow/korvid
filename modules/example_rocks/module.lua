module "example_rocks" {
  output = "exe",
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "collision", "frameloop" },
}
