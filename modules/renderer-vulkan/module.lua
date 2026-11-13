-- Inner engine: the Vulkan 1.3 backend for the render interface. The only module that
-- includes a Vulkan header. Its shaders are the render interface's (render/shaders/), which
-- the build compiles to SPIR-V into bin/shaders/. What differs per platform is the loader
-- and the surface (src/vk_platform.h): through SDL3 on Linux (src/sdl3/), vulkan-1.dll and
-- the window's HWND on Windows (src/win32/), libvulkan.so and the activity's ANativeWindow
-- on Android (src/android/).
module "renderer-vulkan" {
  provides  = "render",
  platforms = { "linux", "windows", "android", "androidemu" },
  deps      = { "core", "math", "render", "platform", "platform_hid" },
  external  = { vulkan = ">=1.3", sdl3 = { ">=3.2", platforms = { "linux" } } },
  engine_module = true,
}
