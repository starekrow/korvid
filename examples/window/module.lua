-- A window and nothing else, for bringing up a platform's window backend: it logs what the
-- window delivers and whether the frame loop keeps running while the window is moved or
-- resized. No renderer, so it runs wherever platform_hid does.
module "example_window" {
  output        = "exe",
  deps          = { "core", "platform", "platform_hid" },
  engine_module = true,
}
