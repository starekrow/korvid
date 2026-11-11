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

## Open, not blocking

- **Read-only buffers are `device`, not `const device`.** Metal warns "writable resources in
  non-void vertex function" for the sprite vertex shader, because Slang emits
  `StructuredBuffer` as writable. A warning today; it may cost Metal optimizations, and a
  stricter compiler could make it an error. Slang may have a way to mark them read-only for
  Metal; otherwise it is a Slang issue to file.
- **`kMaxTextures` lives twice:** `renderer_internal.h` and the array in
  `sprite_fragment.slangh`. The reflection check above should compare the array's length too.
- **The frame loop asks for a Vulkan window** (`frameloop.cc` sets `WindowSurface::Vulkan`);
  it must ask for what the bound renderer needs, Metal on the Apple family.
- **What a frame costs,** argument-buffer residency included, is milestone 4's to measure
  against the Vulkan numbers.
