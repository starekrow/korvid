# The Metal renderer: the shader spike

Nov 10, 2026. Slice 7, milestone 2 (docs/PRDs/Engine-Slice-7-PRD.md, R7): before any of
`renderer-metal` is written, does the one set of Slang sources the Vulkan renderer uses
compile for Metal, and how does each thing that Vulkan and Metal do differently map?
Measured on an M3 Max (Apple9, argument buffers tier 2), macOS 26.7, Slang 2026.22, every
file compiled to MSL by `slangc -target metal` and then by the OS's Metal compiler
(`MTLDevice newLibraryWithSource:`), with a compute pipeline built for every kernel.

**Result: go.** Every shader in the engine and in arcfire compiles for Metal from the same
sources: the sprite vertex and fragment shaders, the post vertex shader, the five sort
kernels, and arcfire's dissolve, nebula and shimmer. One header changed, under a define the
Metal shader rule sets; with it unset, Slang's output for every file is byte-identical to
before, so the Vulkan build cannot have moved.

## How each difference maps

| Vulkan (SPIR-V) | Metal, as Slang emits it | What the backend does |
| --- | --- | --- |
| `Texture2D gTextures[]`, variable count, last binding, `NonUniformResourceIndex` | **Not expressible.** Slang makes it a flexible array member, then a by-value parameter; MSL refuses both. | Under `KV_TARGET_METAL`, `sprite_fragment.slangh` declares `ParameterBlock<BindlessSet>` (samplers[2], textures[1024]) and keeps the names `gTextures` and `gSamplers` as macros, so game shaders compile unchanged. Slang lowers it to an argument buffer: `constant BindlessSet*` at one buffer index. Tier 2 layout: one 64-bit `MTLResourceID` an entry, samplers at bytes 0-15, textures at 16-8207. The backend writes `gpuResourceID`s straight into a shared buffer (no argument encoder) and makes every texture resident for the pass (`useResource`, or a residency set). `NonUniformResourceIndex` is dropped, correctly: divergent indexing of an argument-buffer array needs no marker on Apple GPUs. Samplers must be made with `supportArgumentBuffers`. |
| A fixed `Texture2D gTextures[1024]` bound to the stage | Fails: "cannot reserve texture resource locations at index 0". Direct binding holds a few dozen textures a stage, not 1024. | Tried and rejected for the above. |
| `SV_InstanceID` (excludes the base instance) + `SV_StartInstanceLocation` | `[[instance_id]]` includes the base on Metal; Slang compensates and emits `base + (iid - base)`. | Nothing: `gSorted[base + instance]` fetches the right row. Indirect draws use `MTLDrawPrimitivesIndirectArguments`, whose fields are `VkDrawIndirectCommand`'s in the same order, so the buffer the sort's `cs_bucket_scan` writes is drawn from as it is. No multi-draw on Metal: one `drawPrimitives(indirectBuffer:)` per run of buckets, Vulkan's existing fallback. |
| `[[vk::push_constant]] ConstantBuffer<PushData>` (112 bytes), and the post pass's | `constant PushData*` at buffer 0. | `setVertexBytes`/`setFragmentBytes`/`setBytes` at index 0 (under 4 KB, no buffer to manage). |
| `StructuredBuffer`, `RWStructuredBuffer` with `[[vk::binding(n, set)]]` | `device T*` at a buffer index of Slang's choosing (below). | Buffers in shared storage, one `setBuffer` each. |
| `groupshared`, `InterlockedAdd` on it, `GroupMemoryBarrierWithGroupSync` | `threadgroup` arrays, atomics, `threadgroup_barrier`. | Nothing: all five kernels build pipelines (up to 1024 threads a group; the sort uses 256; 32-wide SIMD groups). Their threadgroup memory, at most 16 KB in `cs_key_sort`, is inside the 32 KB limit. |
| A game's post shader over `post_fragment.slangh` (`Texture2D gWorld`, a sampler, the params) | `texture(0)`, `sampler(0)`, the push block at buffer 0, params at buffer 1. | Bound directly; one texture is no trouble. |

