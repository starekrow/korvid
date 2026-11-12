module "rocks" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "scene", "text", "anim", "audio", "collision", "frameloop" },
  tools  = { "kpack" }, -- the ship build packs the staged assets into bin/rocks.kpak
}
