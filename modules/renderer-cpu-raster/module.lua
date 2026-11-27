-- Inner engine: the CPU renderer's pixel kernels (docs/PRDs/CPU-Renderer-PRD.md, "Fast code
-- where it pays"): the tiles' rasterizer, the UI primitive, the game shaders' spans, the
-- encode to sRGB. A module of its own so it can be built with the fast code-generation
-- profile while renderer-cpu, whose frame code uses the engine's inline math, is not: it
-- includes nothing of the engine's but core/types.h, and takes the sRGB tables as arguments.
-- raster.cc is built again for AVX2 where the toolchain names it (isa_sources), and select.cc
-- chooses a path at start-up; every path draws the same bytes.
module "renderer-cpu-raster" {
  platforms   = { "linux", "windows", "macos", "ios", "iossim", "android", "androidemu" },
  deps        = { "core" },
  codegen     = "fast",
  unity       = false,
  isa_sources = { avx2 = { "src/raster.cc" } },
}
