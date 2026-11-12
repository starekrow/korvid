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
the build-generated list (`build/<target>/gen/<exe>/modules.gen.cc`) is the only input. Modules
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
declare `.state = StateUse::None`; a system that talks to the OS, the GPU or a module's own
pools declares `.main_thread = true` and runs on the driving thread); `parallel_for(ctx, rows, grain, fn)` splits one system's
rows over the pool, each chunk emitting and spawning into buffers merged in chunk order, so
hashes agree across thread counts. The profiler (`core/profile.h`, docs/design/profiler.md)
records zones, waits, marks and counters into a ring per thread; every system, handler,
boundary, chunk and frame is zoned for free, `--prof-report` logs the top zones with the fps
line, `--profile FILE` writes a Chrome trace, and `--hitch-ms` dumps the hitching frame's tree. `ctx.schedule(event, tick)`
files an event in the timer wheel for the first boundary of that tick. Triggers declare
an edge on one field of a `prev` component (`Changed`, `CrossedBelow`, `CrossedAbove`);
the engine evaluates them at the end of every tick and emits `StateEdge` for the next.

Modules follow the kormake directory convention (`module.lua`, `include/korvid/<name>/`,
`src/`, `src/<platform>/`, `shaders/`, `assets/`, `tests/`). kormake (`tools/kormake`, its
own README there) reads `kormake.lua` and every `module.lua`, resolves the instance graph
for a platform and config, and writes `build/<platform>-<config>/build.ninja` plus
`compile_commands.json`. It generates each executable's module list in dependency order,
one unity file per module, copies a module's `assets/` to `bin/assets/<module>/`, and
runs the bound shader compiler over `shaders/`. The toolchain and the shader compiler are
modules too (`toolchain-linux`, `slang`).

