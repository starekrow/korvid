# Renderer

Nov 7, 2026 (game shaders and the full-frame pass: Nov 10; the core and the backends: Nov 13). The sprite renderer
after slice 3: what lives on the GPU, what a frame does, and the rules the pieces follow. The mechanisms are described
as Vulkan has them, where they were first built; the last section says what is shared between the three backends and
what each writes for itself.

## Principle

**The CPU never visits a sprite to draw a frame.** `renderer_apply` writes rows into a
persistent table as deltas arrive; a frame copies the rows that changed, lets compute sort
and cull the table, and records one indirect draw per run of buckets it knows to be
non-empty. A frame in which nothing moved uploads nothing and draws from the same sorted
list it would build again.

## The tables

- **Sprite rows** (`GpuSprite`, 112 bytes, device-local): slot-addressed by the render
  world's pool index, so `sprite_of_entity` is the only lookup and the CPU mirror
  (`Pool<RenderSprite>`) and the GPU row share an index. A row holds the previous and
  current placement, the tick stamp of its last `Set`, material row, uv, size, tint,
  layer and a live flag. The table grows by doubling (every row re-marked dirty, the old
  buffers retired through the garbage queue).
- **Material rows** (`GpuMaterial`, 64 bytes): tint, two float4 parameters, the bindless
  texture slot resolved when the row is written, the blend mode. The handle's index is
  the row; row 0 is the default material. A dirty bitmap over the rows drives uploads.
- **Sort scratch**: `bucket_of[row]`, `block_hist[block][bucket]`, `sort_meta` (bucket
  totals, bucket starts, the visible count), `indirect[bucket]`
  (`VkDrawIndirectCommand`), `sorted[]` (row indices in draw order).

## Ticks without a copy

The `Tick` delta increments a counter and touches no row. A `Set` on a row stamped with
an earlier tick first moves the row's current placement into its previous one, then
writes and stamps it. The vertex shader and the cull interpolate rows stamped with the
frame's tick by the view's alpha and show the rest at their current placement: a row that
was not set this tick did not move. Only rows that were set are dirty, so the upload is
proportional to what moved.

## Dirty rows

A bit per row. The frame walks the words, joins runs separated by fewer than 32 clean
rows (uploading a clean row again is cheaper than another copy region), stages each run
into the frame ring and records one `vkCmdCopyBuffer` per run. A dead slot in a run is
written as a zero row.

## The sort

