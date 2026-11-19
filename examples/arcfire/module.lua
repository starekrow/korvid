-- A vertical bullet-hell shooter on Korvid, and the engine's stress test that is also a
-- game: parallax planes over a 3:4 field, a ship and its Spread, formations and a turret
-- from a stage script keyed to scroll distance, aimed bullets, grazes, the egg (a death in
-- slow motion), a HUD in side panels; every count a dial, and three ways for it to fly
-- itself (--demo, --autoplay, --stress).
module "arcfire" {
  output        = "exe",
  engine_module = true,
  deps  = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_render", "asset_live", "sprite", "scene", "text", "audio",
            "collision", "ui", "cli", "frameloop", "xml", "bulletml", "anim", "tilemap", "save" },
  -- kpack packs the staged assets into bin/arcfire.kpak in the ship build; the shader
  -- rule compiles shaders/*.slang into bin/shaders/ (the game's fragment shaders).
  tools = { "kpack", ["shader-compile"] = { install = "shaders" } },
  licenses = { "assets/LICENSE-CC0.txt", "assets/LICENSE-Kenney.txt", "assets/LICENSE-ScreamingBrainStudios.txt",
               "assets/LICENSE-Kenney-SciFiSounds.txt", "assets/LICENSE-Kenney-ImpactSounds.txt", "assets/LICENSE-JuhaniJunkala.txt",
               "assets/LICENSE-Kenney-SpaceShooter.txt", "assets/LICENSE-Kenney-SpaceShooterExtension.txt", "assets/CREDITS.md" },
}