## Binding indices

Slang numbers a stage's buffers in the order their globals are declared through the
include chain, whether the entry point uses them or not: in the sprite shaders the push
block is 0, layers 1, materials 2, the bindless set 3, sprites 4, sorted rows 5; every game
fragment shader includes the same headers and gets the same numbers (arcfire's nebula never
reads the layers and still leaves 1 for them). The sort kernels have their own order
(sprites 2, materials 3, sorted 4, ...), a different pipeline and no conflict. `[[vk::binding]]`
does not reach Metal.

That stability is real but implicit: moving one declaration in a header renumbers
everything after it, silently. So the Metal shader rule also writes Slang's reflection
(`-reflection-json`) beside each library: every parameter by name with its kind and index,
the same numbers the MSL holds. The backend keeps fixed constants for its layouts, as the
Vulkan backend does, and a dev build checks them against the reflection when it makes a
pipeline, failing by name if a header has moved one.

## What the build changes (milestone 3)

- The `slang` module's shader rule takes its target from the toolchain instead of fixing
  `spirv`: on the Apple family `-target metal -DKV_TARGET_METAL=1`, writing
  `bin/shaders/<stem>.metal` and `<stem>.refl.json`, then `xcrun metal` and `metallib`
  making `<stem>.metallib`, so a broken shader fails the build, not the game. That needs
  Xcode's Metal Toolchain, which Xcode 26 downloads separately
  (`xcodebuild -downloadComponent MetalToolchain`), as Windows needs the Vulkan SDK for
  `slangc`. Until it is installed, the backend can compile the `.metal` source at load time
  (`newLibraryWithSource:`, what this spike used), with the errors at run time.
- Slang predefines nothing for its Metal target (`__METAL__` and `__TARGET_METAL__` are both
  unset), so the define is the rule's.
- Slang's macOS release has no `libslang-glslang`, so it cannot emit SPIR-V on a Mac. Nothing
  here needs SPIR-V; it only means a Mac cannot check the Vulkan shaders' build.

## The backend (milestone 3)

`modules/renderer-metal` is renderer-vulkan's CPU side ported as it is (the render world, the
dirty rows and their runs, the bucket counts, the ring, the materials, the game pipes, the
sort check), with Metal's calls where Vulkan's were:

- **A frame** is a command buffer, retained and waited on when its slot comes round again,
  where Vulkan waits on a fence; its garbage and its span of the ring go then. The drawable
  comes from the layer (`nextDrawable`, three of them, `displaySyncEnabled` for vsync); a
  window with no area has none, and the frame still sends its uploads.
- **Uploads** are a blit: textures from the ring into private textures, dirty rows from the
  ring into the private tables. Metal tracks hazards across encoders and command buffers on
  one queue, so a frame's copies wait for the frame before it to stop reading, as Vulkan's
  barriers made them.
- **The sort** is one serial compute encoder: its dispatches run in order, each one's writes
  seen by the next. Every dispatch names its threadgroup size (256): MSL has no `numthreads`.
- **The draws** set the push block with `setVertexBytes`/`setFragmentBytes`, bind the five
  sprite buffers as one range at the indices above, declare every live texture resident
  (`useResources`), and draw each non-empty bucket from the indirect buffer.
- **Clip space.** `ortho2d` maps onto Vulkan's clip space, y down; Metal's is y up. The
  backend negates the y row of both projections it pushes (`metal_clip`), so the shaders and
  the math library stay as they are. Checked by eye: the overlay's text is upright at the
  window's top left, the balls' highlights where their texture has them.
- **Ownership** is manual (no ARC): records holding Metal objects live in the engine's pools,
  which copy bytes. Every object is made with a +1 reference and released at once or after
  the frame that last used it, like Vulkan's handles.
- **Validation** is the environment's: `MTL_DEBUG_LAYER=1` (and `MTL_SHADER_VALIDATION=1`)
  before the process starts; the renderer logs whether it is on.
