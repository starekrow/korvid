-- The mechanical part of every game: options, window and renderer from a config, the
-- frame loop (pump, ticks, extract, render), resize, reporting, and ExitRequested on
-- the bus. A game module declares a FrameLoopConfig and provides the logic.
module "frameloop" {
  deps          = { "core", "platform", "math", "render", "sprite" },
  engine_module = true,
}
