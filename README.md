# Korvid

Korvid is a multi-platform game engine with support for 2D accelerated graphics,
audio, keyboard/mouse/gamepad inputs, resource packs, collision, a built-in CLI,
and other niceties. It runs on Linux, Windows, macOS, iOS, and Android. Dependencies are
minimal; on most platforms you'll just need a local C++ compiler and a few
packages - SDL3 for desktops, slangc and Ninja if you don't already have them.

The engine is fully deterministic on each platform (though not across
platforms). This can make it _much_ easier to track down bugs, since replays
will trace the exact same paths through your code with just an initial state and
an input script.

The engine provides its own build tool, kormake, that understands its module
format and makes some tasks easier.

Korvid is written in what is sometimes called the Orthodox C++ style: No STL, no
exceptions, templates used sparingly, new/delete not used. Games are also C++
modules today, but more languages are coming. You'll get the best use out of
the engine if you choose a language that can work directly with data structures
in memory.

Some notable roadmap items:

  - Android support
  - 3D graphics
  - More animation types
  - Physics
  - Authoring environment
  - Additional language support

A fair amount of AI was used to develop this engine. All the music and art in 
the examples was created by humans and is credited. See 
`docs/development-notes.md` for details. 

Korvid builds from scratch in about 3 seconds on my laptop, which makes me very
happy.

## Quick Start

