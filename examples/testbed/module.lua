module "testbed" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "frameloop" },
  licenses = { "assets/CREDITS.md", "assets/LICENSE-CC0.txt" },
}
