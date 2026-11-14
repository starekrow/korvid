# Engine Slice 9 — The Android 2022 baseline

Nov 13, 2026 · @starekrow

> Written before "One render core" took the number: that slice is Slice 9 (Engine-Slice-9-PRD.md) and this one follows it. Its extraction of a `render_world` module, and R11, are done there, differently: `renderer-vulkan11` is written against the core's contract (`korvid/render/core/core.h`), and `vulkan-common` is what the two Vulkan backends share beyond it.

## Goal and exit criteria

Slice 8's Android runs on a phone whose driver offers Vulkan 1.3 and full descriptor indexing, and refuses every other. Slice 9 adds a second Vulkan backend, `renderer-vulkan11`, that asks for nothing beyond what Khronos's `VP_ANDROID_baseline_2022` profile guarantees, so a game built on it starts on any phone that meets that profile.

The outer engine does not change: the render interface, the per-tick deltas, the GPU tables' layouts, the sim and every module above `render` stay as they are, and so does `renderer-vulkan` apart from code moving out of it into shared modules. The older spec is paid for in three places only: the new backend, one more define in the shared shaders, and limits a game on that backend must live within (16 textures, a smaller keyed sort). The result is allowed to be slower than the 1.3 backend where the baseline leaves no better way.

There is still no Android device, and no baseline-class GPU of any kind. Everything here is proved on this machine's Radeon made to answer as a baseline device by Khronos's profiles layer, with the validation layer checking every call against those reduced answers, on Linux and in the emulator. That proves the backend uses nothing the baseline lacks. It proves nothing about an old Mali or Adreno driver's bugs or speed, and the PRD says so where it matters.

It is done when:

- `renderer-vulkan11` builds for `linux`, `androidemu` and `android`, bound in the master file (`bind "render" { ... }`); `renderer-vulkan` remains every platform's default.
- With the profiles layer simulating `VP_ANDROID_baseline_2022` (API version, features, limits, extensions) and validation on, `testbed`, `stones`, `rocks`, `arcanum`, `arcfire` and `gallery` start, draw and exit on the new backend with no validation message, on Linux and in the emulator. The same run of `renderer-vulkan` is refused at start, by name, which shows the rig is really reducing what the device offers.
- `--check-sort` passes on the new backend; the gallery's screenshots are within the committed tolerance of the existing Vulkan references (scenes that exceed a baseline limit are listed and get references of their own or are skipped by name, not silently).
- Sim hashes on the new backend equal the recorded ones at 1 and 8 threads (the renderer has no say in them; this catches a build-flag mistake).
- `arcfire` and `arcanum` fit in 15 textures through atlases made by a tool in `tools/`, and run unchanged in behaviour on every other backend with those atlases.
- The slice 7 extraction is made: one `render_world` module holds the CPU half that Vulkan, Metal and D3D12 each carry a copy of today, and one `vulkan-common` module holds what the two Vulkan backends share. On `renderer-vulkan` the SPIR-V is byte-identical to before, and gallery references and hashes are as they were on Linux. Metal and D3D12 are changed on this machine blind and rebuilt on theirs before the slice closes, or keep their copies until they are (for review).
- An Android package built on the new backend declares Vulkan 1.1 and API 30 in its manifest and launches in the emulator from the launcher.
- `docs/design/renderer-vulkan11.md` says what differs from `renderer-vulkan` and why; `docs/design/renderer.md` gains the limits a game meets on each backend.

## What the profile guarantees and the renderer asks for

Read from `VP_ANDROID_baseline_2022.json` in KhronosGroup/Vulkan-Profiles (Vulkan 1.1.106) against the renderer as merged in slice 8.

