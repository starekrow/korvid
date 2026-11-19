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

- **The UI primitive** is `sprite_ui.slang`'s `fs_ui` line for line, in float, per pixel (`raster.cc`, `draw_ui`):
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
  unrun (run since: "Windows", below).

## Milestone 4: game shaders and the post pass

- **Slang's C++, built by kormake.** slang has a second rule, `shader_cpu`. It applies only where renderer-cpu is
  bound (a rule's new `bound` field). kormake got four generic rule fields for code a tool writes into a module:
  - `wrap` writes the file the tool reads instead of the input. The wrapper is the first one whose `match` the
    input contains: `cpu_post.slangh` for a shader that includes `post_fragment.slangh`, else `cpu_sprite.slangh`.
  - `glue` writes a C++ file compiled into the module that owns the shader, quietly (a toolchain's new
    `flags_generated`).
  - `engine_module` lists the module the glue defines in each executable's module list, after its owner.
  - A module declines a tool's rule by name (`skip_rules`): render's own shaders are not CPU shaders.

  An executable's tests link its generated objects, so the gallery's test lists its shaders' modules.
- **The glue** (`korvid/renderer-cpu/shader_glue.h`, `.inl`):
  - Slang pastes its CPU prelude into every file, the host's C headers expanded in it. The glue includes those
    headers first, so their guards keep the pasted copies out. Then it wraps the generated code in a namespace of
    the shader's own and renames its three `extern "C"` entries. No two shaders' code meets, and none of it meets
    the sim's: `check-fast-objects.sh` now lists the generated objects with the fast ones (60 weak symbols, none
    shared).
  - Slang suffixes a struct's fields differently from shader to shader (`tint_2`, `tint_3`). So the span and the
    engine's tables cross by layout, checked by `static_assert`, and the globals are found by name with C++20
    `requires`.
  - Textures are `ITexture` objects, one per slot per thread, that call back into the renderer
    (`cpu_shader_sample`). The sampler is told apart by its state's value: nearest-repeat, linear-repeat or
    linear-clamp.
- **Registration.** Each generated module's startup calls `renderer_cpu_shader_generated(stem, fn)`. The game's own
  form comes through `renderer_cpu_shader(name, fn)` and wins over Slang's. The registry is the program's (code
  linked in), so neither needs the engine. When a material first names a shader, `rb_game_pipe_load` looks it up.
  With no CPU form, its sprites draw with the engine's pipeline and an error is logged, as on a GPU.
- **Drawing.**
  - A game shader's quad is drawn in runs: each row's covered pixels are one call, with the varyings at the first
    pixel and their step (uv, world, local, tint, the material).
  - The outputs blend by the material's blend, and an alpha of -1e30 is a `discard`.
  - Its tile hash takes the frame's time and tick, since a shader may animate.
- **The post pass.**
  - The world pass draws its tiles into a 16-bit linear image of the window's size.
  - In the window pass, each tile's ground is the post shader's picture (a row a call, written as is) instead of
    the clear; the layers above draw over it.
  - The tile cache is off in such a pass: the shader may read the world anywhere.
- **The fast path.** `RendererDesc::cpu_shaders = false` (frameloop's and the gallery's `--cpu-shaders none`)
  draws game-shader materials with their blend's pipeline and leaves the post pass out. Each skip is logged once,
  not as an error. `renderer_cpu_shader_skip(name)` does it for one shader.
- **Against Vulkan:**
  - The gallery's `shaders` scene (rings with `discard`, the post shader): largest difference 1 level.
  - `references/cpu/gallery_shaders.png` is now the Slang path's; the engine-pipeline picture moved to
    `references/cpu-none/`. The gallery's test draws both ways (34 checks).
  - arcfire's nebula, dissolve and shimmer draw as Vulkan's do. Its frames cannot be compared exactly: the nebula
    follows the render time.
  - arcfire's normal replay matches its 236 hashes with the shaders running.
