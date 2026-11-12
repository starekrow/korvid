-- The renderer's features in fixed scenes, each drawn and kept as a screenshot, so two
-- backends (or one backend before and after a change) can be compared picture by picture
-- with the picture tool. It drives the render interface directly: no sim, no frame loop,
-- nothing that depends on a platform's libm, so the pictures differ only where the GPUs do.
module "gallery" {
  output        = "exe",
  deps          = { "core", "math", "platform", "platform_hid", "render" },
  tools         = { ["shader-compile"] = { install = "shaders" } },
  engine_module = true,
}