A sprite's bucket is `(clamp(layer, -128, 127) + 128) * 8 + slot`: 2,048 buckets, layer
major, so ascending bucket order is draw order. The slot is the material's pipeline: 0 to 2
the engine's blends, 3 to 7 game shaders (slice 6; before it, 768 buckets of layer and
blend). The CPU keeps an exact count per bucket, updated on `Set` and `Remove` (and when a
material's slot changes, by walking its sprites), so it knows the non-empty buckets without
looking at rows.

Four compute passes over blocks of 1024 rows (256 threads, 4 rows each):

1. **count**: cull each live row's bounding circle against the view rect, write its
   bucket (or "culled") to `bucket_of`, build the block's histogram in shared memory and
   write it to `block_hist`.
2. **scan**: a thread per bucket (eight buckets a thread) runs over the blocks, turning counts into each
   block's offset within the bucket and producing the bucket's total.
3. **bucket scan**: one group scans the 2,048 totals into bucket starts and writes an
   indirect command per bucket: 6 vertices, the visible count, the bucket's start as
   `firstInstance`.
4. **scatter**: per block, the local histogram and its scan give each bucket's first
   local rank; a bitonic sort of the block's `(bucket << 10 | local index)` keys makes
   rows of one bucket contiguous and in row order; each row lands at
   `bucket start + block offset + rank`.

Order within a bucket is row order, every frame, so overlapping sprites on one layer
never swap, unless the bucket holds keyed sprites (slice 5). The CPU counts keyed rows per
bucket next to the plain counts; after the scatter, a fifth pass, **cs_key_sort**, runs one
group per bucket with keyed rows (the bucket in the push block's `key_bucket`): it loads
the bucket's range of `sorted[]` (up to 2,048 rows, 16 KB of shared memory), computes each
row's key (`Key`: its `order`; `Feet`: minus the interpolated y plus `order`; `Row`: 0) as
an order-preserving uint, bitonic-sorts (key, row) pairs and writes the rows back. Ties are
broken by row, so the result is as stable as the scatter's. A bucket past 2,048 visible
rows sorts its first 2,048. `--check-sort` snapshots every row's key on the CPU when it
records the check and verifies (key, row) order, allowing near ties either way. `--check-sort` reads the outputs back every 30th frame and verifies them.

## The draws

The CPU walks its bucket counts and records a `vkCmdDrawIndirect` per run: from a
non-empty bucket through the last non-empty bucket of the same slot before one of
another slot intervenes, with one pipeline bind. Buckets in between with nothing in them
draw zero instances. Without `multiDrawIndirect` it is one call per non-empty bucket.
The vertex shader reads `sorted[base + instance]`; Slang's `SV_InstanceID` excludes the
base instance, so `SV_StartInstanceLocation` adds it back.

## The frame ring

One host-visible buffer. A frame's allocations are contiguous in ring order even when
they begin before the previous frame's fence passes (texture uploads land from the asset
pump, before the frame starts); releasing a frame moves the tail to where that frame
ended. `used` counts the bytes in flight with wrap gaps, so head == tail is unambiguous.
A frame that took nothing leaves the tail alone. Growth retires the old buffer through
the garbage queue; spans already handed out keep pointing at it, which is why a span
carries its buffer.

## Textures

A texture upload creates the image at once, stages the pixels into the ring (or a one-off
staging buffer, retired with the frame, when they would not fit in half the ring) and
queues a `PendingUpload`. The next frame records the copy and its barriers ahead of
everything else. A slot's descriptor is rewritten in each frame's set once that frame's
fence has passed. There is no upload command buffer and no upload fence.

## Fences

Two frames in flight, one fence each. Everything deferred (buffers, images, views,
memory, swapchains, semaphores, pipelines) is queued on the frame being recorded and
destroyed when that frame's fence passes; fences on one queue pass in submission order,
so a frame's fence covers what earlier frames referenced too. Swapchain recreation builds
the new chain with `oldSwapchain` set and retires the old one the same way. The only
`vkDeviceWaitIdle` is in `renderer_destroy`.

## Dev facilities

`--screenshot FILE` copies one presented frame to a host buffer and writes a binary PPM;
`--check-sort` verifies the sort. Both wait on their frame's fence. The Mesa screenshot
layer crashes on the first acquire after a fenced recreation (it does not expect the old
swapchain to outlive the new one's creation); the built-in path replaces it.

## GPU timing and captures

In a profiling build, while the profiler records, each frame writes five timestamps
(its start, after the uploads, after the sort, after the drawing, its end) and one
pipeline-statistics query around the sort and the drawing (vertex, fragment and compute
shader invocations), and reads them back when its fence next passes: no wait, no stall.
They become the profiler's `gpu` track and `renderer_gpu_stats` (the frame, its passes in
milliseconds, the invocations, and overdraw: fragments over the frame's pixels), which the
frame loop logs beside its fps line. `VK_KHR_calibrated_timestamps` (or the EXT) aligns
the spans with the CPU's zones in a trace. A frame remembers whether it wrote its queries,
so the profiler can start and stop mid-run.

Debug-utils names (the sprite tables, the material rows, the ring, the layer offsets,
every texture by its name, the pipelines by blend and entry point, the swapchain images)
and labels (upload; sort with count, scan, bucket scan, scatter and key sort; draw, with a
label per run of buckets naming its layers and blend) are on whenever the instance has the
extension, in every build: without a listener they cost nothing, and they make a capture
read like the code. For depth: RenderDoc for one frame (per-draw timings, an overdraw view,
shader stepping); on AMD under Linux, `MESA_VK_TRACE=rgp` writes Radeon GPU Profiler
captures straight from RADV.

## Samplers

Set 1 holds two samplers, nearest and linear (slice 5), and the material row's `filter`
picks one per draw: the vertex shader passes it flat to the fragment shader, which samples
`gSamplers[filter]`. Pixel art stays on nearest; font atlases and anything drawn at a scale
of its own use linear.

## Game shaders

Slice 6. A material may name a game's fragment shader (`MaterialDesc::shader`, a material
file's `"shader"`): a SPIR-V file in `<exe>/shaders/`, built by kormake's shader rule from
the game module's `shaders/*.slang`, whose include path holds every module's `shaders/` so
the game includes `sprite_fragment.slangh` by name. That header is the whole interface: the
varyings (uv, tint, world position, the quad's local 0 to 1, the texture, sampler and
material row), the bindings, and `sprite_texel`; the game defines `fs_main` and nothing
else. The vertex shader, the sort and the blend states stay the engine's; the push block
carries `time`, the render time in seconds, for what animates without the sim.

Pipelines are by slot. The first material naming a (shader, blend) pair loads the module
and builds a pipeline for it in a free game slot, five of them; a material whose pair has a
slot reuses it, and the material row records its slot for the sort. A shader that will not
load (missing, invalid) is logged once and its materials draw in their blend's engine slot,
as does a sixth pair. The cost of the eight slots is the sort's larger histogram: 2,048
buckets against 768 adds about 0.015 ms a frame to the sort under arcfire's `--bench
ring --dial rank=56` (0.19 to 0.21 ms on a Radeon 780M), and nothing to the draws, which stay one per run.

## The full-frame pass

Slice 6. `renderer_post_layer(r, layer, shader)` splits the frame at a layer: the buckets
below it draw into the world's target, a color image of the swapchain's size and format
(so the sprite pipelines draw into it unchanged), which then turns readable; the window's
rendering begins with one triangle through the game's post shader (the engine's `vs_post`
from `post.spv`, the game's `fs_main`, no blending), and the buckets at and above the layer
draw over it as before. The shader's interface is `post_fragment.slangh`: `post_world(uv)`
(linear, clamped at the edges), up to 16 float4s the game sets with `renderer_post_params`,
and a push block with the target's size, the render time, the tick and the interpolation
point. The pass has its own pipeline layout (set 0: the image, the sampler, a host-visible
parameter buffer per frame in flight, the set rewritten as its frame is recorded), so the
sprites' sets and push block are bound again after it.

One target serves both frames in flight: commands on one queue run in submission order, so
the barrier that opens a frame's world (from the last frame's fragment reads, the old
contents discarded) is all the sharing needs. The target is made on the first frame that
needs it and again when the swapchain's size or format moves, the old one retired through
the garbage queue. A format the device cannot both draw into and sample with a linear
filter, a shader that will not load or link, or no shader at all: the frame draws straight
to the window as it did before, and none of the pass's objects exist until a shader is set.
Its cost on a Radeon 780M at 1280 x 720: about 0.1 ms of GPU a frame and one more layer of
overdraw.

## The UI primitive

Krome slice 1 (docs/PRDs/Krome-Slice-1-PRD.md). A path beside the sprites for what they do
badly: thousands of small rectangles with a look each, in painter's order, clipped, in the
window's pixels (`korvid/render/ui.h`). A `UiList` is built on the CPU in the words the
shader reads (a box of 22, a glyph of 9, the clips they name) and handed over for a layer
with `renderer_ui_set`; the render world keeps each layer's region, packed and in order,
with a version that moves on every change (`rw_ui_*`, tested in `render_tests`).

Each frame slot holds the lists in one host-visible buffer of its own, which the core
rewrites only when the world's version has moved past the slot's, before anything is
recorded (a bigger buffer rewrites the slot's bindings, which a recording command buffer
must not see). A still list costs a draw call and nothing else. The draw is one instanced
call per layer, after that layer's sprite buckets and before the next layer's, in
whichever pass the layer is; it reuses the sprites' bindings, and the push block's spare
words carry the list's region and count (`ui_base`, `ui_count`; `pad_t0`, `pad_t1` in the
shaders, so the sprite shaders compile to the same bytes). `shaders/ui.slang` does the
rest: rounded corners per corner by signed distance, a border inside the edge, a two-stop
gradient, a shadow as an erf of the distance, an image or a glyph's coverage, a clip by
discard, colors decoded from sRGB, output premultiplied.

On Vulkan the buffer is set 0's binding 8. Metal and D3D12 have stubs for now: they
answer that they have no buffer, and the core draws no UI there; their `KV_RB_UI` is 0, so
`renderer_caps(r).ui` is false (the gallery leaves its `ui` scene out, Krome warns once),
and `ui.slang` compiles to placeholder entry points under `KV_TARGET_METAL` and
`KV_TARGET_D3D12`. A backend that takes the primitive up defines `KV_RB_UI 1`, makes its
`rb_ui_buffer` and `rb_ui_draw` real, and drops the placeholder branch.

## The core and the backends

Slice 9 (docs/PRDs/Engine-Slice-9-PRD.md). Three backends draw this renderer: `renderer-vulkan` (Linux, Windows,
Android), `renderer-metal` (macOS, iOS) and `renderer-d3d12` (Windows). Each was first a copy of the one before it with
the calls changed. What they had in common is now written once, in `render`, in two halves, and a backend holds only
what differs. The core shares structure; it does not hide the backends. There is no hardware abstraction here, no
table of function pointers and no template over a backend.

**The linked half** (`korvid/render/world.h`, `render/src/world.cc`) needs no GPU type, so it is ordinary code that
links with any backend and is tested without one (`render_tests`). `RenderWorld` is the sprites and their rows, the
materials' records, the dirty bitmaps, the bucket and keyed counts, the layers' offsets and the tick. Over it:
`rw_apply` (the whole of `renderer_apply`), the buckets, material rows and rebinding, the dirty-run and draw-run walks,
the push block (with a flag for clip spaces whose y is up), the key snapshot and the sort verifier.

**The compiled-in half** (`korvid/render/core/`: `layout.h`, `core.h`, `core.inl`, `frame.inl`) is built inside each
backend's library by a two-line source, `src/render_core.cc` (`.mm` on Metal), after that backend's
`renderer_internal.h`, so it sees the backend's types whole. It holds the functions of `renderer.h` (all but create,
destroy, get, resize and set-surface, which are each backend's), the pools and their handle kinds, the default texture
and material, the game shaders' slots, the frame ring, the garbage queue, the CPU's half of `--check-sort` and
`--screenshot`, and the frame.

**The contract** is `core.h`, and it is the whole of what a new backend has to meet:

- *Five values*, as `#define`s: `KV_RB_NAME` (the log channel), `KV_RB_CLIP_Y_UP`, `KV_RB_SHADER_EXT`, `KV_RB_SURFACE`,
  `KV_RB_FRAME_SCOPE` (empty, or Metal's `@autoreleasepool`). The core uses what they are worth and never asks which
  backend it is: there is no `#if` on a backend's name in the core.
- *Two types*: `RbBuffer` (a struct on Vulkan and D3D12, the `MTLBuffer` itself on Metal) and `RbGarbage` (one thing
  to destroy once the frame that last used it is done).
- *Five records completed by deriving*: `Texture : TextureCore`, `GamePipe : GamePipeCore`, `PostPass : PostCore`,
  `FrameInFlight : FrameCore`, `Renderer : RenderCore`. Plain data on plain data, nothing virtual. The core's functions
  use the core's part and the members the contract names (`textures`, `game_pipes`, `post`, `frames`, `gpu_prof`, a
  frame's `layers`); a backend's files see one struct and spell their own fields as they always did.
- *The `rb_` functions*, the same names and signatures in all three, at the height of a pass and never of an API call.
  Resources: a buffer's mapping and retirement, the ring's buffer, one piece of garbage destroyed, a texture's image
  made, staged and retired, a game pipeline loaded, the post shader set, the tables made. The frame: wait, write a
  slot, acquire, submit, present, finish; and to record it: begin, the uploads (begin, copy a run of rows, end), the
  sort (run, read back, end), the post target, a pass (begin, the post triangle, bind the sprites, draw a run of
  buckets, end), read the window back, end.

**The frame** (`frame.inl`) is the order of work the three agree on, written once: wait for the slot's last frame,
release its span of the ring and its garbage, grow the tables, write the layers and the changed texture slots, acquire,
record, submit, present, and the dev stall for a screenshot or a sort check. The core stages the dirty rows and walks
the draw runs; the backend copies a run (`rb_copy_rows`) and issues one (`rb_draw_run`), which is where Vulkan's
multi-draw, Metal's loop and D3D12's `ExecuteIndirect` stay. The sort's order is `rc_sort_steps`: four kernels with
their labels and group counts, then `rw_next_keyed` for the buckets the key sort visits; each backend dispatches them in
a loop with its own barrier between. What a backend must carry from one step to the next (the image's index; on Metal
the command buffer, the drawable and the open encoder) lives in its `FrameInFlight`.

**The rules of the seam**, which are what keep it from growing into the thing it replaced:

1. No templates over the backend, no virtual functions, no tables of function pointers. A name has one definition in
   a build.
2. A function is the core's when its bodies in the backends are the same once the names are normalised. If they are
   only alike, it stays in the backends (the post pass's objects, the profiler, the swapchain, the binding model,
   barriers and resource states).
3. The core does not reach into what a backend added to a record, other than the members the contract names. The
   backends read and write the core's fields freely.
4. A piece goes in the linked half when it falls out as plain data in, plain data out; in the compiled-in half when
   making it backend-blind would take a handle, a callback or a copy it does not need. If it has to be argued into
   either, it is in the wrong one.
5. Names: `rw_` the linked half, `rc_` the compiled-in half, `rb_` a backend.

**What it came to** (`tools/render-overlap.sh`, main at the slice's start against its end):

| | Before | After |
| --- | --- | --- |
| Lines in the three backends' `src/` | 9,712 | 6,922 |
| Distinct code lines identical in all three | 686 | 194 |
| The CPU's `record` zone, arcfire `--bench ring --dial rank=56`, Vulkan on a Radeon 780M | 0.075 ms | 0.074 ms |

The PRD asked for the identical lines to fall under 60, counted the same way. They did not, and the count is the wrong
measure of the end: of the 194, 26 are includes and preprocessor lines, 27 are the contract's own signatures (identical
because the contract says so), and 88 more are under thirty characters (`f64 t = time_now();`, a loop header, a
`PostPass& p = r->post;`). What is left that could still move is the common skeleton of `renderer_create` and
`renderer_destroy`, which differ in every step between their first and last lines.

`tools/render-gate.sh` is what held the pictures still while this was done: on each backend's own machine, the three
configs built, the tests, shader sources and compiled shaders unchanged, the gallery exact against that backend's
references, the sort checks, arcfire under validation, and the bench numbers beside the baseline's.
