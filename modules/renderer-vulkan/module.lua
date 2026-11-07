-- Inner engine: the Vulkan 1.3 backend for the render interface. The only module that
-- includes a Vulkan header. Shaders are Slang, compiled to SPIR-V by the build.
module "renderer-vulkan" {
  provides  = "render",
  platforms = { "linux" },
  deps      = { "core", "math", "render", "platform" },
  external  = { vulkan = ">=1.3", sdl3 = ">=3.2" },
  tools     = { ["shader-compile"] = { install = "shaders" } }, -- the rule's outputs land in bin/shaders/
  engine_module = true,
  unity     = false, -- renderer.cc's private types are fields of the public Renderer; one TU until the refactor
}
