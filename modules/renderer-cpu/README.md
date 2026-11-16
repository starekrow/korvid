# renderer-cpu

A backend for the render interface that needs no GPU (docs/PRDs/CPU-Renderer-PRD.md). It is
linked beside the platform's GPU backend; render's dispatcher takes it when no GPU backend can
draw, or when asked (`--renderer cpu`, `KV_RENDERER=cpu`). With no window it draws headless,
into a buffer, for tests and captures (`gallery --headless`).

It draws everything the GPU backends do: sprites with every blend and both filters, the sort's
keys, layers and screen rows, the UI primitive (and so Krome), the post pass, and the games'
shaders.

- `renderer.cc`: making and ending it, resources, the frame's steps as the core calls them.
- `frame.cc`: the sort and cull (the GPU's, on the CPU), quads and UI items, tiles and their
  hashes, the job batch, the post pass.
- `luts.cc`: the sRGB tables and the UI's curves, made with the engine's math.
- `shader.cc`: the game shaders' registry and their texture reads (`korvid/renderer-cpu/shader.h`).
- `shaders/`: the span kernels a game shader is compiled inside (slang's `shader_cpu` rule);
  `include/korvid/renderer-cpu/shader_glue.*`: the glue around the generated C++.

The pixel kernels are `renderer-cpu-raster`: built with the `fast` profile and again for AVX2,
chosen at start-up; every path draws the same bytes.

Switches, for measuring:

| | |
| --- | --- |
| `--cpu-shaders none` | game shaders off: their materials draw with their blend's pipeline, no post pass |
| `KV_CPU_ISA=base` or `avx2` | the kernels to draw with |
| `KV_CPU_TILE_CACHE=0` | draw every tile, every frame |
| `KV_CPU_TARGET8` | round each draw through 8-bit sRGB, as a GPU's target does |
| `KV_CPU_STATS[=N]` | a line of means every N frames (300) |

`tools/cpu-numbers.sh` runs the numbers; `tools/render-gate.sh linux cpu` the gate. Notes and
numbers: docs/design/renderer-cpu.md.
