# Engine Slice 3 — Dev PRD

Nov 7, 2026 · @starekrow

## Goal and exit criteria

Slice 2 built the day-2 game programmer's engine and left the renderer alone on purpose. Slice 3 is the renderer and the platform: the "GPU-driven from day one" line of the requirements that slice 1 deferred, the platform work that an MVP cannot ship without (gamepads, real async I/O, sockets), and the first CLI so the engine can be inspected and poked from outside while it runs.

The outer engine does not change shape. The `SpriteDelta` stream stays the renderer's only input about sprites; the extract, the kinds and the schedule are untouched. What changes is everything below `render/renderer.h`, the platform module's reach, and a new `cli` module.

It is done when:

- The renderer keeps a persistent GPU sprite table. `renderer_apply` writes rows; a frame uploads only the rows the ticks changed. No per-frame rebuild, no CPU sort. A frame in which nothing moved uploads nothing.
- Sorting by layer and blend, culling against the view, and the draw count all happen on the GPU; the CPU records indirect draws and never looks at a sprite to draw a frame.
- Materials live in a GPU table addressed by row, with the parameter block the render header promised. A material edit at runtime reaches the next frame.
- Swapchain recreation never waits for the device to idle. The old swapchain and its semaphores retire through the same fenced queue as every other GPU resource.
- Per-frame GPU-visible data (dirty rows, material rows, texture uploads) comes from one ring allocator released by the frame fence. Texture uploads no longer block the main thread on their own fence.
- `renderer.cc` is split by concern; no renderer source is over about 600 lines; Vulkan headers still appear in `renderer-vulkan` only.
- Materials have a file format and load like textures, asynchronously, with the same handle-at-once, placeholder-until-ready contract.
- A gamepad steers `rocks`. The I/O queue is io_uring on Linux with the synchronous backend kept as a fallback. The platform offers non-blocking TCP sockets.
- A `cli` module exists: a line protocol over stdin and TCP, commands registered as module data, and `ls`, `dump` and `patch` over materials working from a terminal while the game runs.
- Validation stays clean with sync validation on; `rocks --demo --hash` still matches across runs and thread counts; the leak report at exit is empty.

## Renderer

**Sprite table (R1).** One device-local storage buffer of `GpuSprite` rows, slot-addressed by the render world's pool index, so the renderer's existing `sprite_of_entity` map is the only lookup and the GPU row is the CPU slot. A row holds the previous and current placement, the tick stamp of its last `Set`, material row, uv, size, tint, layer and a live flag. The `Tick` delta no longer copies current into previous for every sprite: a `Set` on a row stamped with an earlier tick first moves its current placement into previous, then writes; the vertex shader interpolates only rows stamped with the frame's tick and shows the current placement for the rest. A sprite that did not move costs nothing on either side. Rows written since the last frame are tracked in a dirty list; the frame copies them, coalesced into runs, from the frame ring into the table.

**Sort, cull, draw (R2).** A sprite's bucket is `(clamp(layer, -128, 127) + 128) * 3 + blend`: 768 buckets. The CPU keeps an exact count per bucket, maintained on `Set` and `Remove`, so it knows which buckets are non-empty without looking at sprites. The GPU does the rest, per frame, in compute:

1. *Count.* Per block of 1024 rows: cull each live row's bounding circle against the view rect, write the row's bucket (or "culled") to a side buffer, build a block histogram in shared memory and write it out.
2. *Scan.* Per bucket, a running sum over the block histograms gives each block's offset within the bucket; an exclusive scan over bucket totals gives each bucket's start and writes one indirect draw command per bucket (6 vertices, `instanceCount` = visible count, `firstInstance` = bucket start).
3. *Scatter.* Per block: local histogram, local exclusive scan, a bitonic sort of the block's `(bucket, local index)` keys, so rows of one bucket are contiguous and in slot order; each row's sorted index goes to `bucket start + block offset + rank`.

The CPU walks its non-empty buckets in order and records one `vkCmdDrawIndirect` per run of consecutive buckets with the same blend mode (`drawCount` = run length, one pipeline bind), so the draw list is exact, the order within a bucket is slot order (deterministic frame to frame), and the per-frame CPU cost is proportional to the distinct layers in use, not the sprites. Culled buckets draw zero instances. The vertex shader reads `sorted[base + instance]` with the base instance from `SV_StartInstanceLocation`, which Slang excludes from `SV_InstanceID`.

