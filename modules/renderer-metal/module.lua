-- Inner engine: the Metal backend for the render interface, on the Apple family. The only
-- module that includes a Metal header. Its shaders are the render interface's
-- (render/shaders/), which the build compiles into Metal libraries in bin/shaders/. The
-- layer comes from the window through SDL3 (src/sdl3/). Objective-C++ without automatic
-- reference counting: src/renderer_internal.h says why. iOS and its simulator join in
-- slice 7's milestone 7.
module "renderer-metal" {
  provides  = "render",
  platforms = { "macos" },
  deps      = { "core", "math", "render", "platform", "platform_hid" },
  external  = { metal = ">=3", sdl3 = ">=3.2" },
  engine_module = true,
}
