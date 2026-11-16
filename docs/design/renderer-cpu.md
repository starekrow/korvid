# The CPU renderer

Nov 15, 2026. Notes on `renderer-cpu` as it is built (docs/PRDs/CPU-Renderer-PRD.md): what each milestone found, and
the numbers each port reports.

## Milestone 1: the seam

- **Two backends in one build.** Each backend's code and its compiled-in core live in `kv::<KV_RB_NS>`; render's
  dispatcher (`render/src/dispatch.cc`) is the only definition of `renderer.h`. kormake passes the bound backends as
  `KV_BIND_RENDER_<MODULE>=<place>` and `KV_BIND_RENDER_COUNT`. Linux links with `--start-group`: render's dispatcher
  calls into the backends that depend on render. render is built without a unity file, so a program that never calls
  the renderer (render's own tests) does not pull the dispatcher in.
- **Choosing.** `renderer_choose` runs before the window opens: `--renderer`, then `KV_RENDERER`, then each bound
  backend's `renderer_probe` in the binding's order. Vulkan's start-up no longer fails without a 1.3 loader when another
  backend is bound; its probe then says no.
- **The CPU backend so far** clears its target to the view's color, presents through `HostWindow::software_frame` (SDL's
  window surface on Linux; a DIB blitted with `SetDIBitsToDevice` on Windows, unbuilt here), and takes screenshots,
  with a window or headless (`gallery --headless`).

## The Slang spike

Can a game's fragment shader run on the CPU through `slangc -target cpp`? Yes, with three workarounds, at about 6 ns a
pixel for `gallery_rings` (one texture sample, a `length`, a `frac`, a `lerp`), scalar, `-O2`, one core. The spike's
wrapper and host are in `modules/renderer-cpu/notes/` (`spike_rings_cpu.slang.txt`, `spike_host.cc.txt`).

- **A compute-shaped wrapper.** The CPU target has no fragment stage. The engine's wrapper includes the game's file and
  defines a compute entry, `cs_span`, that steps the varyings across a span of pixels and calls `fs_main` for each.
- **`discard`** is refused outside a fragment stage. The wrapper defines `discard` as a macro returning a colour whose
  alpha no shader writes (-1e30) before including the game's file; the span reads it as "pixel kept".
- **The entry attribute.** Left on, `[shader("fragment")]` makes Slang's C++ take `fs_main`'s varyings by pointer while
  the wrapper's call passes them by value, and the generated C++ does not compile. The wrapper defines
  `shader(stage)` as `ForceInline`.
- **What the generated C++ needs.** Slang pastes its CPU prelude in, preprocessed with the host's system headers (9,300
  lines for one shader): it is generated and compiled on the platform it runs on. The shader's globals are one struct
  (`GlobalParams_0`, its fields named `gPush_0`, `gMaterials_0`, `gTextures_0`, ...); buffers are `{data, count}`.
  Textures are Slang's `ITexture`, a virtual interface the host implements (`Sample`, `SampleLevel`, `Load`,
  `GetDimensions`): the CPU renderer answers it with its own sampler. That interface is Slang's, at the shader
  boundary; the seam's no-virtuals rule stays the renderer's own.
- **Names carry Slang's suffixes** (`_0`), deterministically. The glue the shader rule will append to each generated
  file fills `GlobalParams_0` by those names, and a test will hold them.

## Milestone 2: sprites

- **The sort** is the GPU's on one thread (`frame.cc`): every live row placed as `row_placement` places it, culled by
  its bounding circle, bucketed, counted, scattered in row order into `tables.sorted`. Each keyed bucket's first
  2,048 rows are then merge-sorted by (orderable key, row). It fills the same `sorted`, `bucket_of` and `sort_meta`
  the GPU's does, so `--check-sort` verifies it with the GPU's verifier: the testbed's swarm (42,005 rows, 40,005
  visible, 513 buckets) checks clean.
- **The draws.** Each run's rows become quads in the window's pixels (`prepare`): corners in 1/256 pixel, wound so
  the inside is positive, the inverse of the corner-to-pixel map for the local coordinates, the uv rect and tint in
  fixed point. At the pass's end they are binned into 64 x 64 tiles in order, and the tiles drawn as a job batch.
  A tile's picture depends only on its list, so the result is the same on any number of threads.
- **The pixels** (`raster.cc`, which includes nothing of the engine's but its types):
  - Coverage: edge functions in 1/256 pixel, the top-left rule.
  - Sampling: nearest; linear with 8-bit weights; repeat addressing; sRGB textures decoded through a table before
    filtering.
  - Tint and blends: integer arithmetic on 16-bit linear light.
  - Encoding: through a 64K-entry table when the tile is written out.
- **Against Vulkan** (`examples/gallery/tests`, headless in `kormake test`):
  - The eight sprite scenes are within the cross-backend tolerance; five are within one level everywhere.
  - What differs is edges that run exactly through pixel centres: lines of pixels along 45-degree edges in
    `transform` (233 pixels) and `camera` (19). There `kv::sin` and the GPU's `sin` differ in the last bit and
    decide a whole line; the rule is the same.
  - The CPU's own references (`references/cpu`) are exact.
- **The pixel format, measured.** The testbed's swarm (42,005 sprites at 960 x 540, a dev build, 16 threads,
  `--prof-report`):

  | Target | Tile work, summed over threads | Raster wall time per frame | fps |
  | --- | --- | --- | --- |
  | 16-bit linear tiles | 145.7 ms | 10.0 ms | 49 |
  | 8-bit sRGB per draw, as the GPU's (`KV_CPU_TARGET8`) | 161.1 ms | 11.1 ms | 46 |

  Linear is the faster, by about 10%, and is kept.
- **Inline math stays out of the sim.** `tools/check-fast-objects.sh` lists the weak symbols of the objects meant
  for the fast profile (`raster.o`: none) against every other object's. arcfire's normal replay matches its 236
  hashes with the CPU renderer linked and drawing, and with Vulkan drawing.
