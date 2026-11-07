# Korvid

Step 2: a Vulkan 1.3 sprite renderer behind a render interface, sprite entities with
a per-tick delta extract into the renderer's persistent world, and an example game with
a three-state machine (INIT, RUN, EXIT) that bounces four balls over a tiled background:
horizontal, vertical, random, and one that blinks in and out of existence, each with its
own material so all three blend modes are on screen. Textures and materials are
renderer-issued handles whose index is the GPU slot or row; a null or stale material
renders with the built-in red/white checkerboard.

Module registration is declarative (`core/module.h`): a module is one `constinit`
`ModuleDesc` with tables of event types, component schemas, entity kinds, resources and
systems, plus startup/shutdown for OS resources and module state. A kind is a fixed set
of components with a capacity; the engine builds a table per kind (one SoA column per
component on reserved pages, a handle kind, a resource per column) and copies the `prev`
columns at the top of PreSim. Systems reach data through the `TickContext`: `write_kind`
for the table a system holds the write on, `read<T>`/`write<T>` for one column by name,
`view<A, const B>()` for every kind with those components (constness is the access mode),
`resource<const T>(name)` for what a module published under one of its declared resources
(the window, the input state), and `spawn_later`/`destroy` commands applied at the phase
boundary. Names resolve to ids once with `resource_id`, `column_id` and `kind_id`. In dev
builds an access the system did not declare asserts with the system and resource named. Nothing registers itself and nothing runs before main;
the build-generated list (`example_testbed/src/modules.gen.cc` for now) is the only input. Modules
start in list order and stop in reverse, and event, component and kind ids are assigned in
list then declaration order. Within each tick phase (input, presim, sim, postsim) the
order is derived from the systems' read/write sets: a reader runs after every writer of
what it reads, `after`/`before` add edges by hand, two writers of one resource with no
path between them or a cycle is a registration error naming the systems, and ties break
by module then declaration order. A system that reads a cosmetic column may not write a
sim one. `engine_init` logs the order and the reason for every edge. `--hash` logs a hash
of every sim column of every live entity after each tick, `--hash-systems` after each
system, to locate where two runs diverge.

Events are plain data in typed per-tick buffers, delivered at phase boundaries. An event
type may carry a field schema (`KV_EVENT_EX`); then an event whose handle no longer
resolves is dropped at delivery and counted, and the dev trace (`--trace-events`) prints
every delivered event with who emitted it. Modules declare handlers (`KV_ON(Event, fn)`)
that run at every boundary on the new batch of their event, in waves: what a wave emits
is delivered to the next, up to a cap of four, and what remains waits for the next
boundary. Handlers have read/write sets and are ordered like systems. A type flagged
`EvNextTick` is held until the next tick's first boundary. Systems with no path between them share a level and run at once on the job pool
(`--threads N`; a module's own state is an implicit resource its systems write unless they
declare `.state = StateUse::None`); `parallel_for(ctx, rows, grain, fn)` splits one system's
rows over the pool, each chunk emitting and spawning into buffers merged in chunk order, so
hashes agree across thread counts. `ctx.schedule(event, tick)`
files an event in the timer wheel for the first boundary of that tick. Triggers declare
an edge on one field of a `prev` component (`Changed`, `CrossedBelow`, `CrossedAbove`);
the engine evaluates them at the end of every tick and emits `StateEdge` for the next.

