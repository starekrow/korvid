-- Outer engine: user interface. Screens of named nodes (panels, labels with rich text,
-- lists, scrolls, meters, fields) from .ui.json documents or code, laid out against a
-- design size, drawn as screen-space sprites, driven by the action map, reporting by events.
module "ui" {
  deps          = { "core", "math", "render", "platform", "platform_hid", "asset", "asset_render", "sprite", "audio" },
  engine_module = true,
}
