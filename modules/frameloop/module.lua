-- The mechanical part of every game: options, window and renderer from a config, the
-- frame loop (pump, ticks, extract, render), resize, reporting, and ExitRequested on
-- the bus. A game module declares a FrameLoopConfig and provides the logic.
module "frameloop" {
  deps          = { "core", "platform", "platform_hid", "math", "render", "asset", "asset_render", "sprite", "scene", "sprite_text", "cli", "touchpad" },
  engine_module = true,
}