Modules follow the kormake directory convention (`module.lua`, `include/korvid/<name>/`,
`src/`, `shaders/`, `assets/`, `tests/`). A module's `assets/` are copied to
`build/bin/assets/<module>/` and loaded by that relative path. `build.ninja` is a
hand-written stand-in until kormake exists.

    core             memory (general, arenas, scratch, pools on reserved pages; see
                     docs/design/allocators.md), handles with a kind registry and C API
                     (docs/design/handles.md), component schemas (field tables that hash,
                     compare and print rows), entity kinds (engine-owned tables),
                     Map<K, V> (dense entries, index past 16),
                     Name interning, Rng (PCG32), events, phases, tick clock, log
    platform         SDL3 window and keyboard input (events on the bus); the I/O queue
                     (io_uring-shaped: submit, pump, reap; mocked with fread for now)
    math             header-only: vec2, point2, rect2, color, affine2, ortho2d, and their
                     schema field traits (fields.h)
    render           header-only interface: the SpriteDelta stream, renderer API
                     (textures, materials, caps, apply, render), MaterialDesc
    asset            load_texture returns a handle at once; the read rides the I/O queue,
                     decode (PNG on zlib) and upload happen in asset.pump, completion is
                     AssetReady/AssetFailed on the bus
    sprite           Transform2 + Sprite schemas, the kind "sprite.entity" with the
                     SpriteWorld convenience surface over its table, and the extract: each
                     tick, for every kind with both components, a Tick marker plus a Set
                     for every entity that is new or changed and a Remove for every one
                     destroyed (sprite.extract, PostSim)
    renderer-vulkan  provides "render": a persistent sprite world keyed by entity handle
                     with previous and current state per sprite, dynamic rendering, sync2,
                     bindless textures, Slang shader compiled to SPIR-V by the build; the
                     only Vulkan user
    collision        Collider2 (circle or box, layer and mask) and the broadphase: a uniform
                     grid over every kind with Transform2 and Collider2, overlap pairs diffed
                     tick to tick into OverlapBegin and OverlapEnd (collision.broadphase, Sim)
    frameloop        what every game needs and no game decides: options, window and
                     renderer from a FrameLoopConfig, the loop (pump, ticks, extract,
                     render), resize, reporting, ExitRequested on the bus
    example_testbed  the testbed: balls as a kind, one move system over a view, a bounce
                     handler, a blinker on scheduled events, a trigger at the third bounce
    example_rocks    an asteroids-like: ship, bullets, rocks and debris as kinds; control,
                     move, wrap and fade systems; overlaps become hits in handlers, rocks
                     split, bullets expire by timer, waves arrive by timer, the ship's death
                     is a trigger on its Health column, a restart follows; --demo scripts
                     the ship so runs reproduce
    example_stones   the same engine used the other way: plain structs in an Array, one
                     tick that does input, motion, n-squared collision and sprite syncing
                     in order; the engine supplies the window, the keys, the sprites and
                     reflection for the log, and nothing else is asked of it
    example_cpukiller a stress tool, no window: the job pool's dispatch overhead and
                     contention, nested batches, the memory system under cross-thread
                     load, and the engine's tick rate on one thread against many with the
                     state hash as the referee; prints a table

    ninja            # builds build/bin/example_testbed, build/bin/shaders/sprite.spv and the tests
    ninja check      # runs the tests
    build/bin/example_testbed         # opens a window; Esc or close exits via ST_EXIT
    build/bin/example_rocks --demo    # the asteroids-like, flying itself; without --demo, you fly
    build/bin/example_stones          # stones in a box, written immediate-mode style
    build/bin/example_cpukiller       # the stress table (--threads N, --bodies N, --ticks N, --quick)
    build/bin/example_cpukiller --window  # the same with the window, renderer and swapchain alive
    build/bin/example_testbed --run-seconds 2   # exits by itself (smoke test)
    build/bin/example_testbed --no-validation   # debug builds enable the validation layers by default
    build/bin/example_testbed --no-vsync        # mailbox/immediate present instead of fifo
    build/bin/example_testbed --borderless      # no libdecor frame (see note below)
    build/bin/example_testbed --hitch-ms 50     # log where any frame over 50 ms spent its time
    build/bin/example_testbed --hash            # log the sim state hash per tick (--hash-systems: per system)
    build/bin/example_testbed --trace-events    # print every delivered event with its emitter (dev builds)
    build/bin/example_testbed --threads 1       # serial; the default uses every core

Needs g++ (or clang++), ninja, libsdl3-dev, libvulkan-dev, zlib1g-dev, slang-compiler,
and for debug runs vulkan-validationlayers. Nothing links libvulkan: the loader is reached
through SDL and every entry point is loaded at runtime (renderer-vulkan/src/vk_funcs.h).

Known platform issue: on GNOME Wayland, SDL3 falls back to libdecor for the window frame
(GNOME has no server-side decorations). libdecor repaints the frame synchronously inside
every xdg configure, and Mutter sends one configure per pointer motion during a mouse
resize, so the event pump stalls for the length of the drag. Measured here: 2.5 ms per
configure with the cairo plugin, 4.3 ms with the GTK plugin, at hundreds of configures
per second. Keyboard resizes and borderless windows are unaffected. Nothing in the
engine runs during the stall; `--hitch-ms` shows it as time spent in the pump.