| The renderer uses | The baseline gives | What follows |
| --- | --- | --- |
| Vulkan 1.3: dynamic rendering, `synchronization2` | 1.1; neither | Render passes and framebuffers, `vkCmdPipelineBarrier`, `vkQueueSubmit`. About thirty call sites. |
| A runtime array of 1,024 sampled images, non-uniform indexing, update-after-bind, partially bound, variable count | No descriptor indexing; 16 sampled images a stage, 48 a set; dynamic indexing of an image array only with a uniform index | A fixed set of 16 textures chosen in the fragment shader by a `switch` (R4) |
| `drawIndirectFirstInstance` | Not promised | The bucket's start travels in the push block (R6) |
| 7 or 8 storage buffers in the sort's compute stage, 4 in the vertex stage | 4 a stage, 24 a set | The sort's buffers become one, addressed by offset (R5) |
| 256 invocations a compute group | 128 | `kGroup` 128 |
| 13.3 KB of shared memory in the bucket sort, 16,384 B in the keyed sort | 16,384 B | The bucket sort fits. The keyed sort is at the limit exactly; halved to 1,024 rows for headroom (R5) |
| A 112-byte push block | 128 | Fits, with 16 bytes for R6 |
| `shaderDrawParameters`, compute, `fragmentStoresAndAtomics`, 32-bit indices | All promised | Nothing |
| `multiDrawIndirect`, timestamps, pipeline statistics | Not promised (`maxDrawIndirectCount` 1) | Already optional |
| Textures up to the device's size | 4,096 square | A load above it fails by name |
| Anisotropy | 1 | Not used |

