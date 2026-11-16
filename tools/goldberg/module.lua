-- goldberg: the asset orchestrator. Watches a game's asset roots, solves what each file
-- is and what is done with it from .berg rule files, and cooks them into an output tree.
-- An engine tool: C++ primitives here (src/), the application in Lua (lua/), read at
-- start from the source tree. docs/PRDs/Goldberg-Master-PRD.md.
module "goldberg" {
  output        = "exe",
  platforms     = { "host" },
  deps          = { "core", "platform", "cli", "tool", "lua", "script", "math", "kimb", "kim", "svg" },
  engine_module = true,
}
