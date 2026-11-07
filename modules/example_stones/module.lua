module "example_stones" {
  output = "exe",
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "frameloop" },
}
