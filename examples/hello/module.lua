-- The game the introduction builds (docs/guides/introduction.md): one sprite in a window,
-- steered by the arrow keys, that pulses when Space is pressed.
module "hello" {
  output        = "exe",
  engine_module = true,
  deps          = { "core", "math", "platform_hid", "asset", "asset_render", "sprite", "frameloop" },
  licenses      = { "assets/CREDITS.md", "assets/LICENSE-CC0.txt" },
}