The engine's modules are in `modules/`, the examples in `examples/<name>/` (module
`example_<name>`) and the tools in `tools/<name>/` (`tools/kpack` is `tool_kpack`, which
writes `bin/kpack`); `tools/kormake` is kormake itself. `examples/` and `tools/` each have a
`korvid.config` naming the engine root, and a game kept outside this repository does the
same (`engine "../korvid"`): it then builds into its own `build/`. kormake builds what you
name, by module, executable or directory name, or with no name the module whose directory
you are in; at the root it builds everything. `tools/kormake/README.md` is the manual,
`tools/kormake/BUILDING.md` how kormake is built and how to read a failed build.

    core             memory (a size-class general allocator with per-thread pages and
                     remote frees, arenas, scratch, pools on reserved pages, heaps with soft
                     and hard limits from the root's EngineConfig; see docs/design/allocators.md), handles with a kind registry and C API
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
    toolchain-linux  the Linux platform as a module: compiler, flags per config, what each
                     external means here, and the default bindings (render, shader-compile)
    slang            the shader compiler as a module: provides shader-compile and the rule
                     that turns .slang into SPIR-V; a system lookup until a binary is vendored
    touchpad         a gamepad on a touch screen: a stick where the left thumb lands, buttons
                     as translucent circles, laid out by JSON (touchpad/default.json); it
                     fills a pad slot, so a game's pad bindings work on a phone unchanged.
                     On where the screen is touched; --touchpad tries it with the mouse
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

    tools/kormake/bootstrap.sh                   # once (and after kormake changes): builds the build tool with the host C compiler
    tools/kormake/bin/linux/kormake              # at the root: everything in linux-debug: libraries, examples, shaders, assets, tests
    tools/kormake/bin/linux/kormake test         # runs the tests (add dev or ship to either for linux-dev, linux-ship)
    tools/kormake/bin/linux/kormake rocks dev    # one example and what it links, in linux-dev (or: cd examples/rocks && kormake dev)
    build/linux-debug/bin/example_testbed         # opens a window; Esc or close exits via ST_EXIT
    build/linux-debug/bin/example_rocks --demo    # the asteroids-like, flying itself; without --demo, you fly
    build/linux-debug/bin/example_stones          # stones in a box, written immediate-mode style
    build/linux-debug/bin/example_cpukiller       # the stress table (--threads N, --bodies N, --ticks N, --quick)
    build/linux-debug/bin/example_cpukiller --window  # the same with the window, renderer and swapchain alive
    build/linux-debug/bin/example_cpukiller --profile t.json  # plus a trace of the whole run
    build/linux-debug/bin/example_testbed --run-seconds 2   # exits by itself (smoke test)
    build/linux-debug/bin/example_testbed --no-validation   # debug builds enable the validation layers by default
    build/linux-debug/bin/example_testbed --no-vsync        # mailbox/immediate present instead of fifo
    build/linux-debug/bin/example_testbed --borderless      # no libdecor frame (see note below)
    build/linux-debug/bin/example_testbed --hitch-ms 50     # log where any frame over 50 ms spent its time
    build/linux-debug/bin/example_testbed --hash            # log the sim state hash per tick (--hash-systems: per system)
    build/linux-debug/bin/example_testbed --trace-events    # print every delivered event with its emitter (dev builds)
    build/linux-debug/bin/example_testbed --threads 1       # serial; the default uses every core
    build/linux-debug/bin/example_testbed --prof-report      # the top zones per frame with every fps line
    build/linux-debug/bin/example_testbed --profile t.json   # a trace for Perfetto (ui.perfetto.dev) or chrome://tracing

Needs a C compiler for the bootstrap, g++ (or clang++ via `--cxx`), ninja, libsdl3-dev,
libvulkan-dev, zlib1g-dev, slang-compiler, and for debug runs vulkan-validationlayers. Nothing links libvulkan: the loader is reached
through SDL and every entry point is loaded at runtime (renderer-vulkan/src/vk_funcs.h).

## Building on Windows

Windows is being brought up a module at a time; the `windows` root in `kormake.lua` lists
what builds there so far. It uses MSVC and the SDL project's prebuilt SDL3 package.

1. **Visual Studio 2022 or later, or the Build Tools**, with the "Desktop development
   with C++" workload. Nothing else is needed from it: kormake finds `cl`, the Windows SDK
   and Visual Studio's Ninja itself (through `vswhere`), so any prompt works, not only a
   Developer prompt.
2. **SDL 3.4.20, the Visual C++ package**, unpacked into `vendor/` at the repository root
   (git ignores it):
   - Download `SDL3-devel-3.4.20-VC.zip` (16.1 MB) from
     <https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20>.
   - Check it: `certutil -hashfile SDL3-devel-3.4.20-VC.zip SHA256` must print
     `1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969`.
   - Unpack it into `vendor/` so that this file exists:
     `vendor/SDL3-3.4.20/include/SDL3/SDL.h`. (The archive holds one folder,
     `SDL3-3.4.20`; unpack it as it is, not its contents.)
   - Kept somewhere else? Set `KORMAKE_SDL3_DIR` to the folder that holds `include\` and
     `lib\` instead.

   The version is pinned in `modules/toolchain-windows/module.lua`; moving to another one
   changes it there and here. A build without the package stops at `gen` with these
   instructions; executables that use SDL get `SDL3.dll` copied beside them.
3. **The Vulkan SDK** (LunarG): `winget install KhronosGroup.VulkanSDK`, or the installer
   from <https://vulkan.lunarg.com/sdk/home>. It brings the Vulkan headers, `slangc` (the
   shader compiler) and the validation layers that debug builds turn on. Its installer
   sets `VULKAN_SDK` and puts its `Bin` on `PATH`; open a new prompt afterwards so both
   are seen. `KORMAKE_VULKAN_DIR` overrides where kormake looks. Running a build needs
   only the GPU driver: `vulkan-1.dll`, the loader, comes with it.

Then, from any prompt:

    tools\kormake\bootstrap.bat                                   # once: builds kormake.exe with cl
    tools\kormake\bin\windows\kormake.exe dev                     # windows-dev: what the windows root lists, with tests
    tools\kormake\bin\windows\kormake.exe test dev                # runs them
    tools\kormake\bin\windows\kormake.exe arcfire dev             # one game and what it links
    build\windows-dev\bin\example_window.exe                      # a window, its events, and the frame loop's gaps

kormake.exe carries its scripts inside it, so run `bootstrap.bat` again after a pull that
changes `tools\kormake\scripts\`; a stale one stops at `gen` on a field it does not know.

Known platform issue: on GNOME Wayland, SDL3 falls back to libdecor for the window frame
(GNOME has no server-side decorations). libdecor repaints the frame synchronously inside
every xdg configure, and Mutter sends one configure per pointer motion during a mouse
resize, so the event pump stalls for the length of the drag. Measured here: 2.5 ms per
configure with the cairo plugin, 4.3 ms with the GTK plugin, at hundreds of configures
per second. Keyboard resizes and borderless windows are unaffected. Nothing in the
engine runs during the stall; `--hitch-ms` shows it as time spent in the pump.

## Building on macOS

macOS 26 or later on Apple silicon. The renderer is `renderer-metal`; everything the
Linux build has builds here (the `macos` root in `kormake.lua`).

1. **Xcode** from the App Store, opened once to accept its license, then the Metal
   compiler, which Xcode no longer installs by itself:

       xcodebuild -downloadComponent MetalToolchain

   kormake asks `xcrun` for the compiler and the SDK, so no particular `PATH` or shell is
   needed. If `xcrun` finds the Command Line Tools instead of Xcode, point it at Xcode:
   `sudo xcode-select -s /Applications/Xcode.app`.
2. **Ninja and SDL 3.4.20** from Homebrew: `brew install ninja sdl3`. The version is
   pinned in `modules/toolchain-apple/module.lua`, as on Windows. Elsewhere than
   `/opt/homebrew/opt/sdl3`? Set `KORMAKE_SDL3_DIR` to the folder that holds `include/`
   and `lib/`.
3. **slangc**, the shader compiler: the macOS release from
   <https://github.com/shader-slang/slang/releases>, unpacked anywhere, its `bin/` on
   `PATH`. It drives Xcode's Metal compiler to write `.metallib` files.

Then:

    tools/kormake/bootstrap.sh                          # once, and after a pull that changes tools/kormake/scripts/
    tools/kormake/bin/macos/kormake dev                 # macos-dev: the macos root, with tests (debug, ship likewise)
    tools/kormake/bin/macos/kormake test dev            # runs them
    build/macos-dev/bin/example_arcfire                 # the shooter; --demo flies itself
    MTL_DEBUG_LAYER=1 build/macos-debug/bin/example_testbed   # Metal's API validation (off unless asked for)
    tools/kormake/bin/macos/kormake package arcfire ship  # dist/example_arcfire/example_arcfire.app

`kormake package` writes an application bundle: the executable in `Contents/MacOS/`, its
pack, shaders and licenses in `Contents/Resources/` (where the engine's base path looks
inside a bundle), Homebrew's SDL3 copied into `Contents/Frameworks/` and linked by
`@rpath`, an `Info.plist`, and a signature. The signature is ad-hoc, which runs on the
machine that built it; set `KORMAKE_CODESIGN_IDENTITY` to a Developer ID to sign for
others (notarizing is up to you: `xcrun notarytool`). `build/macos-ship/package.log` has
what the packager ran.

iOS is coming up the same way (`ios` and `iossim` in `kormake.lua`), native (UIKit, Core
Audio, Metal; no SDL): the same Xcode, with its iOS platform installed. The engine's tests
run in a booted Simulator, and an app installs there:

    xcrun simctl boot "iPhone 18 Pro"                      # any simulator; or open Simulator.app
    tools/kormake/bin/macos/kormake test iossim-dev        # builds for the Simulator, runs each test in it
    tools/kormake/bin/macos/kormake package iossim-dev example_window
    xcrun simctl install booted dist/example_window/example_window.app
    xcrun simctl launch --console-pty booted org.korvid.example-window   # its log in this terminal

The Simulator's GPU has no indirect draws, which the renderer is built on, so a game starts
there and stops at the renderer; drawing on iOS needs a device (A14 or later). On a device,
with an Apple developer account signed into Xcode, the device connected and in Developer
Mode, and a development profile Xcode has made (build any app to the device once):

    KORMAKE_IOS_TEST_BUNDLE_ID=com.example.tests tools/kormake/bin/macos/kormake test ios-dev   # every suite on the device
    tools/kormake/bin/macos/kormake package ios-dev example_testbed                            # signed for development
    xcrun devicectl device install app --device "<name>" dist/example_testbed/example_testbed.app
    xcrun devicectl device process launch --device "<name>" --console org.korvid.example-testbed

The bundle identifier is one your profile covers (a wildcard profile covers any).
`tools/ios/device_test.sh` and `tools/ios/sign_app.sh` say what else they read from the
environment.
