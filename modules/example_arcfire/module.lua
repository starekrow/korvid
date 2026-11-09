-- A vertical bullet-hell shooter on Korvid, and the engine's stress test that is also a
-- game: parallax planes over a 3:4 field, a ship and its Spread, formations and a turret
-- from a stage script keyed to scroll distance, aimed bullets, grazes, the egg (a death in
-- slow motion), a HUD in side panels; every count a dial, and three ways for it to fly
-- itself (--demo, --autoplay, --stress).
module "example_arcfire" {
  output        = "exe",
  engine_module = true,
  deps  = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "scene", "text", "audio",
            "collision", "ui", "cli", "frameloop" },
  tools = { "tool_kpack" }, -- the ship build packs the staged assets into bin/example_arcfire.kpak
  licenses = { "assets/LICENSE-CC0.txt", "assets/LICENSE-Kenney.txt", "assets/LICENSE-ScreamingBrainStudios.txt", "assets/CREDITS.md" },
}