**Material table (R3).** `MaterialDesc` gains `params[8]` (two float4s) alongside texture, blend and tint. The GPU table holds, per row, the texture slot, tint and params; rows change through `renderer_material_set` and are uploaded like sprite rows. The sprite shader applies the material's tint and reads its texture slot through the row; the sprite shader uses `params.xy` as a UV offset so a patched parameter is visible. A destroyed texture's slot shows the default, as before, because the slot is resolved through the texture pool at the time the material row is written.

**Fenced swapchain (R4).** `recreate_swapchain` builds the new chain with `oldSwapchain` set, then queues the old handle, its views and its per-image semaphores on the current frame's garbage list; `vkDeviceWaitIdle` leaves the frame path (it stays in `renderer_destroy`). A format change retires the pipelines the same way. The design doc's status table loses its last gap.

**Frame ring (R5).** One host-visible, persistently mapped ring buffer. `ring_alloc(bytes, align)` returns an offset for the frame being recorded; the frame remembers its tail, and the wait on a frame slot's fence releases that frame's span. Dirty sprite rows, material rows and texture uploads stage through it; a texture larger than the ring falls back to a one-off staging buffer retired through the garbage queue. Uploads record into the frame's command buffer ahead of rendering, so the dedicated upload command buffer and its blocking fence go away. A frame that cannot present (zero-size window) still submits its uploads.

