module "example_rocks" {
  output        = "exe",
  engine_module = true,
  deps   = { "core", "platform", "math", "render", "asset", "sprite", "scene", "text", "anim", "audio", "collision", "frameloop" },
  tools  = { "kpack" }, -- the ship build packs the staged assets into bin/example_rocks.kpak
}
