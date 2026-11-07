# Korvid

Step 2: a Vulkan 1.3 sprite renderer behind a render interface, sprite entities with
an extract, and an example game with a three-state machine (INIT, RUN, EXIT) that
bounces a textured ball over a tiled background. Textures and materials are
renderer-issued handles whose index is the GPU slot or row; a null or stale material
renders with the built-in red/white checkerboard.

Module registration is declarative (`core/module.h`): a module is one `constinit`
`ModuleDesc` with tables of event types, resources and systems, plus startup/shutdown for
OS resources and module state. Nothing registers itself and nothing runs before main;
the build-generated list (`hello/src/modules.gen.cc` for now) is the only input. Order is
list order, then declaration order: modules start in list order and stop in reverse,
event ids are assigned in that order, and within each tick phase (input, presim, sim,
postsim) systems run in module order then declaration order. `engine_init` rejects
duplicate names and unknown resource references and logs the schedule it built.

Modules follow the kormake directory convention (`module.lua`, `include/korvid/<name>/`,
`src/`, `shaders/`, `assets/`, `tests/`). A module's `assets/` are copied to
`build/bin/assets/<module>/` and loaded by that relative path. `build.ninja` is a
hand-written stand-in until kormake exists.

    core             memory (general, arenas, scratch, pools on reserved pages; see
                     docs/design/allocators.md), handles with a kind registry and C API
                     (docs/design/handles.md), events, phases, tick clock, log
    platform         SDL3 window and keyboard input (events on the bus); the I/O queue
                     (io_uring-shaped: submit, pump, reap; mocked with fread for now)
    math             header-only: vec2, point2, rect2, color, affine2, ortho2d
    render           header-only interface: RenderPacket, renderer API (textures,
                     materials, caps), MaterialDesc
    asset            load_texture returns a handle at once; the read rides the I/O queue,
                     decode (PNG on zlib) and upload happen in asset.pump, completion is
                     AssetReady/AssetFailed on the bus
    sprite           Transform2 + Sprite components, SpriteWorld, extract_sprites
    renderer-vulkan  provides "render": dynamic rendering, sync2, bindless textures,
                     Slang shader compiled to SPIR-V by the build; the only Vulkan user
    hello            the example game

    ninja            # builds build/bin/hello, build/bin/shaders/sprite.spv and the tests
    ninja check      # runs the tests
    build/bin/hello                   # opens a window; Esc or close exits via ST_EXIT
    build/bin/hello --run-seconds 2   # exits by itself (smoke test)
    build/bin/hello --no-validation   # debug builds enable the validation layers by default
    build/bin/hello --no-vsync        # mailbox/immediate present instead of fifo
    build/bin/hello --borderless      # no libdecor frame (see note below)
    build/bin/hello --hitch-ms 50     # log where any frame over 50 ms spent its time

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
