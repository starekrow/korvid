-- Inner engine: the Direct3D 12 backend for the render interface, on Windows (desktop x64 and
-- ARM64; the GDK's Xbox flavour of d3d12 is the same API with its own headers). The only
-- module that includes a D3D12 or DXGI header. Its shaders are the render interface's
-- (render/shaders/), which the build compiles to DXIL wherever this backend is bound: the
-- slang rule's renderer-d3d12 variant writes <stem>.dxil.zip, slangc's container of one
-- signed DXIL blob per entry point (src/d3d_shader.cc reads it through pack and inflate).
-- Windows binds Vulkan by default; kormake.lua's `bind "render" { windows = ... }` picks
-- this one.
module "renderer-d3d12" {
  provides  = "render",
  platforms = { "windows" },
  deps      = { "core", "math", "render", "platform", "platform_hid", "pack", "inflate" },
  external  = { d3d12 = ">=12" },
  engine_module = true,
}
