-- Outer engine: animation. Clips are assets (JSON files or runtime descriptors) made of
-- tracks over uv, tint, placement and event channels; an AnimPlayer component on any kind
-- plays one, and "anim.play" writes the channels at the end of every tick.
module "anim" {
  deps          = { "core", "math", "render", "platform", "asset", "sprite", "scene", "save" },
  engine_module = true,
}
