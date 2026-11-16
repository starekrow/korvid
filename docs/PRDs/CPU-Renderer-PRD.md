# The CPU renderer

Nov 15, 2026 · @starekrow · branch `software-renderer`, an experiment out of band (no slice number yet)

## Goal and exit criteria

Korvid draws through one GPU backend a build: Vulkan, Metal or D3D12, chosen when the program is built. A
machine whose GPU (or driver) lacks what that backend needs gets an error at start-up and nothing on screen. This
experiment adds a renderer that runs on the CPU, **`renderer-cpu`**, linked into the same program as the GPU
backend and taken when the GPU cannot do the job, or when asked for. It is the first time two backends live in one
build, so most of what it proves is the seam, not the rasterizer.

It draws the same pictures as the GPU backends from the same inputs: the render world's rows, materials, textures,
the UI primitive's lists, the post pass and, where it can, the games' shaders. It is meant for tools (Krome's
screens, goldberg's window), for 2D games at modest loads, and for running the renderer where there is no GPU at all
(tests, CI, a headless capture).

It is done when:

- A Linux build and a Windows build each link a GPU backend and `renderer-cpu` together. The program picks one
  at start-up: the GPU backend when it can draw, else the CPU one, with the reason logged.
  `--renderer cpu` (and `gpu`, `auto`) forces the choice.
- Nothing about the GPU path changes. Its compiled shaders are byte-identical, the gallery is exact against each
  backend's references, and arcfire's bench is within noise. A build that binds one backend costs what it costs
  today.
- The CPU renderer draws the gallery's ten scenes within the cross-backend tolerance of Vulkan's references
  (`picture compare` defaults: 8 levels, 0.5% of pixels), and exactly against its own references. The game-shader
  scene is held to that with game shaders on (the Slang path), and with `--cpu-shaders none` compared only against
  the CPU renderer's own references.
- kromedemo's and kromescreens' `--demo --shots` pictures are within the same tolerance of Vulkan's, and both demos
  pass under `--renderer cpu`.
- It renders with no window: a program (or a test) can create it headless and take pictures. The gallery runs that
  way in `kormake test`.
- The sim's determinism is untouched: arcfire's replays hash the same with the CPU renderer linked, built with
  its fast code-generation profile.
- The numbers are written down (see "Performance"): frames per second for kromedemo, kromescreens' 10,000-row status
  page and arcfire's normal play, at 1280×720 and 1920×1080, on the Linux machine, on one core and on all of them.

## What the code has

Measured on `software-renderer` at its start (tool-stuff, Nov 15). The two reports this draws on looked at every
file named here.

**One backend a build, by design.** `docs/design/renderer.md` ("The core and the backends") states it: no virtual
functions, no tables of function pointers, "a name has one definition in a build". Each backend compiles the
shared core into itself: `src/render_core.cc` includes the backend's `renderer_internal.h`, then
`korvid/render/core/core.inl`. That core defines the functions of `renderer.h` against the backend's `Renderer`,
`Texture` and `FrameInFlight`, which derive from `RenderCore`, `TextureCore` and `FrameCore`. Each backend defines
`renderer_create`, `_destroy`, `_get`, `_resize` and `_set_surface` itself.

**How a backend is chosen.** Each backend module says `provides = "render"`. The toolchain names the default
(`render = "renderer-vulkan"` on Linux, Windows and Android, Metal on Apple), and `kormake.lua:20` overrides Windows
to D3D12. kormake's resolve links every module bound to an interface into any program that depends on it
(`resolve.lua:160-170`). `bind` already accepts a list (`util.as_list`). With two backends listed today, every
`renderer_*` and `rc_*` symbol and the `kv::Renderer` struct would be defined twice. On Windows both backends are
built, but only D3D12 is linked. The shader rule picks one variant per platform build (`emit.lua:227-246`).

**What the core shares that the CPU renderer reuses whole.**
- `RenderWorld` and `rw_apply`: the rows, dirty bits, bucket and keyed counts, layer offsets.
- `rw_push_block`: the projections and the cull rects.
- `rw_snapshot_keys`, `rw_next_run`, `rw_ui_set` / `rw_ui_stage`.
- The texture and material API's records, the pools, the default texture and material, the game slots.
- The screenshot writer, `--check-sort`'s verifier.

All of this is plain data (`modules/render/src/world.cc`, `ui.cc`, `core.inl`) and tested without a GPU
(`render_tests`).

**What the GPU computes, which the CPU must match** (`modules/render/shaders/`):

- **Sprites** (`sprite.slang`, `sprite_common.slangh`):
  - Placement: a centred quad of `size × scale`, rotated by GPU `sin`/`cos`, at `lerp(prev, cur, t)`, where
    `t = stamp == tick ? alpha : 1`. World rows add the layer's interpolated scroll; screen rows add their anchor.
  - The view: an affine projection, no pixel snapping.
  - Shading: uv from the row plus the material's `params0.xy`, the texture sampled nearest or linear with repeat
    on one mip, times `sprite.tint × material.tint`, straight alpha.
- **Blends**, done in linear light into an sRGB 8-bit target, each draw reading back what the one before it
  wrote:
  - Opaque: written as is.
  - Alpha: `src·a + dst·(1-a)`, and for alpha `a + dst.a·(1-a)`.
  - Additive: `src·a + dst`.
  - The UI: premultiplied, one / one minus alpha.
- **Order:**
  - Bucket `(clamp(layer)+128)·8 + slot`, ascending; row order within a bucket.
  - Keyed buckets: Key → `order`; Feet → `-(y + order)`. Compared as orderable uints, so -0 sorts before +0. Ties
    by row. At most the first 2,048 visible rows of a bucket are key-sorted.
  - A layer's UI list draws after its buckets.
  - The cull (bounding circle against the view's box) is conservative except for that 2,048 cap.
- **The UI primitive** (`ui.slang`, 188 lines):
  - Clip by discard at the pixel centre.
  - Boxes: a rounded box by signed distance (per-corner radius), an inner border, a two-stop gradient at an angle
    (lerping premultiplied linear colours), an image (uv clamped to 0..1 by its packing), and a shadow (offset,
    spread, blur through Abramowitz-Stegun's `erf`) composited under the box.
  - Glyphs: coverage raised to `lerp(0.75, 1.45, luminance)` of the ink.
  - Colours: sRGB bytes decoded by `pow` in the shader, premultiplied out.
- **The post pass** (`post.slang`, `post_fragment.slangh`):
  - The buckets below a layer draw into a world target of the swapchain's format, so 8-bit sRGB.
  - A full-window triangle then runs the game's post shader over it, sampling linear and clamped, with 16 float4
    parameters and a push block of size, time, tick, count and alpha.
- **Game shaders:** gallery `rings` and `post`, arcfire `nebula`, `dissolve` and `shimmer`.
  - They are compiled by `slangc` to SPIR-V, metallib or DXIL. There is no CPU target anywhere in the tree today.
  - They use `discard`, `frac`, `smoothstep`, `exp`, value-noise fbm and `gPush.time`.
- **No dependence on derivatives:** every texture has one mip, and min equals mag filter. No `ddx`, `ddy` or
  `fwidth` exists.

**Presenting without a GPU.** `platform_hid` already has `WindowSurface::Software` (`host_window.h:13-18`): on
SDL, `present_blank` fills through `SDL_GetWindowSurface` / `SDL_UpdateWindowSurface`. Nothing calls it. Win32's
`present_blank` is empty, and its window paints on a thread of its own.

**Where the fallback would sit today.**
- renderer-vulkan's module start-up fails without a Vulkan 1.3 loader, and a failed start-up aborts `engine_init`
  (`engine.cc:1232`).
- Device checks happen later, in `renderer_create` (`score_device`). A null renderer stops frameloop's start-up.
- The window's surface kind is fixed before the renderer exists (`frameloop.cc:243`).
- D3D12 refuses WARP outright (`d3d_device.cc:51`). Vulkan takes a CPU device (lavapipe) at the lowest score.

**Load to size against** (from `docs/` and the examples):
- The gallery: 960×540, 3 to 18 sprites a scene, and 32 UI items.
- kromedemo `--bench`: about 2,000 nodes and 13,000 glyphs, overdraw 2.1.
- arcfire's normal play: 690 to 3,140 sprites, overdraw about 4.3. The nebula is a full-field sprite costing
  twelve noise lookups a pixel.
- `--bench ring --dial rank=56`: 62,000 sprites at overdraw 29. A GPU's job; the CPU renderer is not held to
  it.

**CPU pixels that exist.** truetype and `asset/font.cc` rasterize glyphs; `image` and `tools/picture` handle PNG,
PPM and comparisons. There is no rasterizer for triangles or quads, and no null or reference renderer.

## The shape

### Two backends in one build

The rules of the seam stay: no virtual functions, no tables of function pointers, no templates over a backend.
What changes is the rule "a name has one definition in a build". It becomes **one definition per backend
namespace**, with one switch at the public API.

- **Each backend compiles in a namespace of its own.** `render_core.cc` becomes:
  ```cpp
  namespace kv::rb_vk {
  #include "renderer_internal.h"
  #include "korvid/render/core/core.inl"
  }
  ```
  The namespace is named by the backend's `KV_RB_NS`. The backend's other files open the same namespace. Its
  `Renderer`, `Texture` and `FrameInFlight`, and the core's functions over them, are `kv::rb_vk::…` and
  `kv::rb_cpu::…`. They no longer collide, and each is still one concrete type that its code sees whole.
- **`render` owns the public API.** `korvid/render/renderer.h` keeps every signature. Its `Renderer` becomes an
  opaque type whose first byte says which backend made it: `RenderCore` starts with `u8 backend`.
  `render/src/dispatch.cc` defines each `renderer_*` as a `switch` over that byte, calling the bound backends'
  namespaced functions. The function list is written once, as an X-macro in `korvid/render/core/api.h`.
  `kormake` passes the bound backends as defines (`KV_RENDER_BACKENDS=vk,cpu`).
  - With one backend bound, the switch has one case: the compiler removes it, and the call is what it is today.
    The bench says so (exit criterion).
  - With two, each call is one predictable branch on a byte. The calls are per frame and per resource, never per
    sprite.
- **Choosing.** `RendererDesc` gains `backend` (`Auto`, `Gpu`, `Cpu`). With `Auto`:
  1. The dispatcher asks each GPU backend `rb_probe(desc, &why)`. This is a check of what that backend needs,
     made before any window or surface exists: instance and loader version, a device with the features it uses,
     the formats.
  2. It takes the first that answers yes. If none does, it takes the CPU renderer and logs each refusal's
     reason.
  3. `renderer_window_surface()` answers for the chosen backend, so frameloop opens the window with the right
     surface kind (SDL's `VULKAN` flag, or none for the software surface).

  The order of a build's bound list is the order of preference. frameloop and the gallery take
  `--renderer auto|gpu|cpu`, and `KV_RENDERER` in the environment.
- **Start-up that can fail softly.** A GPU backend's engine module no longer fails `engine_init` when its
  library is missing, provided another backend is bound. It records "unavailable", and `rb_probe` reports it.
  This changes renderer-vulkan's start-up (`module.cc:16-38`). D3D12's needs no change.
- **kormake:**
  - `bind "render" { linux = { "renderer-vulkan", "renderer-cpu" } }` links both (a list already works) and
    passes the list as defines to `render` and to each backend.
  - The shader rule makes every bound backend's variant. Variants name their outputs apart
    (`sprite.spv`, `sprite.cpu…`), as the Android PRD once proposed (`Engine-Slice-Later-PRD-android2022.md:88`).
  - Each backend's `KV_RB_SHADER_EXT` finds its own.
- **`renderer_get(Engine&)`** returns the renderer that was created, whichever made it: the dispatcher remembers
  it in `render`'s state, rather than each backend looking up its module by name.

This was the main design choice of the PRD; see "Decided", 1.

### The CPU backend

`modules/renderer-cpu`, `KV_RB_NS rb_cpu`. It meets the same contract as the GPU backends (`core.h`'s five
defines, two types, five records and the `rb_` functions), so the frame's order (`frame.inl`) is the one all
three share. What its `rb_` steps mean:

- **Buffers are memory.**
  - `RbBuffer` is an allocation. The ring is ordinary memory.
  - `rb_copy_rows` is `memcpy` into the CPU renderer's own copy of the sprite and material tables.
  - Garbage is freed when its frame slot comes round, as now.
  - Frames in flight: two, so the core's ring and garbage logic hold. The CPU "submit" can run the frame at
    once (one frame of latency, never more).
- **The sort is the GPU's, on the CPU** (`rb_sort`):
  1. One pass over the live rows: the cull, as `bucket_of_row` does it, against the push block's rects; then the
     bucket.
  2. A counting sort into `sorted[]`. Stable, so rows stay in row order within a bucket.
  3. For each keyed bucket, a stable sort of its first 2,048 visible rows by the orderable-uint key, ties by row.

  This is O(rows) and runs as a job over row blocks when the rows are many. `--check-sort` verifies it with
  `rw_verify_sort`, as it does the GPU's.
- **Drawing is binned into tiles, rasterized in parallel.**
  - Binning: the frame walks the draw runs in order (`rb_draw_run`, `rb_ui_draw`) and appends each visible quad, UI
    item and the post triangle to the bins of the tiles it touches, as a compact command (the row index or the UI
    item's offset, and its bounds). Tiles are 64×64, a parameter.
  - Rasterizing: `rb_record_end` runs the tiles as a job batch (`jobs_run`). Each tile draws its commands in order
    into a tile-sized buffer held in cache, then writes the tile out.
  - What this guarantees: a tile's picture depends only on its commands, so the result is the same on one thread
    or sixteen, and identical across runs.
  - Damage skipping: a tile whose command list and inputs hash the same as last frame's is not drawn again (see
    "Krome and static frames").
- **The pixels** (decided: whichever is faster):
  - A tile holds linear light at 16 bits a channel, in fixed point, premultiplied. Blends are integer arithmetic
    on it, with no decode of the destination and no encode after each draw.
  - A tile is encoded to 8-bit sRGB once, when it is written out: a table over the top 12 bits, correctly rounded,
    checked exhaustively against the exact encode in a test.
  - It is expected to be faster than blending through an 8-bit sRGB target as the GPU does (a decode and an encode
    per blended pixel, against none). Milestone 2 builds it and measures the other on one scene to confirm, and the
    notes keep both numbers.
  - The pictures are slightly better than the GPU's where draws overlap: there is no rounding to 8 bits between
    draws. They are held to the cross-backend tolerance, not to the GPU's bytes.
- **Sprites:**
  - The quad's four corners come from the row the way `sprite_common.slangh` computes them.
  - An axis-aligned quad (no rotation, the common case) is a rectangle fill that steps uv. A rotated one is a
    convex quad with edge functions in fixed point with 8 sub-pixel bits and the top-left rule. Its interior
    diagonal does not exist, so the two-triangle split costs nothing.
  - Coverage samples at pixel centres; varyings are affine (w is 1).
- **Sampling:**
  - Nearest, and linear with 8 bits of weight (the common hardware precision), with repeat addressing.
  - An sRGB texture decodes its texels to linear before filtering. A texture may keep a decoded 16-bit copy made at
    upload, if the numbers say it pays (a P2 option).
  - One mip, so no level is ever chosen.
- **The UI primitive** is `ui.slang` ported line for line:
  - The box: its signed distance, border, gradient, image and shadow; the clip; the premultiplied output.
  - `erf` and the per-glyph `pow`: the glyph's exponent is quantized to 64 steps and its coverage to 256, through
    a table. Its error against `pow` is measured in a test and has to stay under one 8-bit level.
  - Glyphs are the hot case: an axis-aligned, nearest-pixel copy with a coverage table.
- **The post pass:**
  - The buckets below the post layer go into a world target (linear, 16 bits a channel, the window's size).
  - Then the post program runs over every pixel of the window, as a tile job; then the rest draws over.
  - Without a CPU form of the game's post shader (or with game shaders cut out), the frame draws as the GPU does
    when the shader will not load: straight to the window, no post.
- **Game shaders** (decided: Slang's C++ unless the game overrides, and a fast path that cuts them out). A
  material that names a game shader draws, in this order of preference:
  1. **The game's own CPU shader**, if the game registered one under that name (`renderer_cpu_shader(e, "nebula",
     fn)`). It is a C++ function over a span of pixels with the varyings filled in. This is for a shader whose
     generated form is too slow, or one Slang cannot translate.
  2. **The shader compiled for the CPU.** kormake's shader rule gets a CPU variant that runs `slangc -target cpp`
     over the game's shader, wrapped by an engine-provided kernel (`sprite_cpu.slang`). The wrapper turns a tile's
     span of pixels into calls of the game's `fs_main` with the varyings filled in, and `discard` becomes a flag
     it reads. The generated C++ is compiled into the game's program (with the `fast` profile) and registered by
     the shader's name, found as the GPU backends find `<name>.spv`.
  3. **Its blend's engine slot**, which is the GPU's own rule for a shader that will not load.
  - **The fast path** cuts game shaders out on purpose, all of them or one by name: `RendererDesc.cpu_shaders`
    (`All`, `None`) and `renderer_cpu_shader_skip(e, "nebula")`, with frameloop's `--cpu-shaders none`. The
    materials then draw in their engine slots, and the post pass is left out.
  - The post pass's shader takes the same three steps, its wrapper `post_cpu.slang`.
  - Whether Slang's CPU target takes a fragment entry called from a compute-shaped wrapper (`discard`, the prelude
    it needs at run time) is the first thing milestone 1 finds out. If it cannot, step 2 is empty until it can, and
    steps 1 and 3 stand.
- **Presenting** (`rb_frame_acquire`, `rb_frame_present`): the window gives the backend a place to write and
  shows it:
  - `HostWindow::software_frame(&pixels, &pitch, &format)` / `software_present()`.
  - On SDL: the window surface (`SDL_GetWindowSurface`, its format honoured, `SDL_UpdateWindowSurface`).
  - On Win32: a DIB section blitted to the window (`SetDIBitsToDevice`, on the window's thread or with its DC). The
    window's thread already owns painting, and the backend hands it the frame.
  - Android (`ANativeWindow_lock` / `unlockAndPost`): a P2.
  - Apple platforms: out of scope (every supported Apple machine has Metal).
  - The tile buffers write straight into the window's pixels when its format is BGRA8 or RGBA8; otherwise through a
    converting copy.
  - Pacing: the window surface's vsync where the system gives it (SDL's `SDL_SetWindowSurfaceVSync`), so the
    display paces the frames as a swapchain's do. Where it does not (Win32's DIB), `RenderCaps::paced` is false
    and frameloop starts frames no closer than a display period apart, unless `--no-vsync` asks for every frame it
    can draw.
- **Headless:** with no window (`native_window` null and `backend = Cpu`), the frame draws into a buffer of
  `desc.width × desc.height`. `--screenshot` and `renderer_screenshot` read it as they read a swapchain. This is how
  tests take pictures with no GPU, no display and no driver.

### Krome and static frames

A tool's frame mostly repeats the last. Krome paces on demand, so an unchanged screen costs nothing already. When
one button's look changes, the whole window draws again on the GPU, which is cheap there. On the CPU it is not:
1920×1080 at overdraw 2 is four million blended pixels.

The tile hash makes the cost the change's, not the window's. Each tile's command list (rows, UI items, their bytes,
the textures' versions) hashes to a value. A tile that hashes as it did last frame keeps last frame's pixels.
Hover on one button redraws the few tiles under it.

### Fast code where it pays, without touching the sim

The rasterizer wants the compiler free to vectorize and fuse: `-O3`, contracted multiply-adds, no math `errno`, no
trapping, and on a few files reassociation. The engine builds everything with `-ffp-contract=off` (and cl's strict
equivalent), because kv's math has to give every platform the same bits. Three rules let the two live together:

1. **A code-generation profile, asked for by a module and spelled by the toolchain.**
   - A module says `codegen = "fast"` (decided: named profiles, chosen per module for now). Each toolchain says
     what that is:
     `-O3 -ffp-contract=fast -fno-math-errno -fno-trapping-math` on gcc and clang, `/O2 /fp:fast` on cl.
   - Today only toolchains set flags (`sandbox.lua`'s module fields have no `cxxflags`). This adds one field to
     modules and one table to toolchains.
   - **Never `-ffast-math` on a link line:** with gcc it links `crtfastmath.o`, which flushes denormals for the
     whole process at start-up and would change the sim.
2. **Fast code shares no inline math with the sim.**
   - An inline function from a shared header (kv's scalar math, `vec2`'s operators) compiled in a fast unit is a
     weak definition the linker may pick for every unit. The sim would then run on the fast copy.
   - The CPU renderer's fast units therefore include only `core/types.h` and their own headers. Their math is
     their own, in their own namespace.
   - A test holds the rule: a script lists the weak symbols of the fast objects and fails on any that the rest of
     the program also defines. Arcfire's replays are hashed with the backend linked (exit criterion).
3. **The instruction set is the platform's baseline: SSE2 on x86-64, NEON on arm64.**
   - Machines that need a software fallback are the ones least likely to have AVX2. Wider kernels (AVX2, perhaps
     AVX-512) are built as separate objects of the same source, under a define, and chosen once at start-up by
     `cpuid`.
   - The pixel math that decides a picture is integer or fixed point (blends, the sRGB tables, sampling weights),
     so each path gives the same bytes. Float is used only for geometry and the UI's distances, rounded to fixed
     point before it reaches a pixel. A test draws the gallery on every path the machine has and compares them
     exactly.

### Performance

Decided: each port reports its numbers; outliers are investigated, and whether any of them need work is decided
then. There are no targets in advance.

What every port measures, in a dev build, at 1280×720 and at 1920×1080, on one core and on all of them:

- kromedemo (its form and gallery pages) and kromescreens (every page; the 10,000-row status page with its steps
  sent every tick), including the cost of a hover that changes one button.
- arcfire's normal play under its bot, nebula and shimmer on, then with `--cpu-shaders none`.
- The gallery's ten scenes, each.
- `--bench ring --dial rank=56`, to know the ceiling. It is a GPU's job, and nothing is expected of it.

The profiler gets the backend's zones (sort, bin, raster, post, present), and `renderer_gpu_stats` reports them as
if they were the GPU's, so frameloop's line works unchanged. The numbers go in `docs/design/renderer-cpu.md`, a
table per port, with each outlier and what was found.

## Out of scope

- Apple platforms (Metal is everywhere they run).
- Switching backends while running: a lost device, or a GPU that fails mid-run.
- MSAA, mip maps, and any shader feature the engine does not use.
- 3D.
- Matching the GPU at stress loads.
- Making the GPU backends faster.
- A software path in `platform_hid` for anything but presenting.
- Running a game's shader through an interpreter.
- Passing over a CPU-type Vulkan device (lavapipe) for this renderer: Vulkan's device choice stays as it is, a CPU
  device scored lowest (decided: not yet).
- WARP, lavapipe or any other system rasterizer as a dependency. One may become an optional preference later, if
  it proves remarkably better than this renderer; nothing here may need one.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| SR1 | Two backends link into one program: each backend's core in its namespace; `render` owns `renderer.h`'s definitions, a switch on the renderer's backend byte; one bound backend costs what it does today | P0 |
| SR2 | Choosing: `RendererDesc.backend`, `rb_probe` for each GPU backend before the window opens, `--renderer`, `KV_RENDERER`, the reason for a fallback logged; `renderer_window_surface()` follows the choice | P0 |
| SR3 | A GPU backend's start-up is not fatal while another is bound | P0 |
| SR4 | kormake: a bound list passes the backends to `render` and each backend; the shader rule makes each bound backend's variant under names apart | P0 |
| SR5 | `renderer-cpu`: the contract met; memory buffers, the ring and garbage as the core has them; two frames in flight; `renderer_caps` honest (`backend = "cpu"`, `ui = true`) | P0 |
| SR6 | The sort and cull on the CPU, the GPU's semantics exactly (buckets, row order, keys, the 2,048 cap); `--check-sort` verifies it | P0 |
| SR7 | Sprites: placement, interpolation, layer scroll and anchors as `sprite_common.slangh`; fixed-point coverage with the top-left rule; nearest and linear sampling with repeat; sRGB decode before filtering; tint and the three blends in 16-bit linear tiles, encoded to sRGB8 once | P0 |
| SR8 | Tiles: binned commands in draw order; tiles rasterized as a job batch; the picture independent of thread count | P0 |
| SR9 | The UI primitive as `ui.slang`: clip, rounded box, border, gradient, image, shadow, glyph coverage; premultiplied blending; tables measured against the exact functions | P0 |
| SR10 | Presenting on SDL (Linux) and Win32: a frame to write into and to show; resize; minimise | P0 |
| SR11 | Headless: a renderer with no window draws into a buffer; screenshots read it; the gallery runs headless in `kormake test` | P0 |
| SR12 | The post pass: a world target, the post program over the window, the layers above drawn over | P1 |
| SR13 | Game shaders: the game's registered CPU shader first, then Slang's C++ through an engine wrapper, then the blend's engine slot; `--cpu-shaders none` and per-shader skipping as a selectable fast path; the post pass the same | P1 |
| SR14 | kormake `codegen` profiles, named, chosen per module (a module field, a toolchain table); the CPU renderer's kernels and the generated shaders built fast; never `-ffast-math` at link | P1 |
| SR15 | The inline-math rule and its check (weak symbols of fast objects); arcfire's replays hashed with the backend linked | P0 |
| SR16 | The instruction-set baseline; wider kernels chosen at start-up; every path the same bytes, tested | P1 |
| SR17 | Tile hashing: an unchanged tile is not drawn again | P1 |
| SR18 | The gallery: within tolerance of Vulkan's references, exact against references/cpu; kromedemo's and kromescreens' pictures within tolerance of Vulkan's | P0 |
| SR19 | `tools/render-gate.sh linux cpu` (and windows): builds, tests, the gallery exact, the sort check, arcfire's demo under `--renderer cpu` | P0 |
| SR20 | Each port's numbers as "Performance" lists them, the profiler's zones, `renderer_gpu_stats` filled; outliers investigated and written down | P1 |
| SR21 | Android presenting through `ANativeWindow` | P2 |
| SR22 | `docs/design/renderer.md` and `docs/design/renderer-cpu.md`: the seam's new rule, the backend's design, its numbers | P1 |

## Milestones

Each leaves every GPU backend building and drawing exactly what it drew, its gate passing.

1. **The seam, and an answer from Slang** (SR1 to SR5, SR10 in part, SR11 in part).
   - The namespaces, the dispatcher and the X-macro list of the API.
   - kormake's bound list.
   - `rb_probe` and the soft start-up.
   - A CPU backend that clears the window, presents on SDL and Win32, and takes a headless screenshot.
   - The GPU gates on all three machines, and the bench beside its baseline: the seam must cost nothing.
   - In parallel, a spike: one gallery shader through `slangc -target cpp` and the wrapper, compiled and called. Its
     result says whether SR13's second step can be filled before milestone 4.
2. **Sprites** (SR6 to SR8, SR15, SR18 in part).
   - The sort, the bins, the tiles, the sampler, the blends in 16-bit linear tiles, the sRGB encode; the 8-bit
     target measured on one scene beside it.
   - The gallery's nine sprite scenes within tolerance of Vulkan's, headless in `kormake test`.
   - The weak-symbol check and arcfire's replay hashes.
3. **The UI and Krome** (SR9, SR17, SR18).
   - The UI primitive and the gallery's `ui` scene.
   - kromedemo and kromescreens under `--renderer cpu`, and their pictures against Vulkan's.
   - Tile hashing and the hover number.
4. **The post pass and game shaders** (SR12, SR13).
   - The world target and the post program.
   - The game shaders' three steps: registered overrides, Slang's C++, the engine slot; `--cpu-shaders none`.
   - The gallery's `shaders` scene; arcfire's nebula, dissolve and shimmer.
5. **Fast, and close** (SR14, SR16, SR19 to SR22).
   - kormake's named profiles, chosen per module.
   - The SSE2 baseline and the AVX2 path, the same bytes on both.
   - Each port's numbers and their outliers, the gate script for the CPU renderer, the docs. The full GPU gate once more on each machine.

## Decided

Nov 15, in review of the first draft:

1. **Two backends share a build through namespaces and one switch at the API.** The renderer's API is not the hot
   path, and compilers handle a switch like this well. The seam's first rule becomes "one definition per backend
   namespace". Separate executables, a system rasterizer posing as a GPU, and function-pointer tables were
   considered and set aside.
2. **Game shaders run through Slang's C++ unless the game overrides one** with a hand-written CPU shader
   registered under its name. A selectable fast path cuts game shaders out (all, or one by name), drawing their
   materials in their engine slots.
3. **The pixel format is whichever is faster.** 16-bit linear tiles are expected to win, and milestone 2 confirms
   it by measuring.
4. **A CPU-type Vulkan device is not passed over, not yet.**
5. **No hard dependency on WARP or lavapipe.** Either may be an optional preference later, if remarkably better.
6. **Each port reports its performance.** Outliers are investigated, and whether any need work is decided then.
7. **kormake's code-generation profiles are named, and selected per module** for now.
8. **The module is `renderer-cpu`**; `--renderer gpu|cpu|auto`.
