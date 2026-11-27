-- Krome: UI for tools and dialogs (docs/design/krome.md, docs/PRDs/Krome-Master-PRD.md).
-- Screens are trees of four kernel node types laid out with rows, columns and stacks,
-- drawn on the renderer's UI primitive, wired up in a Lua state of Krome's own.
module "krome" {
  deps          = { "core", "math", "platform", "platform_hid", "cli", "render", "asset", "asset_render", "lua", "kimb", "typeset", "dejavu", "patherizer" },
  test_deps     = { "kim" }, -- a test compiles .kim text, as goldberg does
  test_files    = { "modules/dejavu/assets/DejaVuSans.ttf" }, -- the edit test's face
  engine_module = true,
  licenses      = { "icons/LICENSE" }, -- std's icons: Material Symbols (Apache 2.0), icons/README.md
}