- **Screenshots** came forward from milestone 4 (they are a blit from the drawable, which a
  dev or debug build makes readable): without them nothing showed which way up the picture
  was.

The shaders moved: everything in `renderer-vulkan/shaders/` is now `render/shaders/`, since
the interface a game's shaders include (`sprite_fragment.slangh`, `post_fragment.slangh`)
is the render interface's and must be on the include path whichever backend is bound, and
both backends compile the same engine shaders. kormake learned to stage a header-only
module's shaders; the Linux build's Ninja file changed only in where they are read from.

Measured on an M3 Max, `testbed` at 1920x1080 under API validation: 12 to 13 sort
checks a run in debug, dev and ship, all passing (culled rows included); no validation
messages; 96 to 121 fps with vsync on a 120 Hz panel, 350 to 500 without.

## Parity (milestone 4)

- **The full-frame pass** (`mt_post_pass.mm`) is renderer-vulkan's: the layers below the post
  layer draw into a private target of the drawable's size and format, the window's pass opens
  with one triangle through the game's post shader, the layers above draw over it. No layouts
  to move the target between; Metal orders the world's writes before the post pass's read and
  that read before the next frame's writes. `post.slang` makes the triangle's clip-space y for
  Metal under `KV_TARGET_METAL` (uv (0, 0) is the window's top left on both backends; Slang's
  output without the define is byte-identical). The game's post shader's bindings are checked
  against its reflection like the sprite shaders'.
- **Read-only bytes cannot meet a writable argument.** The post parameters went first as
  `setFragmentBytes`; Metal's validation refused them, because Slang emits a `StructuredBuffer`
  as a writable `device` pointer and bytes are read-only. They go in a small shared buffer per
  frame in flight, as Vulkan's do. (The same emission is behind the sprite vertex shader's
  "writable resources" warning: still a Slang matter.)
- **GPU timing** (`mt_gpu_profile.mm`): the frame's start and end are its command buffer's
  (`GPUStartTime`, `GPUEndTime`); inside them, timestamps at the end of the upload blit, the
  sort and the last draw (Apple GPUs sample at stage boundaries only, and the M3 Max has the
  timestamp counter set alone). Two measurements shaped it: an encoder with nothing to do
  writes no samples, and an unwritten sample still holds what an earlier frame wrote there,
  so a sample outside the command buffer's window is not this frame's and its pass is an
  empty span; and the samples, `GPUStartTime` and `sampleTimestamps:gpuTimestamp:` are one
  clock in nanoseconds, put on the engine's by an offset. renderer_gpu_stats and the
  profiler's gpu track get the frame, the sort and the draw; Apple GPUs have no statistics
  counters, so invocations and overdraw stay zero. Orbit at 2560x1440: 1.7 ms a frame, the
  sort 0.25, the draws and the post pass 1.45.
- **The gallery** (`gallery`, compared by the `picture` tool) draws nine fixed scenes at
  960x540 pixels on any display and compares two sets within a tolerance; the Metal set is
  committed as PNGs (`examples/gallery/references/metal/`). Run to run on one GPU the
  pictures are identical; the tolerance waits for the first Vulkan set.
- **Vsync** holds 120 Hz on the ProMotion panel once a run is a second old (rocks, arcanum,
  arcfire, the testbed with and without `--check-sort`). Milestone 3's near-100 fps did not
  come back and was not explained; nothing since touched the frame's pacing.

## Open, not blocking

- **Read-only buffers are `device`, not `const device`.** Metal warns "writable resources in
  non-void vertex function" for the sprite vertex shader, because Slang emits
  `StructuredBuffer` as writable. A warning today; it may cost Metal optimizations, and a
  stricter compiler could make it an error. Slang may have a way to mark them read-only for
  Metal; otherwise it is a Slang issue to file.
- **`kMaxTextures` lives twice:** `renderer_internal.h` and the array in
  `sprite_fragment.slangh`. The reflection check above should compare the array's length too.
- **What a frame costs against Vulkan's** on comparable work waits for numbers from the same
  scenes on both; arcfire's performance notes take the macOS ones.
