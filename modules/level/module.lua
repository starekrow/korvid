-- Levels: a catalog of places, each loaded into an asset scope behind a fade with the sim
-- held, built by the game under a root entity, and torn down with it. Reload reads every
-- file again.
module "level" {
  deps          = { "core", "math", "platform", "render", "asset", "sprite", "scene", "save" },
  test_deps     = { "cli" }, -- the tests drive the level commands through the CLI
  engine_module = true,
}
