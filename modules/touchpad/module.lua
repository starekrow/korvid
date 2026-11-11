-- Outer engine: a gamepad on a touch screen. A stick wherever the left thumb lands (within a
-- region, kept off its edges) and buttons as translucent circles, laid out by a JSON file
-- (assets/default.json, or a game's own), drawn as screen-space sprites. It is a pad: it
-- fills a gamepad slot of the input state, so every game's pad bindings ("stick left",
-- "pad South") work on a phone with nothing changed. On by itself where the screen is
-- touched (iOS); on the desktop the mouse can stand in for a finger (frameloop's --touchpad).
module "touchpad" {
  deps          = { "core", "math", "render", "platform", "platform_hid", "asset", "sprite" },
  engine_module = true,
}
