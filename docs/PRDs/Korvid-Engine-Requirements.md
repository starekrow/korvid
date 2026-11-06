# Game Engine — Requirements v0

Nov 4, 2026 · @starekrow

## Scope and targets

- v1: flexible 2.5D sprite + background engine.
- Long-term ceiling: games up to AA; renderer at roughly 25–50% of 2025's cutting edge.
- Platforms: Windows, Linux, Steam Deck, macOS, Android first; iOS, Xbox, PS5, Switch 2 later.
- All core abstractions are our own. No third-party engine or rendering framework (UE5 and The Forge rejected).

## Graphics stack

- **RHI:** a thin custom layer shaped like D3D12/Vulkan: bindless resources, explicit barriers, indirect draws.
- **Backend order:** Vulkan 1.3+ first (Windows, Linux, Steam Deck, Android; macOS via KosmicKrisp). Then D3D12 for Xbox, native Metal for iOS, and console backends under NDA.
- **Shaders:** Slang (HLSL via DXC as fallback). One source compiles to SPIR-V, DXIL and MSL.
- **Renderer is a separate layer above the RHI, and not thin.** It owns the render graph, culling, residency, materials, acceleration structures and upscaler integration.
- **Render graph:** passes declare reads and writes. The graph derives barriers and aliases transient memory.
- **v1 is GPU-driven from day one:** sprite instances in storage buffers, bindless textures, compute cull/sort, indirect draws.
- **Growth path:** motion vectors, plus per-instance and per-material data addressable by ID. RT is added later as passes using inline ray queries.
- **SDL3** for windowing, input and the audio device only. Not used for graphics.

## Core architecture

- **Outer engine** owns the world as entities and components. Entity pools own their nodes.
- **Three separate graphs, never merged:**
  - Transform hierarchy: a forest, stored as flat depth-sorted arrays with SoA hot fields.
  - Asset sharing: a DAG of handles.
  - Update scheduling: a DAG of phases.
- **Extract phase:** each tick produces a delta stream (state + events). It feeds persistent inner-engine worlds (render, audio), not a copy of the graph.
- **Handles:** generational handles across every boundary. GPU-side indices are allocated by the inner engine and mapped from entity handles.
- **Phases** read previous state and write next state.
- **Concurrency is checked by the scheduler:** systems declare read/write sets, and conflicts are rejected at registration. Debug builds check spans handed across the ABI.

## Time model and determinism

- **Fixed tick** for gameplay and physics, driven by an accumulator. Ticks per frame are capped; past the cap the game slows down.
- **Rendering** interpolates between previous and current sim state, reusing the motion-vector data.
- **Render-rate entities:** an entity class may opt into a per-render update on real dt. It is presentation only, and data flows from sim to it, never back.
- **Animation:** state advances on the tick; the pose is sampled at the render interpolation point.
- **Camera and aim** are applied at render rate, from input sampled as late as possible.
- **Simulation LOD:** tiers (for example 120/60/30/15 Hz) staggered by ID. Relevance comes from sim state only, never from the camera or visibility.
- **Determinism within one build and platform is required:**
  - parallel reductions merge in a fixed order;
  - gameplay containers iterate in a fixed order;
  - RNG uses seeded per-system or per-entity streams;
  - physics runs in a deterministic mode.
- **Authoritative sim state is isolated** from cosmetic code, so cross-platform determinism can be added later in a bounded audit.

## Events and triggers

- **Events are plain-data facts** held in typed per-tick buffers. Emitting one is an append. Per-thread emit buffers are merged in a deterministic order.
- **Delivery:** between phases, never immediately. Consumers process whole batches.
- **Cascades:** run in capped waves within the tick. Individual event types can opt into next-tick delivery instead.
- **Scheduled events** carry a target tick and sit in a timer wheel.
- **Triggers are indexed, not polled:**
  - spatial: broadphase overlap-pair diffs give enter and exit;
  - time: the timer wheel;
  - state: component version counters, firing on edges;
  - compound: small dataflow nodes.
- **Script-predicate triggers** are the only polled kind. They are throttled and their cost is reported.
- **Reactions:** subscriptions are data. Handlers are referenced by ID, never by function pointer, in authoritative state.
- **Stale handles** in events are dropped silently.
- **Conflicts** (several events on one entity) are resolved by the consumer, not by delivery order.
- **The event log** doubles as the causal debug trace, paired with deterministic replay.

## Audio, I/O, assets