Install dependencies for your platform (see [Platform Notes](#platform-notes) 
for details):

- Linux: needs g++, SDL3, Vulkan, Ninja, slang compiler
  - Ubuntu distributions would look something like this, your mileage may vary:
    `apt-get install libsdl3-dev libvulkan-dev vulkan-validationlayers ninja-build slang-compiler`
    (the validation layers are for debug builds, which turn them on)

- Windows: needs Visual Studio with C++, SDL3, Vulkan SDK
  - unpack the SDL3 windows release into korvid/vendor/
  - this is pinned to 3.4.20 right now, needs work

- macOS: needs Xcode, sdl3, Ninja, slang compiler, Metal tools
  - Get the Metal tools: `xcodebuild -downloadComponent MetalToolchain`
  - Slang compiler: unpack it somewhere, put bin/ in your path
  - the rest: `brew install ninja sdl3`

In `tools/kormake`, run `bootstrap.sh` or `bootstrap.bat` for your platform. If
that reports an error, see BUILDING.md in that directory for how it's 
supposed to work. Add `tools/kormake/bin/<platform>` to your PATH.

Then in `examples`, run `kormake`. That should build all the examples, and any 
tools they need.

Try them out. The examples build into `build/<platform>-<config>/bin`, so
something like `build/linux-debug/bin/arcfire` should bring up one of the
example games.

Run tests: In `modules` or `examples`, run `kormake test`.

## Notable Engine Features

### Modules

Korvid is structured around modules. Each module is a collection of code,
headers, assets, and metadata. Modules can require other modules by name to be
part of the finished executable, and kormake keeps them all straight, detects
and complains about cycles, and handles all the compiling and linking. You are
encouraged to build your games in your own directories and repos;
`korvid.config` in the root of your project will help kormake find the engine
you want it to use and all the modules your project has and depends on. 

Korvid itself is entirely formed of modules, including all the tools (except
kormake).

Setting up a module is dead easy: You need a directory, a header file if others
will be calling you, usually some code in a file that kormake can compile, and a
module.lua that names the module and any of its dependencies. Put that somewhere
kormake can find it and you have your own module that others can depend on and
link with. Note that the toolchains and platforms are modules themselves.

### Cross-Platform

Korvid currently runs on Linux, Windows, macOS, iOS, and Android. It is designed as a
three-layer engine, with an outer ring of services and entities, an inner
rendering engine, and a platform rendering core. Service modules can easily 
adapt to the platform as well - I/O is just a service, as is windowing.

kormake also has built-in support for building binaries for the host machine in
a cross-compiling setup. This makes it possible for host tools to be built out
of the same modules as the game itself, and allows your toolchain to re-use the
same portability, windowing, and rendering features that are built into the
engine.

### Packaging

Korvid includes its own asset packager, `kpack`. Asset packs (`.kpak` files) are
ZIP files with an index and files stored uncompressed at 4KiB chunk alignment.
The asset loader will in fact work with any ZIP file, but it can do some nice
optimizations with a properly laid out .kpak file.

### Minimal Dependencies

Korvid rejects package creep. Most of the items that would drop you into 
dependency hell have been vendored directly in this repo. The big ones - the
compilers, vulkan support, SDL on desktops - are up to you, but if you
poke around in here you'll find vendored inflate/deflate, a Vorbis decoder, and 
other things that were better compiled in than linked from who-knows-where.

### Deterministic Engine

...details on hashing et al...


### Automatic Ordering

At almost no runtime cost, you can opt in to a scheduler that will guarantee
correct ordering of all your runtime systems and event handlers. You declare the
data components that each one reads and writes, and the scheduler solves it for
you at startup. If some constraint can't be satisfied, you get back a decent
description of exactly what's wrong.

You also get built-in tooling for putting some values aside at the beginning of
a tick if that's needed to keep your game consistent.

If you have spent too many days tracking down why something happens a frame
early or a frame late, or either one depending on the phase of the moon, this
system is for you.

### Parallel Execution

The engine provides a mechanism for spreading work across cores and still
keeping the game deterministic. It builds on the ordering constraints and can
guarantee in-order execution on just the parts that matter.

### Reflection

Most engine structures are reflected to allow inspection by name, typed field
handling, and various kinds of snapshots. Your game can use the same system to
reflect its types for log printing, snapshots, interaction through the command
line, and compatibility with the execution ordering and event systems.

## Engine Structure

## Platform Notes

### Linux
### Windows
### macOS
### iOS
### Android


---------------------------------------------
Old generated content, ignore

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

The engine's modules are in `modules/`, the examples in `examples/<name>/` and the tools
in `tools/<name>/`, each module named for its directory (`examples/arcfire` is `arcfire`,
which builds `bin/arcfire`); `tools/kormake` is kormake itself. `examples/` and `tools/` each have a
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
    testbed          the testbed: balls as a kind, one move system over a view, a bounce
                     handler, a blinker on scheduled events, a trigger at the third bounce
    rocks            an asteroids-like: ship, bullets, rocks and debris as kinds; control,
                     move, wrap and fade systems; overlaps become hits in handlers, rocks
                     split, bullets expire by timer, waves arrive by timer, the ship's death
                     is a trigger on its Health column, a restart follows; --demo scripts
                     the ship so runs reproduce
    stones           the same engine used the other way: plain structs in an Array, one
                     tick that does input, motion, n-squared collision and sprite syncing
                     in order; the engine supplies the window, the keys, the sprites and
                     reflection for the log, and nothing else is asked of it
    cpukiller        a stress tool, no window: the job pool's dispatch overhead and
                     contention, nested batches, the memory system under cross-thread
                     load, and the engine's tick rate on one thread against many with the
                     state hash as the referee; prints a table

    tools/kormake/bootstrap.sh                   # once (and after kormake changes): builds the build tool with the host C compiler
    tools/kormake/bin/linux/kormake              # at the root: everything in linux-debug: libraries, examples, shaders, assets, tests
    tools/kormake/bin/linux/kormake test         # runs the tests (add dev or ship to either for linux-dev, linux-ship)
    tools/kormake/bin/linux/kormake rocks dev    # one example and what it links, in linux-dev (or: cd examples/rocks && kormake dev)
    build/linux-debug/bin/testbed         # opens a window; Esc or close exits via ST_EXIT
    build/linux-debug/bin/rocks --demo    # the asteroids-like, flying itself; without --demo, you fly
    build/linux-debug/bin/stones          # stones in a box, written immediate-mode style
    build/linux-debug/bin/cpukiller       # the stress table (--threads N, --bodies N, --ticks N, --quick)
    build/linux-debug/bin/cpukiller --window  # the same with the window, renderer and swapchain alive
    build/linux-debug/bin/cpukiller --profile t.json  # plus a trace of the whole run
    build/linux-debug/bin/testbed --run-seconds 2   # exits by itself (smoke test)
    build/linux-debug/bin/testbed --no-validation   # debug builds enable the validation layers by default
    build/linux-debug/bin/testbed --no-vsync        # mailbox/immediate present instead of fifo
    build/linux-debug/bin/testbed --borderless      # no libdecor frame (see note below)
    build/linux-debug/bin/testbed --hitch-ms 50     # log where any frame over 50 ms spent its time
    build/linux-debug/bin/testbed --hash            # log the sim state hash per tick (--hash-systems: per system)
    build/linux-debug/bin/testbed --trace-events    # print every delivered event with its emitter (dev builds)
    build/linux-debug/bin/testbed --threads 1       # serial; the default uses every core
    build/linux-debug/bin/testbed --prof-report      # the top zones per frame with every fps line
    build/linux-debug/bin/testbed --profile t.json   # a trace for Perfetto (ui.perfetto.dev) or chrome://tracing

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
    build\windows-dev\bin\window.exe                      # a window, its events, and the frame loop's gaps

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
    build/macos-dev/bin/arcfire                 # the shooter; --demo flies itself
    MTL_DEBUG_LAYER=1 build/macos-debug/bin/testbed   # Metal's API validation (off unless asked for)
    tools/kormake/bin/macos/kormake package arcfire ship  # dist/arcfire/arcfire.app

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
    tools/kormake/bin/macos/kormake package iossim-dev window
    xcrun simctl install booted dist/window/window.app
    xcrun simctl launch --console-pty booted org.korvid.window   # its log in this terminal

The Simulator's GPU has no indirect draws, which the renderer is built on, so a game starts
there and stops at the renderer; drawing on iOS needs a device (A14 or later). On a device,
with an Apple developer account signed into Xcode, the device connected and in Developer
Mode, and a development profile Xcode has made (build any app to the device once):

    KORMAKE_IOS_TEST_BUNDLE_ID=com.example.tests tools/kormake/bin/macos/kormake test ios-dev   # every suite on the device
    tools/kormake/bin/macos/kormake package testbed ios-dev                            # signed for development
    xcrun devicectl device install app --device "<name>" dist/testbed/testbed.app
    xcrun devicectl device process launch --device "<name>" --console org.korvid.testbed

The bundle identifier is one your profile covers (a wildcard profile covers any). kormake
does the signing and the device runs itself, with what macOS ships (`security`, `plutil`,
`codesign`, `devicectl`); `tools/kormake/README.md` lists the variables it reads.
`DEVICECTL_CHILD_SDL_AUDIO_DRIVER=dummy` keeps the tests silent.

## Building for Android

From a Linux host (the other hosts are untried). Android is native: a `NativeActivity`, the
NDK's clang, AAudio, and the Vulkan renderer Linux and Windows draw with. No SDL, no
Gradle, no Android Studio: kormake compiles with the NDK and writes the `.apk` itself.
There are two platforms: `androidemu` is x86-64, for the emulator on an x86-64 host;
`android` is arm64, for a phone. Everything below has run in the emulator; the arm64 build
is built and packaged and has not yet met a device (`docs/design/android-port-notes.md`).

1. **The command-line tools** from developer.android.com/studio ("Command line tools
   only"), unpacked so that `sdkmanager` is at
   `~/Android/Sdk/cmdline-tools/latest/bin/sdkmanager`. In your shell's startup file, in
   this order:

       export ANDROID_HOME="$HOME/Android/Sdk"
       export PATH="$PATH:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator"

   kormake itself needs neither line: it looks under `ANDROID_HOME`, then `~/Android/Sdk`.
2. **The SDK's pieces**, about 3 GB to download and 8 GB installed (the versions are what
   `modules/toolchain-android/module.lua` pins; a Java runtime, 17 or later, runs the
   tools):

       sdkmanager "platform-tools" "build-tools;36.0.0" "platforms;android-36" "ndk;30.0.16248370" \
                  "emulator" "system-images;android-36;google_apis;x86_64"

3. **An emulator**, made once and started whenever it is wanted (it needs `/dev/kvm`):

       avdmanager create avd -n korvid -k "system-images;android-36;google_apis;x86_64" -d pixel_9
       emulator -avd korvid -gpu host -no-metrics
       adb shell settings put secure immersive_mode_confirmations confirmed   # once: Android's "viewing full screen" tip takes the focus

   `avdmanager` prints "Could not load devices from .../devices.xml" and makes the AVD all
   the same. `-gpu host` passes this machine's Vulkan through, which the renderer needs
   (1.3, descriptor indexing).

Then:

    tools/kormake/bin/linux/kormake test androidemu-dev            # every suite, run inside the emulator
    tools/kormake/bin/linux/kormake package androidemu-dev rocks   # dist/rocks/rocks.apk, signed with the debug key
    adb install -r dist/rocks/rocks.apk
    adb shell 'am start -n org.korvid.rocks/android.app.NativeActivity --es args "--demo --hash"'
    adb logcat -s korvid                                           # the game's output

Arguments go in the intent's `args` (the outer quotes keep adb's shell from splitting them
again). What a run writes with a relative path (`--screenshot shot.ppm`, `--profile`)
lands in `/sdcard/Android/data/org.korvid.<game>/files/`, where `adb pull` reads it; saves
go to the app's private directory. The window is the screen, and the size a game asks for
only chooses landscape or portrait; `adb shell wm size 540x960` gives the emulator a
960x540 screen for pictures that must be that size (`wm size reset` after). Touch drives
the on-screen pad every game already binds; a keyboard's Back, or the Back gesture, is
Escape.

`kormake package android-dev rocks` builds the arm64 package the same way; `adb install`
puts it on a connected phone, and `kormake test android-dev` runs the suites there.

A debug package of a game that draws carries the Khronos validation layer when one is
installed: unpack the `android-binaries` archive of a
[Vulkan-ValidationLayers release](https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases)
into `~/Android/Sdk/vulkan-validation-layers/` (so that `x86_64/` and `arm64-v8a/` are
directly inside it), or point `KORMAKE_ANDROID_VK_LAYERS` at wherever it is. The package's
last line says whether it took one.
