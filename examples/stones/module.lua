module "stones" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_render", "sprite", "frameloop" },
  licenses = { "assets/CREDITS.md", "assets/LICENSE-CC0.txt" },
}
