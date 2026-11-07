module "example_testbed" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "frameloop" },
}
