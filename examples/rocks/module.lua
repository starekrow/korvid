module "rocks" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "anim", "audio", "collision", "frameloop" },
  tools  = { "kpack" }, -- the ship build packs the staged assets into bin/rocks.kpak
  licenses = { "assets/CREDITS.md", "assets/LICENSE-CC0.txt", "assets/LICENSE-Kenney-SpaceShooter.txt" },
}
