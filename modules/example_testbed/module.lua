module "example_testbed" {
  output = "exe",
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "frameloop" },
}
