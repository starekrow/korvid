-- A JRPG skeleton on Korvid: two Tiled maps from CC0 art, a player under keyboard or pad
-- control who collides and sorts against trees, houses and villagers, pushes rocks, loses
-- hearts to villagers, exits between the maps, a pause menu with save, load, a level
-- select and the credits, and music that crossfades.
module "arcanum" {
  output        = "exe",
  engine_module = true,
  deps  = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_render", "sprite", "scene", "text", "audio",
            "anim", "collision", "level", "tilemap", "save", "ui", "cli", "frameloop" },
  tools = { "kpack" }, -- the ship build packs the staged assets into bin/arcanum.kpak
  licenses = { "assets/LICENSE-NinjaAdventure.txt", "assets/CREDITS.md" },
}
