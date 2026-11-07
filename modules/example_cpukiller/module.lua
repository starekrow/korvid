-- A stress tool, not a game: no window, no renderer. Profiles the job pool and the
-- memory system under load, and the engine's tick rate across thread counts.
module "example_cpukiller" {
  output = "exe",
  deps   = { "core" },
}