- **Audio:** a realtime system on the device callback thread. It takes a state snapshot for continuous parameters plus an ordered event queue. Events carry their sim tick so they schedule sample-accurately inside a buffer.
- **I/O:** a batched async request queue with priorities, cancellation and completions. It maps onto io\_uring, DirectStorage, the PS5 path and Apple dispatch I/O. Residency streaming sits above it.
- **Assets:** cooked per platform into relocatable load-and-go blobs in hardware-native formats (BC or ASTC; meshlets with quantized vertices), stored in pack files.

## Language and ABI

- **Engine language:** orthodox C++ — no exceptions, no RTTI, no STL in the runtime, own containers, templates mainly for containers and handles, flat short type names.
- **Game interface:** a C ABI, coarse-grained: event batches, component spans and handles. Access modes are encoded in the types.
- **Game-side language:** left open (Swift, Lua, C#, C++). The engine does not depend on any of them.
- **Concurrency safety** comes from the data layer and the scheduler, not from the language.
- **Math:** use the platform libm. Move to a correctly rounded libm only if cross-platform determinism is ever required.

## Modules and dependencies

- **Everything above the core is a module:** renderers (2D, 3D), RHI backends, physics, particles, effects, entity types, importers, CLI commands.
- **Modules register through one registry**, as data:
  - component schemas, systems with read/write sets, event types, handler tables;
  - render-graph passes, asset cookers, CLI commands.
- **No static-initializer registration.** The build generates the module list.
- **Link-time modules** use the C++ API and are built with the same toolchain. The ecosystem distributes modules as source.
- **Runtime-loaded modules** (a stretch goal) use a versioned C-ABI function table, not the C++ ABI.
- **State lives in engine-owned pools**, so a runtime module can unload or reload without losing data.
- **Core contents only:** memory, handles, scheduler, events, I/O, platform, RHI interface, CLI.
- **Minimal builds:** a 2D platformer links the core + one RHI backend + the 2D renderer, and no 3D, RT or physics code. Size budget to be set.
- **Runtime third-party dependencies:** platform SDKs, the Vulkan loader, and SDL3 (subsystems trimmed). Physics middleware enters only as a module.
- **Shader compilation is a tool dependency only.** The runtime loads precompiled shaders.
- **Tooling dependency policy:**
  - single-purpose, widely used libraries with permissive licenses (libpng, zlib/zstd, cgltf, meshoptimizer class);
  - vendored, pinned, and built from source in our build;
  - no frameworks, no transitive dependency trees, no small unvetted libraries;
  - tool dependencies never leak into the runtime.

## Debugging and tooling

- **CLI** over a transport-agnostic line protocol: on-screen, stdin, TCP, devkit TTY, UART.
- **UART-safe output:** terse, ASCII, never blocks the sim (ring buffer with drop markers). Allocation-free parser.
- **CLI reads** run against the last committed tick. **CLI writes** become command events at a phase boundary and are recorded in the input log.
- **Addressing:** hierarchy paths, generational handles, queries.
- **Reflection** through component and event schemas. Handlers can be invoked by ID.
- **Sim control:** pause, step, rate, event breakpoints, trace, rewind and replay.
- **Machine-readable mode** with request IDs. It serves as the test harness and the editor/live-import protocol.
- **Rewind:** keyframe snapshots every N ticks plus the event log. Determinism is required only across one keyframe interval.
- **Drift detection:** per-system state hashes per tick locate where nondeterminism enters. Deliberate nondeterminism is allowed where it is measured.
- **Live import** into a running target: handle-stable swaps at frame boundaries, deferred GPU destruction, background pipeline compiles. On devkits it is a network protocol.
- **Engine provides** inspection views, schemas and world serialization. Level editors are a per-game concern (external tools first).

## Licensing and contributions

- **Core license:** Apache 2.0. Engine credit goes in the NOTICE file.
- **Contributions:** under a DCO, inbound = outbound. No copyright assignment.
- **AI-assisted contributions** are allowed. A human must review the change, understand it and sign off. Mark them with an `Assisted-by:` commit trailer.
- **Engine name** is protected by a short trademark policy: forks rename.
- **Proprietary modules and NDA console backends** live in separate repos.

## Deferred and open

- **Cross-platform determinism:** deferred, possibly never needed.
- **Default event latency:** same-tick waves or next-tick delivery.
- **Game-side language** for the first title.
- **Physics:** middleware or our own. Either way it must have a deterministic mode.
- **Console backends:** when they start, and which console first.

* **Binary size budget** for the minimal 2D build.
* **Runtime module loading:** whether it happens, and when.
* **SDL3:** keep it long-term, or replace it with our own platform layer.
