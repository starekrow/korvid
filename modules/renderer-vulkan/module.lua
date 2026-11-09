-- Inner engine: the Vulkan 1.3 backend for the render interface. The only module that
-- includes a Vulkan header. Shaders are Slang, compiled to SPIR-V by the build. What
-- differs per platform is the loader and the surface (src/vk_platform.h): through SDL3 on
-- Linux (src/sdl3/), vulkan-1.dll and the window's HWND on Windows (src/win32/).
module "renderer-vulkan" {
  provides  = "render",
  platforms = { "linux", "windows" },
  deps      = { "core", "math", "render", "platform", "platform_hid" },
  external  = { vulkan = ">=1.3", sdl3 = { ">=3.2", platforms = { "linux" } } },
  tools     = { ["shader-compile"] = { install = "shaders" } }, -- the rule's outputs land in bin/shaders/
  engine_module = true,
}
