-- Krome's first screen (docs/PRDs/Krome-Slice-1-PRD.md): a form written in Lua, laid out
-- with rows and columns, drawn on the UI primitive; a menu, a modal, a scrolled list.
-- --demo drives the pointer through it and checks what came of it.
module "kromedemo" {
  output        = "exe",
  engine_module = true,
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "krome", "frameloop" },
}
