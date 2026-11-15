-- Krome's demo screen (docs/PRDs/Krome-Slice-1-PRD.md, Krome-Slice-2-PRD.md): a form in markup
-- out of krome/std (and the same form in Lua, --lua), laid out
-- with rows and columns, drawn on the UI primitive; a menu, a modal, a scrolled list.
-- --demo drives the pointer through it and checks what came of it.
module "kromedemo" {
  output        = "exe",
  engine_module = true,
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "krome", "frameloop" },
}
