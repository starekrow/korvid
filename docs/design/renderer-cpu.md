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

## Milestone 3: the UI and Krome

- **The UI primitive** is `ui.slang`'s `fs_ui` line for line, in float, per pixel (`raster.cc`, `draw_ui`):
  - `rb_ui_draw` decodes a layer's list (`frame.cc`, `prepare_ui`) into `CpuUi` items: colors linear and
    premultiplied, the quad cut by its clip.
  - The items go into the pass's one draw list, in order with the sprites' quads (`kCpuDrawUi` marks them), so a
    list draws after its layer's sprites as on the GPU.
  - The UI's own pixels are reached from the window's through the inverse of the push block's screen view, so a
    screen camera that scales or turns the UI is followed.
  - Two tables, made with the engine's math in `luts.cc`:
    - the shadow's `0.5 (1 - erf(x))`, the shader's own approximation, in 128ths of x;
    - a glyph's coverage curve, 64 curves by the ink's luminance, interpolated.
  - A box's inside, where its result is exactly its fill (past the radii and the border, no gradient or image, a
    shadow only under an opaque fill), is one store. It gives the same bytes as the general path.
  - The blend is premultiplied over, in 16 bits.
- **Against Vulkan:**
  - The gallery's `ui` scene: largest difference 5 levels, no pixel past 8. It is exact against `references/cpu`
    (now in the gallery's test).
  - kromedemo's 13 shots: no pixel past 8; the largest difference is 8 (`gallery_dialog`), most are 5.
  - kromescreens' 9 shots: no pixel past 8; the largest difference is 4.
  - Two CPU runs give the same bytes.
- **Tile hashes** (SR17):
  - A draw's hash is its prepared bytes, the texture's version among them. A texture's version moves on every
    create and every upload.
  - A tile's hash is its draws' hashes in order, over the clear.
  - In the window pass, a tile that hashes as last frame, into the same target, is not drawn, and gets no job.
  - `KV_CPU_TILE_CACHE=0` draws every tile.
  - Every kromedemo and kromescreens shot is byte-identical with the cache on and off. `renderer-cpu`'s test
    checks the cache too:
    - an unchanged frame draws no tile;
    - a box's change draws its one tile, and the bytes match drawing every tile.
- **Pacing.** The PRD said pacing is frameloop's, but frameloop had none: it leaned on vsync.
  - On the CPU a frame ended in about a millisecond. Under on-demand pacing the frames a tick asked for, and the
    three after input, all ran before the next tick was due. The loop then slept to its quarter-second
    heartbeat: a hover could take 250 ms to show, and kromedemo ran 5 ticks a second.
  - `RenderCaps::paced` now says whether a backend's present waits for the display (`KV_RB_PACED`, 0 for the
    CPU's).
  - When it does not and vsync is wanted, frameloop starts frames no closer than a 60 Hz period. It gives the
    gap to the Work cycle when that has work, and sleeps otherwise.
  - kromedemo is at 60 frames and 60 ticks a second, as on Vulkan.
  - The display's real refresh rate is not asked for yet.
- **The hover number.** kromedemo's form at 1000 x 640, dev build, 16 threads: `--hover` moves the pointer onto
  Save and off it every tick for ten seconds. `KV_CPU_STATS=1` prints the backend's means every 300 frames;
  `--prof-report` gives the zones.

  | | `render` per frame | Tiles drawn of 160 | Tile work, summed over threads |
  | --- | --- | --- | --- |
  | Tile hashes on | 1.12 ms | 2 | 0.46 ms |
  | Tile hashes off | 2.29 ms | 160 | 23.7 ms |

  - With the hashes on, what is left is the UI lists' decoding (0.10 ms), the binning and hashing of 249 draws,
    and the present.
  - Vulkan's `render` on the same run is 13 to 16 ms of mostly vsync wait.
  - The 1280 x 720 and 1920 x 1080 numbers are milestone 5's table: frameloop has no size option yet.
- **Not done here:** D3D12 and Metal need nothing new for this milestone; Win32's software present is still
  unrun.
