module "example_testbed" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "frameloop" },
}
