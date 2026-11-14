-- A stress tool, not a game: no window, no renderer. Profiles the job pool and the
-- memory system under load, and the engine's tick rate across thread counts.
module "cpukiller" {
  output        = "exe",
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "frameloop" },
  engine_module = true,   -- kormake's list is the one with the window; src/core_modules.cc has the one without
}