Also promised and unused: ASTC and ETC2 (the engine ships PNG; compressed textures are a later slice's and would matter more on these phones than on any other), multiview, Y'CbCr conversion.

Precedents already in the tree:

- **A shader variant keyed by the bound backend.** D3D12's sprite shaders differ (`KV_TARGET_D3D12`: the row fetched at instance rate, registers pinned) and kormake compiles them under a rule variant keyed `["renderer-d3d12"]`. The baseline is a third such variant.
- **A fixed texture array.** Metal's shaders already declare `Texture2D textures[1024]` in a parameter block, not a runtime array.
- **One draw a bucket.** `renderer-vulkan` already draws bucket by bucket where `multiDrawIndirect` is missing.
- **Game shaders reach textures through `sprite_texel()`**, not `gTextures` directly: both that exist (`arcfire_dissolve`, `gallery_rings`) do, so the texture change stays inside the shared headers.
- **Three copies of the CPU half.** `docs/design/renderer-d3d12.md` names the third copy as the point to extract, and slice 7's R8 has waited on it.

## The shared halves

**`render_world` (R1).** Slice 7's R8, made now from three diffs where slice 7 had two: the mirror of the sprite pool, dirty bitmaps, per-bucket counts, material and texture records and their slot assignment, keyed-sort key computation, the garbage queue's shape, the upload ring's bookkeeping. A plain module the backends depend on; no interface, no virtual call, nothing in `renderer.h` changes. `kMaxTextures` stops being a constant of the shared code and becomes a number the backend gives it, which is the only thing the baseline needs from this module that the others do not.

**`vulkan-common` (R2).** What both Vulkan backends need unchanged: the loader and function table (`vk_funcs`), the platform seam (`vk_platform.h` and its SDL, Win32 and Android sources), the swapchain and its Android answers, the debug messenger and its filters, the GPU profile where it does not lean on 1.3. `renderer-vulkan` keeps its device creation, resources, passes and descriptors. The test of the cut is that `renderer-vulkan`'s object code and SPIR-V come out the same.

These two are worth doing whether or not the baseline backend follows, and are the slice's first two milestones for that reason.

## The baseline backend

`modules/renderer-vulkan11`, bound to `render` like the others. Its own `vk11_device.cc`, `vk11_resources.cc`, `vk11_sprite_pass.cc`, `vk11_post_pass.cc`; everything else from the two shared modules.

**The device (R3).** Instance and device at 1.1. The check asks for exactly the profile's features and the limits R4 to R6 lean on, and names what a device failed, as slice 8's does. `multiDrawIndirect` and timestamps are taken where offered. It runs on a 1.3 device too: a game built on it is one package for every phone.

**The API (R3).** One render pass for the scene target and one for the swapchain, each with its framebuffers remade with the swapchain; barriers and submit in their 1.0 forms; layouts tracked as today. No `VK_KHR_dynamic_rendering` or `VK_KHR_synchronization2` taken as extensions where a device has them: one path, the one every baseline device has.

**Textures (R4).** The set holds `Texture2D gTextures[16]`, slot 0 the default white, every slot always bound (an empty one to the default), rewritten between frames when a texture arrives or goes, since nothing may be updated after bind. Under `KV_TARGET_BASELINE`, `sprite_texel()` is a `switch` on the sprite's texture index with a `SampleLevel(..., 0)` in each arm: a constant index into the array, which every device allows, and exact because no texture has mips. Explicit level means no derivative leaves a branch undefined. `RenderCaps::max_textures` reports 16 and the seventeenth load fails as the 1,025th does today.

A font size is a texture. Arcfire holds 33 PNGs and arcanum 18 before fonts, so both need R8's atlases. Rocks (5), stones (2) and testbed (1) fit as they are.

Not done: atlasing inside the renderer. The samplers repeat and tiled backgrounds depend on it; a texture moved into an atlas behind the game's back would tile its neighbours. The game knows which of its textures tile and the renderer does not.

Held for evidence: sixteen textures a pipeline slot in place of sixteen in all (sprites in a layer are already grouped by slot before row order, so a slot could carry its own set). It widens what the sort's bucket means and is only worth it if 15 proves too few for a real game.

**The sort (R5).** `sprite_sort.slang` stays one source and one algorithm. Under the define:

- `gSorted`, `gBucketOf`, `gBlockHist`, `gSortMeta` and `gIndirect` are regions of one buffer, their offsets computed from the sprite capacity, `kBucketCount` and `kBlockRows`. Compute then binds `gSprites`, `gMaterials`, `gLayers` and that buffer: four. The vertex stage binds the same four. The buffer carries the indirect usage bit so the draw can read its region.
- `kGroup` is 128 and what is derived from it follows (the scan's threads each take 16 buckets where they took 8).
- `kKeySortMax` is 1,024, bringing the keyed sort's shared arrays to 8 KB. A bucket with more keyed rows sorts its first 1,024 and leaves the rest in row order, as it does past 2,048 today; the limits table says so.

The CPU side does not know any of this: the same push block, the same dispatch counts scaled by the group size, which the backend owns.

**The draws (R6).** The count pass writes each bucket's indirect command with `firstInstance` 0. The vertex shader, under the define, reads the bucket's start from the sort region by a bucket index in the push block (the block's spare bytes) where it reads `SV_StartInstanceLocation` today. One push and one `vkCmdDrawIndexedIndirect` a non-empty bucket: the loop the 1.3 backend already runs without multi-draw, plus a push. With 2,048 possible buckets and tens in use in every example, the cost is draw calls a tile-based GPU would rather not have and cannot be measured here.

**Shaders (R7).** A rule variant keyed `["renderer-vulkan11"]`: `-DKV_TARGET_BASELINE`, SPIR-V 1.3 (the newest a 1.1 device must take), no descriptor-indexing capability in the output (checked: `spirv-val` against a Vulkan 1.1 environment as a build step of that variant). Outputs are named apart (`sprite.vk11.spv`), so a tree may build both backends. A game's own shaders compile under the same variant with no change of theirs, provided they keep to `sprite_texel()`; one that indexes `gTextures` itself fails to compile there, which is the right way to find out.

The post pass samples one target and has no problem with any of this beyond R3's API.

## What a game does

**Atlases (R8).** `tools/atlas.lua` (kvscript, beside the other asset tools): a list of PNGs in, one PNG and a table of named rectangles out, a pixel of padding extruded round each so linear sampling does not bleed. The game names sprites by rectangle where it named them by file; the sprite's UV rect is already the game's to write (slice 6's sheets work this way). Textures that tile stay their own files and count against the 15.

Arcfire and arcanum are packed with it and use the atlases on every backend: one art pipeline, fewer textures everywhere, and the packing proved on platforms that can be looked at. No runtime choice between packed and loose art.

**Choosing the backend.** A build binds one renderer a platform, so a game's master file says `bind "render" { android = "renderer-vulkan11" }` and ships one package that runs on every Vulkan 1.1 phone, or keeps the default and ships to 1.3 phones only. Both in one package, chosen at start by what the device offers, needs a dispatch layer under `renderer.h` that nothing else in the engine has; out of scope until a game wants 1,024 textures on new phones and a fallback on old ones.

**The limits, in one place.** `docs/design/renderer.md` gains a table a game author reads once: textures, texture size, keyed rows a bucket, by backend.

## Android

**The floor (R9).** Phones of the baseline's age mostly run Android older than 13. The toolchain's triple goes from `android33` to `android30` (Android 11) for both backends: everything slice 8 calls exists there (AAudio from 26; `WindowInsetsController` and `WindowInsets.Type`, the newest calls in `host_window_android.cc`, from 30), so it costs no fallback code. Lower than 30 needs the old system-UI flags and inset reads written beside the new ones; P2, and only on a reason.

**The manifest (R9).** The `uses-feature` Vulkan level and version come from the bound renderer's `module.lua` (1.3 today, 1.1 for the new one), not from the packager.

**The validation layer** is packaged in debug as in slice 8. The profiles layer is a host tool only: in the emulator the simulation happens on the host's side of the passthrough or not at all (M1 finds out which; if not, the emulator run proves the Android surface path on the new backend and Linux carries the limits proof).

## Testing without the hardware

**The rig (R10).** `VK_LAYER_KHRONOS_profiles` with the profile's JSON, simulating API version, features, properties and extensions, under the validation layer. Validation checks each call against what the device now claims, so a seventeenth sampled image, a fifth storage buffer, a 256-wide group or a 1.3 entry point is an error and fails the run. `tools/vulkan/baseline_run.sh` wraps a command in that environment; the layer and the JSON come from the Vulkan SDK or the distribution, found by the script, which says what is missing.

What the rig cannot show, carried to the day there is a phone: driver bugs in old compute implementations (the sort is the engine's most demanding shader and old mobile compilers are where such shaders break); the speed of the sort, the `switch` and the per-bucket draws on a tile-based GPU; memory on a 2 to 4 GB phone; whether real 2020 to 2022 devices in number lack descriptor indexing at all. That last one decides how much this slice is worth and is answerable from gpuinfo.org's reports (for review 1).

**Read on Nov 13: worth building.** Two sources, neither a census of this engine's players.

- Android's distribution dashboard (active devices, 28 days to Nov 24, 2025; a year old): of handhelds, 26.7% report Vulkan 1.3 or 1.4, 62.1% report 1.1, 3.9% report 1.0 and 7.4% none. So `renderer-vulkan` as it stands starts on about a quarter of phones and a 1.1 backend on about nine in ten. The API version is the larger gate, not descriptor indexing.
- gpuinfo.org's Android reports (submitted by people who run a GPU tool, so newer and better phones than the average): `VK_EXT_descriptor_indexing` with all five features the renderer uses on 78% of reports, `VK_KHR_dynamic_rendering` on 59%, `VK_KHR_synchronization2` on 70%. By distinct device, 1,963 have descriptor indexing and 1,173 do not; of devices whose report is from 2024 or later, 1,443 have it and 372 (a fifth) do not, and those are mostly Mali (199) and PowerVR (129): cheap phones and tablets still being sold. Where the extension exists it is generous (all but 18 devices offer 500,000 update-after-bind images or more). `drawIndirectFirstInstance` is on 98%; five reports have the 128-invocation compute limit and three the 4-storage-buffer one, so R5 and R6 answer the profile's letter more than its devices, and are cheap.

What this says of the middle tier (1.1 calls with bindless kept): it would reach some large part of the 1.1 phones, and no figure here says how large. The baseline backend reaches all of them, so the middle tier stays out of scope unless 15 textures proves the limit games hit.

## Out of scope

A device and everything only it settles; choosing a renderer at run time; a middle tier (Vulkan 1.1 with `VK_EXT_descriptor_indexing` where offered); OpenGL ES; compressed textures and mips; more than 16 textures on the baseline by any scheme; renderer-side atlasing; pre-rotation (still slice 8's deferred P1, and more worth having on these phones); Android below API 30; changes to `renderer.h`, the delta stream or the sim.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `render_world`: the CPU half shared by every backend; `renderer-vulkan` unchanged in output | P0 |
| R2 | `vulkan-common`: loader, platform seam, swapchain, messenger shared by both Vulkan backends | P0 |
| R3 | `renderer-vulkan11`: a 1.1 device check that names failures; render passes, 1.0 barriers and submit | P0 |
| R4 | Sixteen fixed textures, `sprite_texel()` by `switch`, the cap reported and enforced | P0 |
| R5 | The sort within four storage buffers, 128-wide groups and 16 KB shared memory | P0 |
| R6 | Draws without `firstInstance`: the bucket's start through the push block | P0 |
| R7 | The `KV_TARGET_BASELINE` shader variant, SPIR-V 1.3, validated for a 1.1 environment | P0 |
| R8 | `tools/atlas.lua`; arcfire and arcanum within 15 textures on every backend | P0 |
| R9 | Android floor at API 30; the manifest's Vulkan level from the renderer | P0 |
| R10 | The profiles-layer rig and its script; every game clean under it | P0 |
| R11 | Metal and D3D12 moved onto `render_world` and rebuilt on their machines | P1 |
| R12 | `docs/design/renderer-vulkan11.md`, the limits table, module docs | P0 |

## Milestones

Each leaves every platform building and `renderer-vulkan` drawing as before.

1. **The rig** (R10): the profiles layer found and run; `renderer-vulkan` refused under it by name; whether the emulator can be put under it settled. Ten minutes to a day, and it is the go or no-go for proving anything here.
2. **One CPU side** (R1, R2, R11 as far as this machine goes): the two extractions. Vulkan's SPIR-V, references and hashes unchanged. Stands alone if the slice stops here.
3. **A triangle's worth** (R3, R7, R4): the new module, its device, passes and the texture set. `testbed` draws under the rig with the sort still the 1.3 backend's shape, which the rig will reject: the errors it gives are M4's list.
4. **Within the limits** (R5, R6): the merged sort buffer, the group size, the push-block start. `testbed --check-sort`, `stones` and `rocks` clean under the rig; the gallery compares.
5. **The games** (R8): the atlas tool; arcfire and arcanum packed and clean under the rig, and unchanged on `renderer-vulkan`.
6. **Android** (R9, R12): the floor, the manifest, a package on the new backend launched in the emulator; the design note, the limits table, and the list for the first old phone.

## For review

In the order a wrong answer costs most:

1. **Whether it is worth building.** Read on Nov 13 ("Testing without the hardware"): about a quarter of active phones report Vulkan 1.3 and about nine in ten report 1.1 or better, so the answer looks like yes. The figures are a year old and not this engine's audience; a game's own Play Console numbers would be the real ones.
2. **A separate backend, not flags in `renderer-vulkan`.** Flags would put about thirty conditionals in the 1.3 backend's passes, barriers, descriptors and device check. The price of the separate one is M2.
3. **Sixteen textures and atlases as the game's job.** The alternative that keeps more textures (a set a pipeline slot) is held until a game needs it.
4. **Arcfire and arcanum packed for every platform**, not only where they must be.
5. **`renderer-vulkan11` runs on new phones too**, and a game picks one backend a package. No run-time choice.
6. **API 30 for all of Android**, lowering slice 8's floor of 33 for the 1.3 backend as well (it refuses by driver, not by OS version).
7. **Metal and D3D12 edited here without a build** in M2, or left on their copies until someone is at those machines.
8. **The name.** `renderer-vulkan11` says the API; `renderer-vulkan-baseline` would say the profile. And this is called slice 9, which slice 8's PRD had promised to the first device run: that moves to whichever slice has a phone.
