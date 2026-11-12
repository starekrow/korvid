-- A stress tool, not a game: no window, no renderer. Profiles the job pool and the
-- memory system under load, and the engine's tick rate across thread counts.
module "cpukiller" {
  output        = "exe",
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "sprite", "frameloop" },
  engine_module = true,
  module_list   = false,   -- it carries two lists of its own: with the window and without
}
