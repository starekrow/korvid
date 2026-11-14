# Engine Slice 9 — One render core

Nov 13, 2026 · @starekrow

## Goal and exit criteria

Three backends draw the engine: `renderer-vulkan` (Linux, Windows, Android), `renderer-metal` (macOS, iOS) and `renderer-d3d12` (Windows). Each was made by copying the one before it and changing the calls, which was the right order (slice 7's R8: extract from what the diffs show, not from a guess), and the diffs now show it. Slice 9 makes the extraction: one render core that holds what the three share, and three backends that hold only what differs.

The point of the core is to **share structural concepts over different implementations**: one `Renderer`, one render world, one frame, one set of names, with each backend supplying the types and the steps. It is not a hardware abstraction, and it is not one architecture applied everywhere. Each shared piece takes whichever form is clearest for it: what needs no GPU type is ordinary code in `render`, compiled once and linked with any backend; what is clearer seeing the backend's concrete types is compiled into the backend and calls its functions directly. Nothing is hidden behind a table of function pointers or a template parameter, and nothing costs more at run time than it does today. Where the clearest way to share something is for the backend to hand the core a `#define`, that is the way.

The spine does not change. `korvid/render/renderer.h`, the delta stream, the GPU layouts, the shaders and every module above `render` stay as they are. No pixel moves.

It is done when:

- `render` holds the core: a linked half that knows no backend and a compiled-in half that each backend builds against its own types. No backend defines a function of `renderer.h`: the core defines all of them.
- The linked half has tests that run with no GPU: a delta stream in, the rows, dirty bits and bucket counts out.
- There is one definition each of the constants, `GpuSprite`, `GpuMaterial`, `PushData`, `Renderer`, the render world and its deltas, the texture and material API, the frame ring, the garbage queue, the dirty-row and draw-run walks, the push block, the sort check and the frame's order of work.
- The count this slice starts from (686 distinct code lines textually identical in all three backends once the `rv_`/`rd_`/`mt_` prefixes are normalised, by `tools/render-overlap.sh`; see "What the code has") falls under 60, and the backends' total (about 9,700 lines) falls by at least 2,000. Both are measured the same way before and after and written in the notes.
- Every compiled shader (SPIR-V, Metal library, DXIL) is byte-identical to before the slice: no shader source changes.
- On each backend: the gallery's nine scenes equal that backend's committed references exactly (same GPU, same driver, so the tolerance is not needed and not used); `--check-sort` passes; arcfire's demo, `rocks` and `arcanum` run under validation with no message; resize, minimise, vsync on and off, `--screenshot` and the post pass behave as before.
- On Android in the emulator: the gallery within tolerance as slice 8 left it, and the fifty-round lifecycle loop clean (the surface lost and regained is the one path only Vulkan has).
- It is no slower. arcfire's `--bench ring --dial rank=56`: the CPU's `record` zone and the GPU's frame, sort and draw times are within run-to-run noise of the numbers taken before the slice on the same machine, per backend.
- `docs/design/renderer.md` describes the core and the contract and is right about the layouts; the Metal and D3D12 notes stop saying "copied from Vulkan".

## What the code has

Measured Nov 13 on `main`, comments and lone braces left out, prefixes normalised, file against its counterparts (the table's last column sums to 717; over each backend as a whole, which is what `tools/render-overlap.sh` counts and the exit criteria use, it is 686):

| File | Vulkan | Metal | D3D12 | Identical in all three | What the shared part is |
| --- | --- | --- | --- | --- | --- |
| `renderer.cc` / `.mm` | 380 | 401 | 336 | 249 | `renderer_apply`, the texture and material API, names, post parameters, caps |
| `*_resources` | 457 | 288 | 410 | 168 | the frame ring, material rows, rebinding, pools, the handle kinds' describe and find |
| `*_sprite_pass` | 531 | 365 | 429 | 143 | dirty runs, row staging, draw runs, the push block, key snapshots, the sort check, game slots |
| `renderer_internal.h` | 334 | 259 | 340 | 109 | constants, the GPU layouts, the CPU records |
| `*_post_pass` | 285 | 111 | 148 | 31 | the shader's name, layer, failure and parameters |
| `*_gpu_profile` | 120 | 91 | 104 | 17 | the marks and the stats record |

The last two rows are the proof of where to stop: the post pass's objects and the profiler are each backend's own, and only their settings are shared.

The copies have begun to drift, which is what the slice is for:

- The Metal copy dropped most of the comments ("as renderer-vulkan's"), so the reasons live in the Vulkan files only.
- Metal keeps the screenshot request in a file global (`g_shot`); the other two keep it in `Renderer`.
- `metal_clip` and `d3d_clip` are one function with two names. `blend_mode_name` and `blend_mode_parse`, which are `render`'s public API, are defined once per backend.
- The `RendererSurface` to `WindowSurface` switch is written twice (`frameloop.cc`, the gallery).
- `docs/design/renderer.md` says a sprite row is 96 bytes; the code asserts 112.
- The gallery has committed references for Vulkan and Metal and none for D3D12.

Slice 7's milestone 5 ("One CPU side") and the later Android 2022 baseline PRD's `render_world` both planned a module of CPU code with no GPU types in it. This slice replaces that plan: see "For review", 1.

## The shape

**Where the core lives (R1).** In `render`, in two halves. Which half a piece goes in is decided piece by piece, by which reads better, and a piece may move from one to the other during the slice without that being a change of plan.

*The linked half* is `render`'s own `src/`: `render` becomes a library like any other, with tests. It holds what needs no GPU type, as plain structs and functions over plain data: the render world (`RenderWorld`: the sprite pool and entity map, the dirty bitmaps, the bucket and keyed counts, the layers, the tick, and `rw_apply` over a delta stream), the material records and their rows, names, the ring's arithmetic, the dirty-run and draw-run walks, the push block, the key snapshot and the sort verifier, the screenshot's file writer, `blend_mode_name`. It calls nothing in a backend, so the dependency runs one way (backend on `render`, as now) and there is no link cycle.

*The compiled-in half* is `modules/render/include/korvid/render/core/`, sources named `.inl` so that no build rule picks them up on their own. Each backend gains one source, `src/render_core.cc` (`.mm` for Metal), which includes the backend's `renderer_internal.h` and then these. It holds what is clearer written against the backend's real types: `struct Renderer` (which contains the `RenderWorld`), the public API's entry points, the resources that own GPU objects, and the frame. kormake does not change (vendored C is already built this way: a `.cc` that includes it).

- `layout.h`: the constants, `GpuSprite`, `GpuMaterial`, `PushData`, the flag bits, the marks. Mirrors `sprite_common.slangh`, in one place. Both halves include it.
- `world.h` and `src/`: the linked half.
- `core.h`: the contract (below), the shared records, `struct Renderer`, and the declarations of the compiled-in half.
- `resources.inl` (pools, handle kinds, textures, the ring's buffer, garbage), `frame.inl` (the frame, the uploads, the draws), `api.inl` (the functions of `renderer.h`, most of them a line that calls the linked half and marks something for the backend).

**The contract (R2).** Before it includes `core.h`, a backend's `renderer_internal.h` provides:

- **Defines.** `KV_RB_NAME` (`"vulkan"`: the log channel and `RenderCaps::backend`), `KV_RB_CLIP_Y_UP` (0 or 1: the core flips the projections it pushes), `KV_RB_SHADER_EXT` (`".spv"`, `".metallib"`, `".dxil.zip"`), `KV_RB_SURFACE`. The core uses their values and never asks which backend it is: there is no `#if VULKAN` in the core.
- **Types**, each a plain struct the backend defines however it likes: `RbBuffer` (a GPU buffer and, where it has one, its mapping), `RbTexture` (a slot's image), `RbPipeline` (a game shader's module and pipeline), `RbGarbage` (one thing to destroy later), `RbFrame` (a frame in flight's command buffer, fence, sets), `RbDevice` (everything else the backend owns: device, queue, swapchain, layouts, pipelines, the post pass's objects, the profiler).
- **Functions**, `rb_*`, the same names and signatures in all three, about thirty-five of them, in three groups. *The device and its frames:* create and destroy, caps, resize, set surface, wait for a slot's last frame, acquire, begin, submit and present. *Resources:* make and retire a buffer, make a texture's image and retire it, write a slot into a frame's set, destroy one piece of garbage, make a game pipeline, load a shader, set and prepare the post pass. *Recording:* copy a staged texture, copy a run of rows from a ring span into a table, run the sort, begin and end a pass into the world's target or the window, bind the sprites, draw a run of buckets, draw the post triangle, read the sort or the window back, labels and marks.

The list is settled in milestone 1 from the code, not from this paragraph.

**One `Renderer` (R3).** `core.h` defines it: a `RenderWorld world` (the linked half's state: `sprites`, `sprite_of_entity`, the dirty bitmaps, the bucket and keyed counts, the layers, `tick`), the shared fields that sit beside GPU objects (`materials`, `textures`, the ring, the post pass's settings, the requests and counters), the tables as `RbBuffer`s, and one member `RbDevice gpu`. Shared records carry a backend tail the same way: `Texture { ...; RbTexture gpu; }`, `GamePipe { ...; RbPipeline gpu; }`, `FrameInFlight { garbage, dirty slots, ring marks, timing flags; RbFrame gpu; }`. In a backend, `r->device` becomes `r->gpu.device` and `r->sprites` becomes `r->world.sprites`.

**One set of names (R4).** `rw_*` is the linked half's, `rc_*` the compiled-in half's, `rb_*` the contract's, and a backend's file-local helpers stay unprefixed in its anonymous namespace as now. `rv_`, `rd_` and `mt_` go. The three backends' files then read side by side: the same function names in the same order, differing in their bodies.

**The rules of the seam (R5).** These are what keep it from becoming the thing it replaces.

1. No templates over the backend, no virtual functions, no tables of function pointers. Plain structs and plain functions: a name has one definition in a build, and "go to definition" lands on it.
2. A function is the core's when its three bodies are the same once the names are normalised. If they are only alike, it stays in the backends. Nothing is reshaped to look shareable (the post pass's objects, the profiler, the swapchain, the binding model, barriers and resource states are the backends').
3. The core does not reach into `r->gpu`. The backends read and write the core's fields freely: no accessors, no "context" structs passed about to hide them. That one-way rule is the whole of the purity asked for.
4. The contract is at the height of a pass, not of an API call: "copy these rows", "run the sort", "draw this run of buckets", never "create a descriptor" or "transition a resource".
5. Neither half is the correct one. A piece goes in the linked half when it falls out as plain data in, plain data out, and gains a test for it; it goes in the compiled-in half when making it backend-blind would take a handle, a callback or a copy it does not need today. If a piece has to be argued into either, it is in the wrong one.
6. The compiled-in half and a backend are one unity build. Small `rb_*` functions on the frame's path are defined in the backend's header so they inline; nothing on that path may grow a call it does not make today.

**The world and the resources in the core (R6).** Moved as they are, from the Vulkan copy, which has the comments: `renderer_apply`, `rc_bucket_of`, the material rows and their rebinding, game slots (`rc_material_slot`, which asks the backend for the pipeline), the texture and material API, derived names, the pools and handle kinds, the frame ring (the accounting is the core's; the buffer is an `RbBuffer` the backend makes; a span carries its `RbBuffer` as it carries a `VkBuffer` today), the garbage queue (the ownership rule and the two-stage count are the core's; an entry is an `RbGarbage` and the backend destroys it), `renderer_caps`, the post pass's layer, shader name, failure flag and parameters, the screenshot and sort-check requests. Metal's `g_shot` becomes the `Renderer`'s fields like the others'.

**The frame in the core (R7).** `renderer_render` and the record function are written once, as the order of work the three already agree on:

1. Wait for this slot's last frame; collect its GPU timings; release its ring span; run its garbage.
2. Grow the tables if the world outgrew them; write the layers' offsets; write the dirty texture slots into this frame's set.
3. Acquire (the backend says whether there is an image and how big).
4. Record: the uploads (staged textures, then `for_dirty_runs` over sprite and material rows, the core staging each run into the ring and the backend copying it); the push block; the sort, when there is something to draw, and the sort check's readback; the world's pass up to the post layer when the post pass is on; the window's pass with the post triangle and the remaining runs; the screenshot's readback.
5. Submit and present; close the frame's ring span and garbage count.
6. A requested screenshot or sort check waits for the frame and is written or verified by the core.

The draw-run walk (which buckets, which slot, the label's text) is the core's; issuing the run is `rb_draw_run`, which is where Vulkan's multi-draw, Metal's loop and D3D12's `ExecuteIndirect` stay. The sort's order (count, scan, bucket scan, scatter, key sort per keyed bucket, each with its group count and label) is one table in the core that each backend's `rb_sort` walks with its own binds and its own barrier between steps. Metal's encoders against Vulkan's single command buffer is handled by `RbFrame` holding whatever is open: the core says "begin the window's pass", not how.

**Cleanup the move makes possible (R8).** One home for the surface-kind mapping that both the frame loop and the gallery can reach. `docs/design/renderer.md` gains "The core and the backends" (the contract, the rules above, what a fourth backend must write) and its layout sizes are corrected; `renderer-metal.md` and `renderer-d3d12.md` keep what each port found and lose the description of copying. Each backend's README lists its files against the contract.

## Proving nothing moved

**Before anything changes (R9).** On each machine: the gallery's references for the backend it runs (D3D12's are taken and committed for the first time, at the parent commit), the bench numbers of the exit criteria, and the hashes of every compiled shader. The line counts are taken once, here.

**After each milestone (R10).** The same gate, a script of a few commands per machine: build debug, dev and ship; the render-facing suites; the gallery compared exactly against that backend's references; `testbed --check-sort`; arcfire's demo for 700 frames under validation; a screenshot; the shader hashes. On Linux it also runs `androidemu` (gallery, the lifecycle loop). The script is written in milestone 1 and is the same on all three machines but for the target's name.

**Who can run what.** This machine proves Vulkan on Linux and in the Android emulator. Metal (macOS, the iOS Simulator) and D3D12 and Vulkan on Windows are proved on their own machines. See "For review", 2.

## Out of scope

Any change to a shader, a GPU layout, `renderer.h` or what a frame costs. One base-instance path for all three (D3D12's instance-rate fetch would work everywhere; it changes the Vulkan and Metal draws, so it is its own piece of work). A header shared between Slang and C++ for the layouts. A suballocator for D3D12, SM 6.6 heaps, Metal's read-only buffer warning: each backend's open items stay open. `renderer-vulkan11` and `vulkan-common` (the Android 2022 baseline PRD): that slice follows this one and writes its backend against the contract, which is the first test of whether the contract is right. An Android device.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | The core in `render`: a linked half in `src/` with GPU-free tests, a compiled-in half in `include/korvid/render/core/` built through each backend's `src/render_core.cc` / `.mm`; no kormake change | P0 |
| R2 | The contract: the defines, the six types, the `rb_*` functions, declared once in `core.h` | P0 |
| R3 | One `Renderer` and one of each shared record, with a backend tail | P0 |
| R4 | `rc_` and `rb_` names; `rv_`, `rd_`, `mt_` gone | P0 |
| R5 | The rules of the seam, written into `core.h`'s header comment and `renderer.md` | P0 |
| R6 | The world, the resources, the ring, the garbage queue and the public API in the core | P0 |
| R7 | The frame and its recording in the core over pass-level steps; the sort's order as one table | P0 |
| R8 | The surface mapping's one home; the design docs and READMEs | P1 |
| R9 | Baselines per backend: references (D3D12's first), bench numbers, shader hashes, line counts | P0 |
| R10 | The gate script, run on every backend's machine at every milestone | P0 |

## Milestones

Each leaves all three backends building and drawing exactly what they drew. A milestone is closed when its gate has passed on all three machines.

1. **Ground** (R1, R2 in outline, R4, R9, R10). The baselines and the gate script. `layout.h`, and the three internal headers include it. `core.h` with the contract's defines and type names (each backend aliases what it has: its `Buffer` is `RbBuffer`); `render_core.cc` in each backend, compiling an empty core. The contract's functions renamed to `rb_*` where they already exist with one shape. No code moves yet; the diff is names.
2. **The linked half** (R1, R6). `render` becomes a library. First what takes its inputs as arguments: `for_dirty_runs`, the draw-run walk, the push block (a clip flag in), `rw_bucket_of`, `blend_mode_name` and `_parse`, derived material names, the key snapshot, the sort verifier (three pointers in), the screenshot's file writer. Then `RenderWorld` and `rw_apply`, each backend's `Renderer` holding one where its loose fields were. Its tests arrive with it. Each piece is deleted from three files as it lands in one.
3. **One Renderer** (R3, R6). The struct and the shared records move to `core.h` with their `gpu` tails; the backends' fields move under `r->gpu`. Then, on that struct: the pools and handle kinds, the entry points of `renderer.h`, the texture and material API, material rows and rebinding, game slots, the ring, the garbage queue, caps, the post pass's settings, the requests. Metal's `g_shot` goes. The largest milestone, and mechanical: its first commit is the rename alone, so the move that follows shows as moves.
4. **One frame** (R7). `renderer_render` and the record function in `frame.inl`; the pass-level `rb_*` steps in each backend; the sort's table. The backends' `renderer.cc` is gone or holds only `rb_create` and `rb_destroy`. The bench numbers are taken again here, since this is the milestone that touches the frame's path.
5. **Close** (R5, R8, the exit criteria). The docs, the surface mapping, the READMEs. The counts and the bench numbers against milestone 1's, in `docs/design/renderer.md`. The full gate once more on every machine, the emulator's lifecycle loop and the iOS Simulator's smoke included.

## For review

Decisions the milestones lean on, in the order a wrong answer costs most:

1. **Two halves, and no rule that everything be one or the other.** Slice 7 and the baseline PRD described a single `render_world` module with no GPU types; the first draft of this PRD compiled everything into the backends. Decided in conversation (Nov 13): the core exists to share structure, not to hide the backends from it, so a clean linkable piece is welcome where it comes naturally (the world: it also gets the renderer its first tests without a GPU) and is not forced where it would need opaque handles, callbacks or a `RenderCore<Backend>` template. Where the line falls is a guess until the code is moved: the split in "The shape" is the starting position and rule 5 is how it is revised. The cost of the compiled-in half: it is built three times across the platforms and can be broken for a backend this machine cannot build. The gate is the answer to that. The baseline PRD's `render_world` items and its R11 are superseded by this slice; its title still says "Slice 9" and wants renumbering.
2. **Metal and D3D12 are edited here without a compiler and proved on their machines at each milestone's gate.** The alternative is to convert Vulkan alone through milestone 4 and port the other two afterwards, which is safer per step and leaves the three furthest apart exactly while the core is being shaped. Preferred: all three together, with the next milestone's Vulkan work allowed to start while the other two gates are pending, and never more than one milestone ahead.
3. **Stopping the contract at passes** (rule 4). A thinner backend is possible (buffers, pipelines and passes as objects the core drives) and would share the uploads' and draws' bodies too. It is the general hardware abstraction this slice says it is not, and the three binding models are where it would hurt.
4. **Whether milestone 4 is worth it** is asked again when milestone 3 closes. Milestones 1 to 3 remove most of the duplication with little risk to the frame's path; milestone 4 shares the order of the frame, which is the most valuable thing to have written once and the only step that could cost a call or blur a backend's own flow. If the shared record function needs more than a few steps whose only reason is one backend, it stays three functions that call the shared pieces.
5. **Exact gallery comparison** as the test, per backend against its own references on the same machine. If a driver update lands mid-slice the references are retaken at the parent commit, not loosened.
6. **Prerequisites:** the Mac and the Windows machine reachable at each gate; D3D12's first gallery references committed before milestone 1's code.
