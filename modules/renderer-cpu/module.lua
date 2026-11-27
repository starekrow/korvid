-- Inner engine: the CPU renderer (docs/PRDs/CPU-Renderer-PRD.md), a backend for the render
-- interface that needs no GPU. Linked into the same program as a GPU backend, it draws when
-- none can (render's dispatcher chooses), or when asked for (--renderer cpu, KV_RENDERER=cpu),
-- or with no window at all: headless, into a buffer, for tests and captures. It presents
-- through the window's software surface (platform_hid's software_frame). Its pixel kernels are
-- renderer-cpu-raster, built with the fast profile.
module "renderer-cpu" {
  provides  = "render",
  platforms = { "linux", "windows", "macos", "ios", "iossim", "android", "androidemu" },
  deps      = { "core", "math", "render", "platform", "platform_hid", "renderer-cpu-raster" },
  engine_module = true,
  unity     = false,
}
