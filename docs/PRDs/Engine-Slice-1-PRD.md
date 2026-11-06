# Engine Slice 1 — Dev PRD

Nov 4, 2026 · @starekrow

## Goal and exit criteria

Slice 1 proves the whole loop end to end, in the shape the engine will keep:

1. Input event
2. Phase systems
3. Component change
4. Extract
5. Bindless batched draw

It is done when:

- The example game opens a window, shows textured sprites, and moves at least one of them from the keyboard.
- No outer-engine code calls Vulkan. Everything the renderer sees arrives through the extract.
- The sim runs on a fixed tick, and rendering interpolates between ticks.
- Validation and sync validation report zero errors over a 60-second run.
- Closing the window shuts down cleanly, with no leaked Vulkan objects reported by validation.

## Core

- **Entity pools + generational handles.** Create, destroy, validate. A stale handle returns an error, never another entity's data.
- **Component storage:** SoA pools per component type. Lookup by handle.
- **Tick loop:** fixed tick (default 60 Hz) driven by an accumulator. Tick count per frame is capped, and the game slows when the cap is hit. The loop exposes `alpha` for interpolation.
- **Phase list:** a hard-coded, ordered list of systems. Each system declares its read/write sets in its registration. Enforcement is stubbed for now.
- **`EventBus` and `event`:**
  - typed per-tick buffers; emitting is an append;
  - delivery between phases, consumed in batches;
  - single-threaded in slice 1, but the API already uses per-emitter buffers and a deterministic merge;
  - no cascades or scheduled events yet.
- **Logging:** a leveled log to stdout. No CLI yet.

## Platform and input

- **Window service:** a platform service over SDL3, owned below the outer engine. It is not an entity.
  - API: open with title, size and vsync preference; query its current size.
  - Platform changes arrive as events on the bus: `WindowResized`, `FocusChanged`, `CloseRequested`.
  - These events count as input and are recorded with it.
- **Input layer**, with the keyboard as its first device:
  - key edges (`KeyDown`, `KeyUp`) are events;
  - the full key state is sampled once per tick into an input-state resource;
  - raw latest state stays available to render-rate code;
  - key codes are engine-defined and mapped from SDL scancodes.
- **Gamepad is out of scope**, but the input layer API must accept a second device type without changes.

## Math library

- **Types:** `vec2`, `point2` (affine: point − point = vec, point + vec = point, no point + point), `affine2` (2×3: rotation, scale, translation), `mat3`, `vec4`, `color` (linear RGBA float), `rect2`.
- **Ops:**
  - `affine2` × point and × vec (vec ignores translation);
  - compose and inverse;
  - lerp for interpolation;
  - ortho projection to clip space for the GPU upload.
- **Style:** plain structs, free functions and operators. Header-only, no SIMD requirement in slice 1.
- **Tests:** affine identities, inverse round-trip, and that point/vec misuse fails to compile.

## Assets and components

| Item | Kind | Contents |
| --- | --- | --- |
| Texture | Asset (handle) | Dimensions, format, GPU image + bindless slot (inner engine) |
| Material | Asset (handle) | Texture handle, blend mode (opaque, alpha, additive), tint, small parameter block |
| Sprite | Component | Material handle, UV rect, layer, tint |
| Transform2 | Component | Position, rotation, scale, z/layer; previous-tick copy kept for interpolation |

- **Loader:** `load_texture(path)` returns a handle. Slice 1 reads the file with fread and decodes the PNG. The API is async from day one, even though slice 1 loads synchronously. The handle starts out pending, and completion arrives as an AssetReady or AssetFailed event on the system bus at the next phase boundary, never inline in the call. A pending texture renders as a placeholder through its bindless slot. The I/O behind the handle can then be replaced later without changing any caller.
- **Hand-built materials:** materials are constructed in code from texture handles. There is no material file format yet.
- **Uploads** go through a staging buffer. Textures become resident before the first frame that references them.
- **Transforms are flat:** no hierarchy in slice 1.

## Extract and Vulkan inner engine

- **Extract:** after the last sim tick of a frame, walk Sprite + Transform2 and write one render packet per sprite:
  - entity handle;
  - previous and current transform;
  - material handle;
  - UV rect, layer, tint.

  A full copy per frame is acceptable in slice 1. Deltas come later.
- **Render side:** packets interpolated by `alpha`, sorted by layer, then batched by blend mode.
- **Vulkan 1.3 baseline:** dynamic rendering, sync2, descriptor indexing. No render passes or framebuffer objects.
- **Bindless:** one texture array descriptor plus samplers. Materials carry their texture's slot index.
- **Instance buffer:** one per frame in flight, holding transform, UV rect, tint and texture index.
- **Draws:** one instanced draw per blend-mode batch.
- **Frames in flight:** 2, with fences. Resources are destroyed only after the frame fence that last used them.
- **Swapchain:** recreated on resize, driven by the `WindowResized` event.
- **No render graph yet:** a single pass to the swapchain image, with explicit barriers written by hand.
- **No outer-engine includes in the inner engine,** and the reverse. The packet struct is the only shared type.

## Build, shaders, dependencies

- **Language:** orthodox C++ — no exceptions, no RTTI, no STL in the runtime.
- **Containers:** our own minimal set (array, pool, ring buffer).
- **Shaders:** Slang compiled to SPIR-V as a build step. The runtime loads only precompiled SPIR-V.
- **Runtime dependencies:** SDL3 (video and events only), the Vulkan loader, and one image decoder (libpng, plus zlib as its dependency).
- **Build-time tools:** the Slang compiler and the Vulkan SDK.
- **Validation:** debug builds enable the validation and sync-validation layers by default.
- **Modules:** code is laid out as core, platform, math, renderer-vulkan and the example game. Registration follows the build-generated module list, even with one module per layer.
- **First platform:** one desktop platform for slice 1 (Linux; Windows follows). macOS through KosmicKrisp is a stretch goal.

## Example game and acceptance tests

**The example game:**

- opens a window;
- loads 2–3 PNG textures from disk;
- builds materials by hand: at least one opaque, one alpha and one additive;
- spawns a background sprite plus a few layered sprites;
- moves a player sprite from the arrow keys at a fixed speed per tick;
- quits on Escape or when the window closes.

**Acceptance tests:**

- [ ] Movement looks smooth at refresh rates above the tick rate, because rendering interpolates.
- [ ] Holding a key moves the sprite the same distance per second at 30, 60 and 144 fps.
- [ ] Destroying an entity, then using its old handle, returns a stale-handle error.
- [ ] Resizing the window recreates the swapchain without errors or a stretched image.
- [ ] Validation and sync validation are clean for 60 seconds, including at least 3 resizes.
- [ ] Grep finds no Vulkan header included outside renderer-vulkan.
- [ ] Clean shutdown reports no leaked objects.

## Out of scope for slice 1

- Transform hierarchy, multithreaded phases, and scheduler enforcement of read/write sets
- Event cascades, scheduled events, triggers, handler tables
- Render graph, delta extract, indirect draws, compute culling
- Real async I/O, the cook pipeline, pack files, live import
- CLI, snapshots, replay, state hashing
- Audio, gamepad, physics
- Material file format, shader hot reload
- D3D12 and Metal backends, consoles