**The split (R6).** `renderer_internal.h` holds the `Renderer` struct and the private declarations; `vk_device.cc` (instance, device, debug messenger), `vk_resources.cc` (memory, buffers, images, textures, materials, ring, garbage, descriptors), `vk_swapchain.cc` (swapchain, frames in flight), `vk_sprite_pass.cc` (pipelines, compute passes, the frame's commands) and `renderer.cc` (the public API). Unity build turned back on.

## Materials as assets

A material file is JSON, next to textures under the module's `assets/`:

```json
{ "texture": "example_rocks/rock.png", "blend": "alpha", "tint": [1, 1, 1, 1], "params": [0, 0, 0, 0, 0, 0, 0, 0] }
```

`asset_load_material(e, path)` returns a handle at once; the renderer row exists from then on with the default texture and the file's defaults, the file read goes through the I/O queue, the parse happens in `asset.pump`, the texture it names is loaded through `asset_load_texture`, and `AssetReady` arrives once the texture is resident (or at once if it was already). `asset_material(e, h)` gives the renderer's material handle and state. The parser is `core/json.h`: a small tree in scratch, no allocation outside it, every error named with the byte offset. The examples load their looks from files.

## Platform

**Gamepad (R7).** `SDL_INIT_GAMEPAD`; pads open on `SDL_EVENT_GAMEPAD_ADDED`. `Input` gains `GamepadState pads[KV_MAX_GAMEPADS]` with a button bit set, axes in [-1, 1] with a dead zone, and `connected`; `sampled` copies them with the keys. Events `GamepadConnected`, `GamepadDisconnected`, `GamepadButtonDown`, `GamepadButtonUp`. `rocks` reads the left stick and the south and east buttons beside the keys.

**Async I/O (R8).** `IoQueue` keeps its API and grows a backend: `io_uring` on Linux (`src/linux/io_uring.cc`, raw syscalls over `<linux/io_uring.h>`; no liburing), the synchronous `fopen` path as the fallback and for platforms without one (`src/posix/io_sync.cc`). The uring backend opens and sizes the file at submit, queues one `IORING_OP_READ` with the request's handle as user data, enters the ring once per pump and reaps completions without waiting. Nothing completes inline; priorities order submission. Which backend is in use is logged at init and selectable (`IoBackend::Sync`) for tests that check ordering.

**Sockets (R9).** `core/net.h` is the shape; `platform` carries the POSIX implementation: `NetListener` (bind, non-blocking accept) and `NetSocket` (non-blocking read into a caller buffer, buffered write drained by the pump). Everything is polled from `platform_pump`; nothing blocks, nothing spawns a thread. This is the transport the CLI runs on; a game may use it too.

## CLI

**Commands are registration data (R10).** `ModuleDesc` gains a `commands` table of `CliCommandDesc { name, help, fn }`. `fn(CliContext&, Span<const char* const> args)` writes lines through the context. The engine collects the table at init like every other table.

**Transports (R11).** The `cli` module reads lines from stdin (when it is not a terminal, or with `--cli-stdin`) and from a TCP listener (`--cli-port N`). Each connection is a session with its own line buffer. A line is a command; `#id cmd` prefixes every response line of that request with `#id ` and ends it with `#id ok` or `#id err <message>`, so a tool can multiplex. Output is ASCII and line-oriented; a session that cannot drain gets a drop marker, never a stall.

**The commands (R12).** `help`, `ls materials`, `dump material <path>`, `patch material <path> <field> <values...>` where field is `texture`, `blend`, `tint` or `params`. `ls`, `dump` and `patch` live in the asset module (whoever reads a format owns the tool); `help` and `quit` live in `cli`. Reads run on the main thread in `Phase::Input` against the last tick; a patch reaches the renderer before the next frame.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Persistent GPU sprite table, tick-stamped rows, dirty-row uploads only | P0 |
| R2 | Compute count/scan/scatter by `(layer, blend)` bucket with view culling; indirect draws per non-empty bucket run; slot order within a bucket | P0 |
| R3 | GPU material table with `params[8]`; runtime edits reach the next frame | P0 |
| R4 | Swapchain recreation through the fenced garbage queue; no wait-idle in the frame path | P0 |
| R5 | Frame ring allocator for per-frame GPU-visible data; texture uploads through it, in the frame's command buffer | P0 |
| R6 | `renderer.cc` split into device, resources, swapchain, sprite pass, API | P0 |
| R7 | Gamepad input: state, events, sampled per tick; `rocks` steerable | P0 |
| R8 | io_uring backend for `IoQueue`; synchronous fallback kept | P0 |
| R9 | Non-blocking TCP listener and sockets, pumped per frame | P0 |
| R10 | Material JSON files loaded through the asset module like textures | P0 |
| R11 | `cli` module: stdin and TCP transports, request ids, commands as module data | P0 |
| R12 | `ls materials`, `dump material`, `patch material` | P0 |
| R13 | Materials for every example loaded from files | P1 |
| R14 | `--cli-port` reachable from a second process while `rocks` runs | P1 |
| R15 | Gamepad rumble, more than four pads | P2 |
| R16 | io_uring for sockets; registered buffers and fixed files | P2 |
| R17 | Render graph, motion vectors, render-rate entities | P2 |

## Out of scope for slice 3

- Any change to the extract or the delta stream; render-rate entities; the transform hierarchy
- Depth or anything beyond one color attachment; MSAA; multiple views or render targets
- A render graph. The sprite pass is one pass with fixed barriers; the graph arrives when there is a second pass
- Cooked textures (BC/ASTC), pack files, residency streaming
- A CLI client binary. `nc` and stdin are the clients; a machine-readable mode beyond request ids waits for a consumer
- Windows, macOS, audio, Vulkan on anything but Linux
- Rewind, replay, live import; the CLI's write path is direct for materials and nothing else

## Open questions, risks and milestones

**Open questions**

- Whether the layer range of 256 is enough for a real game or whether buckets should be allocated by the layers in use.
- Whether material files should hold more than the renderer's row (a name, a texture's sampling mode); the format is one object so fields can be added.
- Whether the CLI should take writes as command events through the input log, as the requirements say, before anything beyond materials becomes patchable.

**Risks**

- Nondeterministic order within a bucket would flicker where same-layer sprites overlap. The block-local bitonic sort is the answer; its correctness is checked against a CPU sort in a test on the shader's logic and by screenshots.
- io_uring may be unavailable (seccomp, `kernel.io_uring_disabled`). The backend choice is at init with the fallback logged, never a failure.
- Slang's `SV_InstanceID` excludes the base instance; the shader adds `SV_StartInstanceLocation`. Verified against the GLSL Slang emits.

**Milestones**

1. Renderer split, no behaviour change.
2. Frame ring, fenced swapchain, uploads through the ring.
3. GPU sprite table, material table, compute sort/cull, indirect draws.
4. Gamepad, io_uring, sockets.
5. JSON, material assets, the examples on files.
6. The `cli` module and the material commands.
