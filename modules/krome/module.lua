-- Krome: UI for tools and dialogs (docs/design/krome.md, docs/PRDs/Krome-Master-PRD.md).
-- Screens are trees of four kernel node types laid out with rows, columns and stacks,
-- drawn on the renderer's UI primitive, wired up in a Lua state of Krome's own.
module "krome" {
  deps          = { "core", "math", "platform", "platform_hid", "cli", "render", "asset", "lua", "kimb" },
  engine_module = true,
}