- **Numbers** (arcfire's demo at 1280 x 720, shimmer on, dev build, 16 threads): the raster takes 9.3 ms of wall
  time a frame over its two passes, 63 ms of tile work summed over threads. With `--cpu-shaders none`: 5.8 ms and
  42 ms.
- **Vsync, found when the user saw tearing.** The SDL window surface's present does not wait for vblank by
  default, and milestone 3's 60 Hz timer ran against a 120 Hz display.
  - `HostWindow::software_vsync` asks SDL for the surface's vsync (`SDL_SetWindowSurfaceVSync`). It applies it
    again to a surface a resize makes.
  - Where it is granted, `RenderCaps::paced` is true (`KV_RB_PACED` is now an expression of the renderer) and the
    display paces the CPU renderer as it paces a swapchain. The frame loop's timer is the fallback; Win32's DIB
    present has none.
  - On this machine (Wayland, 120 Hz) kromedemo runs at 120 fps with 60 ticks a second. arcfire with the shimmer
    runs at 91 to 103 fps: a frame that misses a vblank waits for the next.

## Milestone 5: fast, and close

- **Code-generation profiles** (SR14). A module names one (`codegen = "fast"`) and each toolchain spells it
  (`codegen_profiles`); a tool rule can name one for the code it writes (slang's `shader_cpu`: the games' shaders).
  The module's tests are not built with it.
  - The fast profile is `-O3 -fno-trapping-math` (with the engine's `-fno-math-errno`) on gcc and clang, and
    `/O2 /Ob3` on cl.
  - **Not as the PRD drafted it, on purpose:** no `-ffp-contract=fast` and no `/fp:fast`. arm64 always has FMA,
    so contraction would make the pictures differ by platform, and `/fp:fast` reassociates. Either would break the
    exact references that every platform's gallery test holds (`references/cpu`). The pixel math that decides a
    picture is integer, so what contraction would buy is in the float geometry, which is a small share.
  - Never `-ffast-math` anywhere: its `crtfastmath.o` would flush denormals for the whole process.
- **The kernels are a module of their own**, `renderer-cpu-raster`.
  - Profiles are chosen per module (decided). renderer-cpu's frame code uses the engine's inline math (`kv::cos`,
    `kv::floor`), which must never be compiled fast, so the kernels moved out.
  - They include nothing of the engine's but `core/types.h`. They take the sRGB tables as arguments, since
    renderer-cpu makes those with `kv::pow`.
  - `korvid/renderer-cpu-raster/shader_call.h` holds a game shader's call, which the kernels fill.
- **Instruction sets** (SR16). kormake builds a module's `isa_sources` again for each set the toolchain names
  (`isa = { avx2 = { "-mavx2", "-mno-fma" } }` on Linux, `/arch:AVX2` on Windows), with `KV_ISA_<SET>=1`. Each copy
  of the kernels lives in its own namespace (`rb_cpu::base`, `rb_cpu::avx2`).
  - `select.cc` (built for the baseline) chooses the widest set the CPU runs (`cpuid`, and that the OS saves the
    YMM registers), or the one `KV_CPU_ISA` names.
  - No FMA in the AVX2 copy, so it draws the bytes the baseline does. The gallery's test draws all ten scenes
    with every set the machine runs and compares each exactly against `references/cpu`.
- **The rule's check** (SR15): `tools/check-fast-objects.sh` now covers every object of `renderer-cpu-raster` (both
  copies) and every generated shader's. None of their weak symbols is defined anywhere else.
- **The gate** (SR19): `tools/render-gate.sh linux cpu` (and `windows cpu`). In order:
  - the three configs build, and the tests pass;
  - the fast objects' check;
  - the gallery headless, exact against `references/cpu` and within tolerance of Vulkan's;
  - testbed's sort check and arcfire's demo with `--check-sort`, under `--renderer cpu`;
  - arcfire's normal replay with the CPU renderer drawing (its game shaders running);
  - the bench.
- **The numbers** (SR20): `tools/cpu-numbers.sh` runs each workload under `--renderer cpu` at 1280 x 720 and
  1920 x 1080, on every core and on one (`--threads 1`), with the widest kernels, then at 1920 x 1080 with the
  baseline ones. frameloop's new `--size WxH` sets the window. The backend's `KV_CPU_STATS` line gives the means of
  300 frames:
  - record: the sort, the passes, every tile;
  - the UI lists' decoding, and the pass's end (binning, hashing, the tiles);
  - present, which includes the wait for the display.

  Linux, 8 cores and 16 threads, a dev build, Wayland, the display at 60 Hz when these ran. Record is the renderer's
  cost. Present includes the wait for vblank, so a frame that finishes early shows about 16 ms there. Tiles are the
  means of a frame, both passes counted where a post shader makes two.

  | Workload | Size | Threads | Kernels | fps | Record ms | UI lists ms | Pass end ms | Present ms | Tiles drawn / skipped |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | kromedemo hover | 1280x720 | all | avx2 | 59 | 0.304 | 0.090 | 0.210 | 15.986 | 3.9 / 236.1 |
  | kromedemo hover | 1280x720 | 1 | avx2 | 60 | 0.365 | 0.093 | 0.268 | 16.011 | 4.0 / 236.0 |
  | kromedemo hover | 1920x1080 | all | avx2 | 59 | 0.313 | 0.095 | 0.213 | 15.885 | 4.0 / 455.0 |
  | kromedemo hover | 1920x1080 | 1 | avx2 | 58 | 0.416 | 0.109 | 0.301 | 15.899 | 3.8 / 455.2 |
  | kromedemo hover | 1920x1080 | all | base | 60 | 0.322 | 0.100 | 0.218 | 16.032 | 3.8 / 455.2 |
  | kromedemo bench (2,000 nodes a tick) | 1280x720 | all | avx2 | 60 | 0.630 | 0.503 | 0.124 | 13.898 | 1.0 / 239.0 |
  | kromedemo bench (2,000 nodes a tick) | 1280x720 | 1 | avx2 | 60 | 0.722 | 0.512 | 0.208 | 13.759 | 3.4 / 236.6 |
  | kromedemo bench (2,000 nodes a tick) | 1920x1080 | all | avx2 | 60 | 0.685 | 0.502 | 0.182 | 14.013 | 7.6 / 452.2 |
  | kromedemo bench (2,000 nodes a tick) | 1920x1080 | 1 | avx2 | 60 | 0.803 | 0.502 | 0.300 | 13.972 | 5.9 / 453.6 |
  | kromedemo bench (2,000 nodes a tick) | 1920x1080 | all | base | 60 | 0.699 | 0.516 | 0.182 | 14.113 | 7.6 / 452.2 |
  | kromescreens bench (10,000 steps a tick) | 1280x720 | all | avx2 | 59 | 0.056 | 0.027 | 0.027 | 6.535 | 2.0 / 238.0 |
  | kromescreens bench (10,000 steps a tick) | 1280x720 | 1 | avx2 | 60 | 0.036 | 0.027 | 0.006 | 6.426 | 0.0 / 240.0 |
  | kromescreens bench (10,000 steps a tick) | 1920x1080 | all | avx2 | 60 | 0.076 | 0.028 | 0.046 | 6.854 | 5.3 / 454.5 |
  | kromescreens bench (10,000 steps a tick) | 1920x1080 | 1 | avx2 | 60 | 0.122 | 0.028 | 0.092 | 7.018 | 5.3 / 454.5 |
  | kromescreens bench (10,000 steps a tick) | 1920x1080 | all | base | 60 | 0.077 | 0.028 | 0.048 | 6.816 | 5.3 / 454.5 |
  | arcfire demo, shimmer | 1280x720 | all | avx2 | 60 | 8.399 | 0.000 | 7.999 | 7.461 | 240.0 / 0.0 |
  | arcfire demo, shimmer | 1280x720 | 1 | avx2 | 12 | 71.999 | 0.000 | 70.786 | 0.476 | 240.0 / 0.0 |
  | arcfire demo, shimmer | 1920x1080 | all | avx2 | 50 | 18.365 | 0.000 | 17.940 | 1.196 | 459.0 / 0.0 |
  | arcfire demo, shimmer | 1920x1080 | 1 | avx2 | 7 | 131.374 | 0.000 | 132.393 | 1.966 | 453.1 / 8.5 |
  | arcfire demo, shimmer | 1920x1080 | all | base | 50 | 18.325 | 0.000 | 17.914 | 1.145 | 459.0 / 0.0 |
  | arcfire demo, --cpu-shaders none | 1280x720 | all | avx2 | 57 | 6.479 | 0.000 | 5.987 | 10.989 | 226.8 / 13.2 |
  | arcfire demo, --cpu-shaders none | 1280x720 | 1 | avx2 | 20 | 51.780 | 0.000 | 50.254 | 0.416 | 226.0 / 14.0 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | all | avx2 | 60 | 12.026 | 0.000 | 11.575 | 3.753 | 442.0 / 17.0 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | 1 | avx2 | 10 | 101.971 | 0.000 | 100.601 | 0.882 | 442.0 / 17.0 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | all | base | 58 | 12.246 | 0.000 | 11.833 | 4.106 | 442.0 / 17.0 |
  | arcfire --bench ring rank=56 | 1280x720 | all | avx2 | 20 | 42.613 | 0.000 | 41.154 | 0.720 | 240.0 / 0.0 |
  | arcfire --bench ring rank=56 | 1280x720 | 1 | avx2 | 4 | (no stats line) | | | | |
  | arcfire --bench ring rank=56 | 1920x1080 | all | avx2 | 11 | 85.311 | 0.000 | 83.426 | 1.356 | 459.0 / 0.0 |
  | arcfire --bench ring rank=56 | 1920x1080 | 1 | avx2 | 2 | (no stats line) | | | | |
  | arcfire --bench ring rank=56 | 1920x1080 | all | base | 12 | 85.650 | 0.000 | 83.778 | 1.165 | 459.0 / 0.0 |

  The ring bench on one core draws fewer than 60 frames in its ten seconds (4 and 2 fps), so it has no line.

- **What the numbers say, and the outliers** (decided: each is written down, and whether any needs work is the
  user's call):
  - **A tool's frame costs its change, not its window.** A hover in kromedemo records in 0.3 ms at both sizes
    (4 tiles of 240 or 459 drawn). kromedemo's 2,000-node bench costs 0.6 to 0.8 ms, most of it decoding its UI
    lists. kromescreens' 10,000 steps a tick cost under 0.1 ms. One core does as well as many for all of these.
  - **arcfire holds 60 fps at 1280 x 720 on every core**: 8.4 ms with nebula and shimmer, 6.5 ms without game
    shaders. At 1920 x 1080 it records in 18.4 ms with them (50 fps) and 12.0 ms without (60 fps).
    - Game shaders and the post pass cost about 6 ms of the 18 at 1080p. With the post pass on, every tile of the
      window is drawn each frame: the cache cannot follow a shader that reads the world anywhere.
  - **One core is 8 to 9 times slower than sixteen threads** on arcfire (102 ms against 12 ms at 1080p without
    shaders): the tiles spread well.
  - **Outlier: AVX2 bought nothing**, as first built. The AVX2 copy was within 1 to 3% of the baseline everywhere,
    and its object held 4 instructions on the 256-bit registers: the kernels were scalar per pixel, and the
    compiler found nothing to widen. The user asked for hand-written span kernels for sprites; see "The AVX2 sprite
    spans" below.
  - **Outlier: the ring bench** (`--bench ring --dial rank=56`, 11,800 sprites, most of them additive glows) runs
    at 20 fps at 720p and 11 at 1080p. The engine's profiler puts nearly all of it in the world pass's tiles
    (418 ms summed over threads, about 26 ms of wall time at 720p). It is overdraw, not the sort or the binning.
    The PRD expects nothing of it; it is the ceiling. Vulkan draws it in about 1 ms.
  - **Not measured here:**
    - the gallery's ten scenes one by one (the gallery draws each once, and its test holds their pixels);
    - the testbed swarm beyond milestone 2's numbers;
    - every port but Linux. Windows reports when its build runs `tools/cpu-numbers.sh` (it needs a POSIX shell:
      Git Bash). Done since: "Windows", below.
- **The profiler's zones** are `cpu.sort`, `cpu.raster` and `cpu.tiles`, beside the core's `record` and `render`.
  `renderer_gpu_stats` is not filled in: the CPU's stats line has its numbers, and frameloop prints no GPU line
  when there is no GPU.

## The AVX2 sprite spans

Asked for after milestone 5, when its numbers showed the AVX2 copy buying nothing.

- **A quad's row is one run.** A quad is convex, so its covered pixels in a row are one run. `draw_quad` now solves
  the run's ends from the four integer edge functions (one division an edge a row) instead of testing pixel by
  pixel. The coverage is exactly the same, and the gallery's pictures did not move for it.
- **The baseline's coordinates are per pixel.** A sprite pixel's local coordinates are now `l0 + x ldx + y ldy` at
  each pixel, not a sum stepped across the row: eight lanes cannot reproduce a running sum bit for bit.
  - This moved 27 pixels of the gallery's `transform` scene, where nearest sampling sits exactly on a texel's edge.
    `references/cpu/gallery_transform.png` is redrawn. It is still within tolerance of Vulkan's: 250 pixels past
    8, against 233 before, all on those edges.
- **The AVX2 span** (`raster.cc`, `sprite_span` under `KV_ISA_AVX2`) draws eight pixels at once: the coordinates,
  the uv, nearest or linear sampling with gathers (the texels, then the sRGB table), the tint, and the three
  blends. The blends work on pixel pairs, after a transpose.
  - It is the scalar arithmetic lane by lane: the same float operations in the same order (no FMA in this copy),
    `floor` and truncation as `floor_i` and the casts, the integer sampling weights.
  - `x / 65535` is `(x * 0x80008001) >> 47`, which holds for every 32-bit value (checked over all of them).
  - A texture whose size is a power of two wraps by a mask.
  - Group by group it falls back to the scalar pixel: a texture of another size read past its edge, a tint
    above 1, an 8-bit target (`KV_CPU_TARGET8`), and a run's last pixels short of eight.
- **Proof of the same bytes:**
  - renderer-cpu-raster's test draws 3,000 tiles of random quads with every kernel set and compares them byte for
    byte: turned and axis-aligned quads, both filters, every blend, tints above and below 1, textures of
    power-of-two sizes and not, uvs that wrap.
  - The gallery's test draws its ten scenes with each set, exactly against `references/cpu`.
- **Numbers.** The arcfire rows of `tools/cpu-numbers.sh` again, the display at 120 Hz this time. 1920 x 1080 has
  510 tiles here (30 x 17). The earlier table's 459 (27 x 17) mean that window was no wider than 1728: the display
  it opened on probably cut it.

  | Workload | Size | Threads | Kernels | fps | Record ms | UI lists ms | Pass end ms | Present ms | Tiles drawn / skipped |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | arcfire demo, shimmer | 1280x720 | all | avx2 | 120 | 4.596 | 0.000 | 4.125 | 3.120 | 240.0 / 0.0 |
  | arcfire demo, shimmer | 1280x720 | 1 | avx2 | 25 | 41.158 | 0.000 | 40.734 | 0.492 | 240.0 / 0.0 |
  | arcfire demo, shimmer | 1920x1080 | all | avx2 | 84 | 10.370 | 0.000 | 9.916 | 1.186 | 510.0 / 0.0 |
  | arcfire demo, shimmer | 1920x1080 | 1 | avx2 | 12 | 84.313 | 0.000 | 83.202 | 0.942 | 510.0 / 0.0 |
  | arcfire demo, shimmer | 1920x1080 | all | base | 48 | 19.773 | 0.000 | 19.393 | 1.048 | 510.0 / 0.0 |
  | arcfire demo, --cpu-shaders none | 1280x720 | all | avx2 | 120 | 2.665 | 0.000 | 2.124 | 5.123 | 228.0 / 12.0 |
  | arcfire demo, --cpu-shaders none | 1280x720 | 1 | avx2 | 56 | 17.373 | 0.000 | 17.068 | 0.340 | 227.2 / 12.8 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | all | avx2 | 120 | 4.311 | 0.000 | 3.824 | 3.277 | 459.0 / 51.0 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | 1 | avx2 | 26 | 36.818 | 0.000 | 36.503 | 0.840 | 459.0 / 51.0 |
  | arcfire demo, --cpu-shaders none | 1920x1080 | all | base | 67 | 13.075 | 0.000 | 12.643 | 1.032 | 459.0 / 51.0 |
  | arcfire --bench ring rank=56 | 1280x720 | all | avx2 | 46 | 19.404 | 0.000 | 17.917 | 0.630 | 240.0 / 0.0 |
  | arcfire --bench ring rank=56 | 1280x720 | 1 | avx2 | 8 | 102.353 | 0.000 | 103.654 | 0.554 | 236.0 / 4.0 |
  | arcfire --bench ring rank=56 | 1920x1080 | all | avx2 | 26 | 33.241 | 0.000 | 31.238 | 0.934 | 510.0 / 0.0 |
  | arcfire --bench ring rank=56 | 1920x1080 | 1 | avx2 | 4 | (no stats line) | | | | |
  | arcfire --bench ring rank=56 | 1920x1080 | all | base | 12 | 83.895 | 0.000 | 82.027 | 1.142 | 459.0 / 0.0 |

  At 1920 x 1080 on every core:

  | Workload | Baseline | AVX2 | |
  | --- | --- | --- | --- |
  | arcfire without game shaders | 13.1 ms | 4.3 ms | 3.0x |
  | arcfire with nebula and shimmer | 19.8 ms | 10.4 ms | 1.9x |
  | the ring bench | 83.9 ms | 33.2 ms | 2.5x |

  arcfire now holds 120 fps at 1080p without game shaders, and 84 fps with them. The ring bench runs at 46 fps at
  720p (20 before). On one core, arcfire without game shaders runs at 56 fps at 720p.
  - What is left with shaders on is the shaders: Slang's C++ is scalar, a call a run of pixels, through virtual
    texture reads. A hand-written CPU form of the nebula (`renderer_cpu_shader`) would be the next step, if wanted.
  - The baseline gained a little from solving the runs: 13.3 ms against the committed kernels' 13.75 on the same
    machine the same day (arcfire without game shaders, 1080p).

## Windows

Nov 15, 2026: the first build and run on Windows. The Linux machine itself, dual-booted: a Ryzen 7 7840U (8 cores,
16 threads), Windows 11, MSVC 14.51, a dev build, the display at 60 Hz and scaled by 1.5. What differs from the
Linux numbers is the OS and the compiler, not the hardware.

- **It builds and draws the same bytes.** The branch built clean with cl at the first try (with kormake rebuilt
  from the branch's scripts: `isa_sources` and `codegen` are new fields). The gallery's test draws all ten scenes
  exactly as `references/cpu` has them, on the baseline kernels and on AVX2: cl's build makes gcc's bytes.
- **The gate passes:** `tools/render-gate.sh windows cpu`, every check. Two of them were Linux's until now:
  - arcfire's replay was handed Git Bash's `/c/...` path, which a Windows program cannot open; the gate passes the
    native one (`native_root`). Its 236 hashes match with the CPU renderer and its game shaders drawing.
  - `tools/check-fast-objects.sh` read gcc's weak symbols with `nm`. On cl an inline function is a COMDAT, and the
    linker keeps the first copy it meets, which is the same hazard. The script reads cl's objects with `dumpbin`
    (the one beside the build's `link.exe`): the fast objects' functions, against every other object's. cl's
    `/O2` makes every function a COMDAT, so being one says nothing; sharing one does. What it lets pass, and why:
    string literals and a CRT header's statics (data, and the same bytes), cl's array constructor and destructor
    loops, and the UCRT's stdio functions, which its `<stdio.h>` defines inline (`select.cc`'s `fprintf`). None
    does arithmetic, and cl's fast profile keeps `/fp:precise` anyway. 69 functions, none shared. The module's
    test is no longer counted as fast, here or on Linux: it is built as the rest are.
- **Win32's software present** (a DIB, `SetDIBitsToDevice`) works: kromedemo under `--renderer cpu`, captured from
  the screen while it ran, is the picture its screenshot is. It is paced by the frame loop (no vsync on a DIB).
- **The D3D12 gate on this branch:** the gallery is exact, the sort checks pass and the GPU frame is as before, but
  "compiled shaders differ from the baseline". The DXIL in every `.dxil.zip` is the same. What changed is the
  Slang IR module each zip also carries (`children/<n>/<n>.slang-module`, which `d3d_shader.cc` never reads): it
  records the compiler's search paths, and the shader rule now passes one more, `renderer-cpu/shaders`. The gate
  hashes whole zips; hashing their DXIL alone would say what it means to. Not done here.

`tools/cpu-numbers.sh` on this machine:

| Workload | Size | Threads | Kernels | fps | Record ms | UI lists ms | Pass end ms | Present ms | Tiles drawn / skipped |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kromedemo hover | 1280x720 | all | avx2 | 59 | 0.476 | 0.063 | 0.407 | 0.431 | 7.5 / 232.5 |
| kromedemo hover | 1280x720 | 1 | avx2 | 58 | 1.104 | 0.057 | 1.040 | 0.380 | 9.4 / 230.6 |
| kromedemo hover | 1920x1080 | all | avx2 | 59 | 0.472 | 0.060 | 0.407 | 0.785 | 9.5 / 500.5 |
| kromedemo hover | 1920x1080 | 1 | avx2 | 59 | 1.161 | 0.060 | 1.004 | 0.707 | 9.5 / 500.5 |
| kromedemo hover | 1920x1080 | all | base | 59 | 0.529 | 0.060 | 0.463 | 0.842 | 9.5 / 500.5 |
| kromedemo bench (2,000 nodes a tick) | 1280x720 | all | avx2 | 59 | 0.584 | 0.397 | 0.181 | 0.401 | 3.1 / 236.8 |
| kromedemo bench (2,000 nodes a tick) | 1280x720 | 1 | avx2 | 59 | 0.746 | 0.328 | 0.415 | 0.371 | 3.1 / 236.8 |
| kromedemo bench (2,000 nodes a tick) | 1920x1080 | all | avx2 | 59 | 0.564 | 0.354 | 0.205 | 0.746 | 4.2 / 505.9 |
| kromedemo bench (2,000 nodes a tick) | 1920x1080 | 1 | avx2 | 59 | 0.927 | 0.333 | 0.593 | 0.726 | 4.2 / 505.9 |
| kromedemo bench (2,000 nodes a tick) | 1920x1080 | all | base | 59 | 0.601 | 0.351 | 0.224 | 0.858 | 4.2 / 505.9 |
| kromescreens bench (10,000 steps a tick) | 1280x720 | all | avx2 | 58 | 0.042 | 0.031 | 0.006 | 0.425 | 0.0 / 240.0 |
| kromescreens bench (10,000 steps a tick) | 1280x720 | 1 | avx2 | 58 | 0.040 | 0.031 | 0.007 | 0.349 | 0.0 / 240.0 |
| kromescreens bench (10,000 steps a tick) | 1920x1080 | all | avx2 | 56 | 0.042 | 0.029 | 0.008 | 0.762 | 0.0 / 510.0 |
| kromescreens bench (10,000 steps a tick) | 1920x1080 | 1 | avx2 | 58 | 0.042 | 0.030 | 0.008 | 0.696 | 0.0 / 510.0 |
| kromescreens bench (10,000 steps a tick) | 1920x1080 | all | base | 59 | 0.042 | 0.030 | 0.009 | 0.741 | 0.0 / 510.0 |
| arcfire demo, shimmer | 1280x720 | all | avx2 | 59 | 6.479 | 0.000 | 6.018 | 0.562 | 240.0 / 0.0 |
| arcfire demo, shimmer | 1280x720 | 1 | avx2 | 18 | 53.465 | 0.000 | 53.141 | 0.331 | 240.0 / 0.0 |
| arcfire demo, shimmer | 1920x1080 | all | avx2 | 59 | 14.699 | 0.000 | 14.255 | 0.719 | 510.0 / 0.0 |
| arcfire demo, shimmer | 1920x1080 | 1 | avx2 | 9 | 106.144 | 0.000 | 107.737 | 0.616 | 494.0 / 16.0 |
| arcfire demo, shimmer | 1920x1080 | all | base | 34 | 27.921 | 0.000 | 27.532 | 0.647 | 510.0 / 0.0 |
| arcfire demo, --cpu-shaders none | 1280x720 | all | avx2 | 59 | 2.972 | 0.000 | 2.507 | 0.519 | 226.6 / 13.4 |
| arcfire demo, --cpu-shaders none | 1280x720 | 1 | avx2 | 47 | 20.753 | 0.000 | 20.493 | 0.320 | 227.4 / 12.6 |
| arcfire demo, --cpu-shaders none | 1920x1080 | all | avx2 | 59 | 5.052 | 0.000 | 4.599 | 0.808 | 474.0 / 36.0 |
| arcfire demo, --cpu-shaders none | 1920x1080 | 1 | avx2 | 22 | 44.784 | 0.000 | 44.495 | 0.553 | 475.1 / 34.9 |
| arcfire demo, --cpu-shaders none | 1920x1080 | all | base | 48 | 19.172 | 0.000 | 18.691 | 0.990 | 474.6 / 35.4 |
| arcfire --bench ring rank=56 | 1280x720 | all | avx2 | 38 | 26.113 | 0.000 | 24.386 | 0.395 | 240.0 / 0.0 |
| arcfire --bench ring rank=56 | 1280x720 | 1 | avx2 | 6 | 137.605 | 0.000 | 139.796 | 0.326 | 236.0 / 4.0 |
| arcfire --bench ring rank=56 | 1920x1080 | all | avx2 | 20 | 43.668 | 0.000 | 41.831 | 0.808 | 510.0 / 0.0 |
| arcfire --bench ring rank=56 | 1920x1080 | 1 | avx2 | 4 | (no stats line) | | | | |
| arcfire --bench ring rank=56 | 1920x1080 | all | base | 8 | 94.937 | 0.000 | 96.153 | 0.890 | 501.5 / 8.5 |

Beside Linux's AVX2 table above (1920 x 1080, every core; the same machine):

| Workload | Windows, baseline | Windows, AVX2 | Linux, baseline | Linux, AVX2 |
| --- | --- | --- | --- | --- |
| arcfire without game shaders | 19.2 ms | 5.1 ms | 13.1 ms | 4.3 ms |
| arcfire with nebula and shimmer | 27.9 ms | 14.7 ms | 19.8 ms | 10.4 ms |
| the ring bench | 94.9 ms | 43.7 ms | 83.9 ms | 33.2 ms |

- **What the numbers say, and the outliers:**
  - **Krome is cheap here as there.** A hover records in about 0.5 ms at either size, kromedemo's 2,000-node bench
    in 0.6 ms, kromescreens' 10,000 steps in 0.04 ms. A hover draws about 9 tiles where Linux drew 4: the window is
    scaled by 1.5, so a button covers more of them.
  - **arcfire holds 60 fps at 1080p on every core**, with its game shaders (14.7 ms) and without (5.1 ms).
  - **Present is under a millisecond** (0.3 to 0.9): a DIB blit, with no wait for the display in it.
  - **Outlier: Windows is slower on the same hardware, and cl's loops slower still.** Two parts, read from the
    same machine's two runs:
    - About 1.2 times everywhere, even where the code is hand-written: the AVX2 spans take 5.1 ms against Linux's
      4.3 on every core, and 44.8 against 36.8 on one. That share is the OS (its power plan and clocks, its
      scheduler), not the code.
    - About 1.25 times more where the compiler writes the loops: the baseline kernels take 1.47 times gcc's (19.2 ms
      against 13.1), and Slang's C++ for the game shaders is slower by as much (their share of a frame: about 9.6 ms
      against 6.1). That is cl's code (`/O2 /Ob3` against `-O3`), the likeliest thing to look at, and not looked
      at here.
  - **One core is 9 times slower than sixteen threads** on arcfire (44.8 ms against 5.1 at 1080p without
    shaders), as on Linux.

## Apple, and the render scale

- **macOS and iOS bind it too**, after Metal (`kormake.lua`). On the Mac it is the fallback, and the gallery's test
  draws every scene exactly as `references/cpu` has it on arm64's baseline (NEON) kernels.
- **Metal stands aside** where it cannot draw, as Vulkan does: with a second backend bound, a GPU short of Apple7
  with tier 2 argument buffers starts the module empty and `renderer_probe` says no. The iOS Simulator's GPU is an
  Apple2, so games there draw on the CPU; a phone draws with Metal unless `--renderer cpu` says otherwise.
- **The iOS window's software frames** (`host_window_ios.mm`): three buffers of ours; a present wraps one in a
  CGImage and makes it the contents of a plain layer over the Metal one, without an animation. The image's release
  hands the buffer back, so a frame never draws into pixels on screen. The frame loop paces the presents. Its
  frames say they rotate (`SoftwareFrame::rotates`): last frame's pixels are not in this one, so the renderer
  draws into a target of its own and copies it in at the present, and the tile cache works as on the desktop
  (kromedemo's hover in the Simulator at 2622 x 1206: 11 tiles of 779 drawn, 0.2 ms; the copy and the present
  3 ms).
- **The render scale.** A Retina Mac's window has 4 pixels for each of its units, a phone's 9, and the CPU drew
  every one: arcfire on the CPU at 52 fps on an M3 Max's 2560 x 1440, 13 fps on an iPhone 15 Pro Max. Now the CPU
  draws one pixel for N x N of the window's (`RendererDesc::cpu_scale`, `--cpu-scale auto|N`, `KV_CPU_SCALE`).
  - The program chooses (`FrameLoopConfig::cpu_scale`); the command line and `KV_CPU_SCALE` override it.
  - auto (0, a game's choice) is the window's `pixel_scale`, rounded down: the window's units on a display at 2x
    or 3x. 1 below 2x (Linux and Windows at 100%, Windows at 150% as measured above, headless), so nothing
    changes there.
  - Krome's programs (kromedemo, kromescreens) choose 1: a tool's frame costs only what changed (the tile cache),
    so dividing saves little, and it would soften text, which is most of what a tool draws.
  - The game and the core still see the window's size: the push block is built for it, and sprites, the HUD's
    screen-space layers and the UI lists reach the smaller target through clip space. Input, layout and cameras
    are untouched. The post shader's `size` is the target's (its uv is the pixel over it), as a GPU's would be at
    that density.
  - A window that can scale a frame up as it shows it is asked for a smaller one (`HostWindow::software_size`):
    iOS's layer does it on the GPU, nearest-neighbour. Elsewhere the frame draws into a target of the renderer's
    and the present scales it into the window's frame, nearest-neighbour, on the job threads (about 1.6 ms at
    2560 x 1440 on the M3 Max).
  - Next, if wanted: the world below the UI layers at the scale and the UI and screen-space layers at every
    pixel, through the post pass's two passes, so a game's HUD stays sharp too.
  - Screenshots are the target's size.
  - `tools/cpu-numbers.sh` passes `--cpu-scale 1`, so its sizes stay pixels drawn.

  arcfire's demo with nebula and shimmer, base kernels, `--renderer cpu`:

  | Machine | Window | Drawn | Record ms | fps |
  | --- | --- | --- | --- | --- |
  | M3 Max | 2560x1440 | 2560x1440 (scale 1) | 16.4 | 52 |
  | M3 Max | 2560x1440 | 1280x720 (auto, 2) | 4.5 | 120 (the display's) |
  | iOS Simulator on the M3 Max | 2622x1206 | 874x402 (auto, 3) | 3.2 | 54 (the Simulator's pacing) |
  | iPhone 15 Pro Max | 2796x1290 | 2796x1290 (scale 1) | 69 | 12 |
  | iPhone 15 Pro Max | 2796x1290 | 1398x645 (scale 2) | 20 | 47 |
  | iPhone 15 Pro Max | 2796x1290 | 932x430 (auto, 3) | 8.5 | 56 |
