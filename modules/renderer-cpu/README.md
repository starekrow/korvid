# renderer-cpu

A backend for the render interface that needs no GPU (docs/PRDs/CPU-Renderer-PRD.md). It is
linked beside the platform's GPU backend; render's dispatcher takes it when no GPU backend can
draw, or when asked (`--renderer cpu`, `KV_RENDERER=cpu`). With no window it draws headless,
into a buffer, for tests and captures (`gallery --headless`).

- `renderer.cc`: making and ending it, resources, the frame's steps as the core calls them.
- `frame.cc`: the sort and cull (the GPU's, on the CPU), quads, tiles, the job batch.
- `raster.cc`: the pixel kernels: edges, sampling, blends, encoding. Nothing of the engine's
  but its types, so a fast build profile can take it (`tools/check-fast-objects.sh`).
- `luts.cc`: the sRGB tables.

What it draws so far: sprites, every blend, both filters, the sort's keys, layers and screen
rows. Not yet: the UI primitive (milestone 3), the post pass and game shaders (milestone 4).
Notes and numbers: docs/design/renderer-cpu.md.
