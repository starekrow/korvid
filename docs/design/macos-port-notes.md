# macOS port notes

Slice 7 (docs/PRDs/Engine-Slice-7-PRD.md). Every place the engine assumed Linux or x64, what
was done, and what is still open, milestone by milestone, in the manner of
`arcanum-notes.md`. Apple silicon (arm64, 16 KiB pages), macOS 26.

## Milestone 1: ground

### The build

- **No toolchain.** kormake already bootstrapped and ran on macOS (`os_posix.c` had its
  `__APPLE__` branch); it stopped at "no toolchain module provides platform 'macos'".
  `modules/toolchain-macos` is the Linux module's rules with Apple's clang, and a new probe,
  `probe_xcode`, asks `xcrun` for the compiler, `ar` and the SDK's path, so the Ninja file
  holds them outright and no particular shell or PATH is needed. The sandbox learned the
  toolchain field `sdk`.
- **The deployment floor is macOS 26.** Homebrew's SDL3 is built for 26; linking it from a
  build that claimed 14 drew a linker warning on every executable and meant the floor was
  not real. Older releases can wait (decided Nov 10). Packaging will need an SDL3 built
  for whatever floor ships, which is the same scripted source build iOS needs.
- **`-ffp-contract=off`.** arm64 clang fuses multiply-adds by default; the x64 builds of the
  same source do not. Each platform still keeps its libm, so replays still diverge across
  platforms; the flag only keeps the compiler from adding a difference of its own.
- **A gcc-only warning.** `inflate.cc` silenced `-Wclass-memaccess`, which clang does not
  know, and clang's `-Werror` refuses an unknown warning group. The pragma is now gcc's alone.

### The posix sources

- **Sockets** (`platform/src/posix/net_posix.cc`). `SOCK_CLOEXEC`, `SOCK_NONBLOCK` and
  `accept4` do not exist on macOS. There, a socket is made plain and `fcntl` sets
  close-on-exec and non-blocking right after; an accepted socket gets the same, and
  `SO_NOSIGPIPE` once per socket replaces `MSG_NOSIGNAL` per send (the SDK declares
  `MSG_NOSIGNAL` without saying which kernels honour it, so it is not relied on). Linux is
  unchanged. `example_sockit2m` echoes 120 MB over loopback byte for byte through it.
- **Paths** (`platform/src/macos/paths_macos.cc`, new). The executable's directory from
  `_NSGetExecutablePath` and `realpath`; the preference directory
  `~/Library/Application Support/<org>/<app>/`, where SDL puts it. Inside an `.app` the
  executable's directory is `Contents/MacOS/`; finding `Resources/` from it is milestone 6's.
- **`/proc/self/exe` in an example.** `example_sockit2m` read it for its working directory
  instead of calling `platform_exe_dir`, which existed for exactly that; it calls it now,
  on every platform.
- **Page-cache eviction.** `sockit2m`'s cold pass evicts its files with `posix_fadvise`,
  which macOS lacks, and macOS has no unprivileged way to drop one file's pages. There the
  files are flushed and the eviction reports failure; the run already warns that the cold
  pass may be warm.
- **Decommit.** `MADV_DONTNEED` is advice on macOS: pages may stay resident and keep their
  bytes, where Linux's (on private anonymous memory) and Windows's `MEM_DECOMMIT` return
  zeros on the next commit. Nothing calls `os_decommit` yet, but the next caller would
  assume the zeros; macOS now maps fresh `PROT_NONE` memory over the range with
  `MAP_FIXED`, which frees the pages and keeps the reservation, and the header states the
  promise. `MAP_NORESERVE` exists on macOS (and is ignored), so reserve needed nothing.
- **16 KiB pages.** Nothing assumed 4 KiB: the small allocator carves its own 64 KiB pages
  at 64 KiB alignment, and everything else rounds with `os_page_align`.

### Weak memory

x64 orders loads with loads and stores with stores, so until now an access missing its
acquire or release could pass every test. Every lock-free structure was read against arm64:

- **Correct as written:** the job pool (its queue is under a mutex; a chunk's writes reach
  the waiter through the `ACQ_REL` increment of `done` and its `ACQUIRE` read), the handle
  registry (rows published by `live`), the mixer's command and return rings and the stream
  ring (single producer, single consumer, release on publish, acquire on read), the
  allocator's remote frees (the block's link is published by the CAS, the owner takes the
  list with an exchange), the spinlocks, and the name table (a plain `bool` behind a lock).
- **Fixed: the profiler's lap check** (`core/src/profile.cc`). The collector reads a record
  with plain loads, then reads the writer's index again to see whether the record was
  overwritten meanwhile. An acquire load keeps later loads after it, not earlier ones, so
  on arm64 the record's loads could be satisfied after the check passed. An acquire fence
  between them is the seqlock reader's rule; `atomic.h` gained `atomic_fence` for it (a
  compiler barrier on x64, `dmb` on arm64). The check was also one short on every platform:
  at `write == i + cap` the writer may be filling slot `i`, so that is lapped too, and an
  overrun ring is now read from one past its oldest slot.
  The writer needed the seqlock writer's half as well (found reviewing the branch on Linux,
  Nov 10): it stored a record's slot after the release that published the previous index,
  and a release orders only what comes before it, so a lapping record could become visible
  before the index that marks the lap. A release fence at the top of `put` and `put_at`
  keeps the slot's stores after it (again a compiler barrier on x64).
- **Fixed: the allocator stranded pages under concurrent remote frees.** See below. Not
  arm64's: Linux and Windows had it too.
- **Minor, left:** `kv_handle_kind_name`, `_name_hash` and `_desc_get` check only the kind
  count, which is bumped before the row is written, not the row's `live` flag. Only a
  reader racing registration with an id it got unsynchronized could see a half-written
  row; the engine registers at startup.

### The allocator's notify stacks (fixed)

A remote free into a page whose remote list is empty pushes the page onto its owner's
notify stack, so the owner learns that a full page has blocks again. The owner also empties
remote lists in `page_take` without taking the page off the notify stack, so the next
remote free pushes it a second time while it is still there: its one `notify_next` link is
rewritten and everything below it on the stack is cut off. Those pages stay on the owner's
full list with free blocks no one reclaims. Two more windows lead to the same place: a
remote freer can still be pushing after the owner has returned the emptied page to the pool
and another thread has re-initialized it, and an adopted orphan may still be linked on its
dead owner's stack. Nothing crashes (pages are never unmapped and the walk nulls each link
as it goes); memory grows.

Measured with four threads allocating continuously and four freeing each block on another
thread, live blocks bounded at 16,384: 3,988 pages (250 MB) carved in 10 seconds and
climbing linearly. A scratch patch that lets a page be on a notify stack at most once (a
flag the remote freer sets with an exchange and the owner clears as it pops) holds the
same run at 36 pages. `core`'s heap test frees in phases, never while the owner allocates,
which is why it had not shown.

The fix (`core/src/memory.cc`) gives each page a notify state, idle, pushing, queued or
adopting, with a generation in the rest of the word:

- Only the remote free that moves a page from idle to pushing pushes it; the owner sets it
  idle as it pops it, after reading its link. A pusher's last step, pushing to queued, is a
  compare-and-swap on its own generation, so a pop that came first leaves the page idle
  and a slow pusher never mistakes a later push for its own.
- A remote freer drops the page's live count last, after any push. While the count holds
  its block the page cannot reach the pool and be re-initialized under the push, which
  closes the window where a freer wrote the link and read the class of a page another
  thread had taken.
- An emptied page goes back to the pool only when idle; a queued one waits to be collected.
- Adopting an orphan waits out a push in flight, then takes the page's notifications for
  its new owner; its link on the dead owner's stack is abandoned, and no one walks a dead
  thread's stacks (an exiting thread also drops its own, so a destructor that allocates
  later starts fresh ones).

The regression test, `core/tests/remote_free_test.cc`, needed three tries to be a test.
Its first form freed every block at once through a small ring, which put each class on one
page, and a stack with one page cannot lose another; it passed against the bug. Its second
form held a fixed delay before every free, which never freed into the page the owner was
allocating from, and the double push needs that; it passed against the bug too. The third
frees even blocks a fixed 4,096 later (many pages a class) and odd blocks at once (traffic
into the active page), with the producer held near both, so the blocks in flight are the
same on any machine in any config. Against the bug it carves 10,500 to 12,100 pages in a
round, in every config; with the fix 160 to 250 for two rounds, under a bound of 512.
(Growth after a warm-up was the wrong measure: the peak working set creeps up to its
ceiling over a million blocks or so, slower in debug.) With it, `core_tests` ran 100 times
in dev and 50 in debug and in ship with no failure.

## Milestone 2: the window and the shader spike

### The window

- **SDL3's Cocoa backend serves as is.** `src/sdl3/` and `src/sdl3_pads/` were Linux's;
  `example_window` opens through the `cocoa` video driver, logs its events and runs at the
  250 Hz it paces itself to, with gaps of about 5 ms once up (one of 27 to 104 ms while the
  window first appears). Cmd+Q already worked: SDL reports it as `SDL_EVENT_QUIT`, which the
  pump already made a `CloseRequested`.
- **Retina.** The window opened at 960x540 "px" on a 2x display: SDL3 on macOS gives a window
  its backing pixels only when asked (`SDL_WINDOW_HIGH_PIXEL_DENSITY`), and without it the
  system scales a quarter of the pixels up. Asked for on Apple; the window is now 1920x1080
  px for a 960x540 request, which is what `width()` and `height()` promise. Not on Linux,
  where at a Wayland scale it would change the framebuffer, and every measurement made there,
  by the scale squared: a decision for Linux, not a side effect of this port.
- **The Command keys.** SDL reports them (`SDL_SCANCODE_LGUI`, `RGUI`) and the engine's `Key`
  had no member for them, so they were dropped. `Key::LGui` and `RGui` (Command on a Mac,
  the Windows key on a PC; SDL's name) are appended, so every existing key keeps its number;
  Win32 maps them to E0 5B and E0 5C. Option was already `LAlt` and `RAlt`. `InputState`'s
  bit set grew from one word to two (it is sized from `Key::Count`); replays record actions
  by name, never key bits, so no recording changes.
- **`WindowSurface::Metal`** opens the window with `SDL_WINDOW_METAL`, which
  `SDL_Metal_CreateView` needs.
- **Live resize held the loop for the whole drag.** Grabbing a corner runs AppKit's
  event-tracking loop on the main thread, inside `SDL_PollEvent`, until the mouse is released:
  `example_window` showed one gap as long as the drag. Windows answered the same thing with a
  window thread of its own; AppKit wants its windows and their events on the main thread, so
  here the frames run inside AppKit's loop instead, as SDL documents (live-resize expose
  events may be drawn from directly). A CoreFoundation timer in the event-tracking run-loop
  mode alone fires every 4 ms there, mouse moving or not, and never in an ordinary pump;
  each tick drains SDL's queue into the bus without pumping the OS (`SDL_PeepEvents`,
  `HostWindow::drain`) and calls the frame function a frame loop gave
  `platform_modal_frame`. Measured by hand: 250 frames a second through the drag, every one
  from inside the resize, longest gap 6.0 to 6.5 ms (4.6 to 5.0 ms outside it: the timer's
  phase sets the pace there, not the loop's sleep); 11.2 ms once, as the mouse is released
  and the loop takes over again. Nothing reaches the 20 ms hitch line. The I/O queue is not
  pumped while the resize lasts. The game frame loop takes the same hook when it runs here
  (milestone 3).
- **Not checked by a person yet:** Cmd+Q, the Command keys, Mission Control and full screen.

### The shaders

The result is `docs/design/renderer-metal.md`. In short: every shader compiles for Metal
from the same sources; the one change is the bindless texture array, which Metal cannot
express unbounded and cannot bind as a fixed array of 1024 to a stage, and which becomes an
argument buffer (a `ParameterBlock`) under `KV_TARGET_METAL`. Unset, Slang's output for every
shader is byte-identical to before. Slang compensates for Metal's `instance_id` including the
base instance. Binding indices are Slang's own, stable but implicit, so the build will carry
Slang's reflection and a dev build will check the backend's layouts against it.

Toolchain findings: Xcode 26 ships its Metal compiler as a separate download
(`xcodebuild -downloadComponent MetalToolchain`), so `xcrun metal` fails until it is
installed; the spike compiled MSL through the Metal framework at run time instead. Slang's
macOS release cannot emit SPIR-V (it has no `libslang-glslang`), which matters only for
checking Vulkan shaders on a Mac.

## Milestone 3: the Metal renderer

`docs/design/renderer-metal.md` has how the backend maps onto Metal. What the milestone
changed around it, and found:

- **kormake** compiles `.mm` sources (their own unity file; clang takes the language from the
  extension), links an external's `frameworks` (`flag_framework`), lets a tool rule carry
  per-platform variants (the shader rule's Metal form: `slangc -target metallib`, which drives
  Xcode's Metal compiler and writes a depfile naming its output, plus Slang's reflection), and
  stages a header-only module's shaders and assets. Linux's and macOS's generated Ninja files
  were byte-identical before and after each change, until the shaders moved.
- **The shaders moved to the render interface** (`render/shaders/`): on macOS renderer-vulkan
  is not in the build, so the header a game's shaders include by name was not on the include
  path, and the Metal backend had nothing of its own to compile.
- **The frame loop** opens the window the bound backend wants (`renderer_surface()`, a new
  function of the render interface) and runs its frames through a live resize
  (`platform_modal_frame`), sharing its clock with the loop.
- **The outer engine joins the macOS root**: asset, sprite, scene, text, anim, collision,
  tilemap, level, audio, ui and the frame loop, with their suites (21 in all, every config),
  and `example_testbed`, `example_stones` and `example_cpukiller` run on Metal.
- **`asset`'s vendored stb_vorbis** silenced a gcc-only warning group, and clang flags a
  comparison inside it that gcc does not: both handled as `inflate`'s were.
- **An engine bug, every platform: an entity's description could run past its buffer.**
  `Table::describe` wrote the space between two components into the last byte when the one
  before ended one byte short, leaving no terminator; the testbed's log printed a stray byte
  after the blinker's line on macOS (`0xF9`, whatever followed on the stack). Separators now
  go in only with room for a terminator; `kind_test` truncates a description at every length.
- **`example_cpukiller --window` could not start, every platform.** Its hand-written module
  list predates the frame loop's needing scene, text and the CLI; it now lists them, in the
  order kormake gives `example_testbed`'s. Its exit line counts the name table, which the
  window's path makes one block longer (no leak: nothing is live after `names_shutdown`).

## Milestone 4: parity

The full-frame pass, GPU timing and the gallery are in `docs/design/renderer-metal.md`. What
the milestone found around them:

- **Every module the Windows root builds now builds on macOS**, and `example_gallery` with
  them: 23 suites pass in debug, dev and ship. rocks, arcanum and arcfire (Shoreline and
  Orbit, with arcfire's three shaders and its post pass) run under Metal's validation with no
  message, at 120 fps.
- **Determinism on arm64.** Each demo at rate 4, at 1 thread and at 16, hashed alike tick for
  tick: rocks over 5,623 ticks, arcanum's whole demo (1,865), Shoreline's (14,265) and
  Orbit's (20,242). Shoreline is 14,265 ticks here and 14,156 on Windows: each platform keeps
  its libm, as between Linux and Windows.
- **A use-after-free in both renderers' deferred destruction.** An object queued for release
  between frames went on the slot of the frame about to be recorded, and was released at the
  wait that opens that frame, before it was recorded: arcanum's 2048x1024 font atlas is past
  half the frame ring, so its staging buffer is a one-off, and Metal crashed copying from it
  after it was freed. Vulkan's code is the same (its vkDestroyBuffer before the copy is
  undefined; a driver may well keep the memory long enough), and it has a second case Metal
  does not: a texture replaced between frames was destroyed while the other frame in flight
  could still sample it (Metal's command buffers retain what they use). Now a slot releases at
  its wait only what was queued up to its last frame's submit; what came after waits for the
  frame after. The Vulkan half is built and tested on Linux and Windows, not here.
- **The post parameters** first went as bytes, which Metal's validation refused against the
  shader's writable buffer (renderer-metal.md has why); they go in a buffer per frame.

## Milestone 6: ship

- **`kormake package` writes an `.app`** for a toolchain with `package = "app"` (macOS's).
  The package command puts the executable in `Contents/MacOS/` and the pack, `shaders/` and
  the licenses in `Contents/Resources/`; `package_app.lua` does the rest. It walks
  `otool -L` from the executable (anything outside `/System/`, `/usr/lib/` and `@`-relative
  names, and those libraries' own), copies each into `Contents/Frameworks/`, gives it the
  install name `@rpath/<name>`, rewrites the references and adds the rpath
  `@executable_path/../Frameworks`: Homebrew's `libSDL3.0.dylib` travels with the game. Then
  `Info.plist` (identifier `org.korvid.<exe>` with `_` as `-`, `LSMinimumSystemVersion`
  from the toolchain's new `os_min`, which its flags use too, high resolution) and a
  signature, libraries first, then the bundle, checked with `codesign --verify --strict`.
  Ad-hoc unless `KORMAKE_CODESIGN_IDENTITY` names an identity: arm64 code does not run
  unsigned, and `install_name_tool` breaks the linker's signature, so signing is last and
  not optional. Notarizing is not kormake's. Every command's output goes to
  `build/<target>/package.log`.
- **The base path inside a bundle.** `os_base_path` on macOS answers
  `<app>/Contents/Resources/` when the executable is in `<app>/Contents/MacOS/` and the
  bundle has the folder; the pack, the shaders and the assets are found where a bundle
  keeps them, and nothing above the platform knows. (An iOS app is flat.)
- **Checked:** rocks, arcanum and arcfire packaged from `macos-ship`, run from another
  directory: SDL3 is the bundle's (`lsof` on the running process), the pack mounts from
  `Resources/`, both shader libraries load, 120 fps, and Orbit's post pass draws.
- **I/O (R5)** stays on the sync backend: arcfire's boot reads 13 MB in one 24 ms pump,
  a quarter of its decoding. The numbers, and arcfire's performance on this machine, are in
  `arcfire-improvements.md`, "On macOS".
- **The fps line's overdraw** is left out when the backend has no fragment counts (Metal),
  instead of printing 0.
- **README**: "Building on macOS".

## Milestone 7: the Apple family

- **Platforms `ios` and `iossim`** in the master file, served by one toolchain module,
  `toolchain-apple` (was `toolchain-macos`), with a section per platform. kormake lays
  `per_platform[<platform>]` over the module's fields (lists appended, maps merged, the
  rest replaced) and names the result for the platform (`toolchain.apple.iossim`), so a
  build that holds a target and the host keeps them apart. A section holds what differs:
  the SDK (`macosx`, `iphoneos`, `iphonesimulator`), the `triple`, the platform's source
  directory, packaging, SDL3 (macOS only) and, for the Simulator, a `test_runner`.
- **`triple`** (`apple-macos26.0`, `apple-ios26.0`, `apple-ios26.0-simulator`): the xcode
  probe passes `-target arm64-<triple>` to compile and link in place of `-arch` and
  `-mmacosx-version-min`, and a tool rule may say `%{triple}`. The shader rule's Metal
  variant is keyed by the toolchain's `family` ("apple") rather than a platform, and asks
  Metal's compiler for `air64-<triple>`: a library is built for one OS, and before this the
  macOS ones were built for slangc's default, macOS 14. The gallery is identical to its
  references after the change.
- **iOS's floor is iOS 26**, with macOS's. The Metal floor (Apple7, A14) is a little higher
  than what iOS 26 runs on (A13); the renderer refuses such a device at start.
- **`src/apple/`** holds the family's shared code: the paths, moved from `src/macos/`
  (`_NSGetExecutablePath` and `HOME` serve both; on iOS `HOME` is the app's container, and
  a flat bundle's resources are beside the executable, so no `NSBundle`). The check that
  keeps AppKit out is the iOS build: an AppKit header does not exist in the iOS SDKs.
  `platform_hid`'s live-resize timer is AppKit's run-loop mode, so it is `TARGET_OS_OSX`
  only.
- **The `ios` root** is every module below the render interface (core, math, inflate,
  image, pack, xml, platform, cli, save, bulletml) with its tests. A module that uses
  "render" links the bound backend, renderer-metal, and through it the window: those
  join with milestone 8. `kormake gen` and `build` are clean for `iossim-dev` and
  `ios-dev` (`vtool` reports IOSSIMULATOR and IOS, minos 26.0), and `kormake test
  iossim-dev` runs all ten suites inside the booted Simulator (`xcrun simctl spawn
  booted`): 10,823 checks pass.
- **No SDL on iOS** (decided after the milestone): the iOS sections name only `src/ios/`.
  The window, the lifecycle, audio and gamepads there are native (UIKit, Core Audio,
  GameController), as the window is on Windows, instead of an SDL3 built from source.

## Milestone 8: native iOS

- **No SDL on iOS.** `platform_hid/src/ios/` is a UIKit backend, as `src/win32/` is a Win32
  one: one window scene, one `UIWindow`, its root controller's view a `UIView` whose layer
  class is `CAMetalLayer` (`native_handle()`); renderer-metal's `src/ios/` takes that layer.
  The SDL3 external is the desktop's only (`platforms = { "linux", "windows", "macos" }`).
- **The entry.** `platform_app_main(argc, argv, run, ctx)`, new in `host_window.h`, is a
  plain call on the desktop. On iOS it calls `UIApplicationMain`; the app delegate, once
  launched, runs `run` from a run-loop timer (not the main queue, which a nested loop would
  starve) and `exit`s with its result. `pump` drains UIKit's loop
  (`CFRunLoopRunInMode(..., 0, true)` until nothing is handled), so UIKit's events and the
  scene's lifecycle arrive between frames and `frameloop` is the same loop as everywhere:
  `frameloop_main` goes through `platform_app_main`, and so does `example_window`. The
  SDK (iOS 27.2) wants the scene lifecycle: the app delegate hands out a configuration
  whose delegate is ours, and the Info.plist declares a one-scene manifest.
- **Lifecycle.** The scene's activation is the window's focus (`FocusChanged`); on iOS a
  window without focus holds the sim (`HoldFocus`, through `hid_backend_holds_unfocused`,
  false on the desktop). In the background `pump` waits in UIKit's loop until the scene is
  back: no frame, no GPU work (iOS refuses it there). Seen in the Simulator with
  `example_window`: focus lost when another app opened, "in the background: waiting",
  back, focus gained, 4.5 ms frame gaps either side.
- **Keys** from a hardware keyboard: `pressesBegan`/`Ended` on the view controller, their
  HID usages through `input_ios.cc`'s table. Touch, gamepads (GameController) and text
  input are slice 8's.
- **Audio.** `platform_audio` splits its backend like platform_hid's (`audio_backend.h`):
  SDL3 in `src/sdl3_audio/` (a new source directory on Linux, Windows and macOS), the
  RemoteIO audio unit in `src/ios/`, an Ambient audio session. `audio_tests` pass in the
  Simulator.
- **Packaging.** `package = "iosapp"`: a flat bundle (executable, shaders, pack or loose
  assets, licenses at the top), an iOS Info.plist (scene manifest, launch screen, full
  screen, MinimumOSVersion), signed ad-hoc. A package without a pack now carries its
  modules' loose `assets/` (the testbed has no pack). `kormake package iossim-dev
  example_testbed`, then `xcrun simctl install booted dist/example_testbed/example_testbed.app`.
- **The tests.** The `ios` root is the whole engine but the games: 21 suites build for the
  Simulator and the device and pass inside the Simulator (`kormake test iossim-dev`).
- **The Simulator cannot run the renderer.** Its GPU is an Apple2 with tier 1 argument
  buffers and no indirect draws or dispatches; the GPU sort and the draws are both
  indirect. Let past the check, the testbed's frame loop ran (60 to 150 fps, the clear color
  presented), but the sprites drew black, and Metal's validation names the reason:
  "Indirect draw & dispatch arguments is not supported on this device". The check stands
  (now logging the family and tier it found). A device (A14 and later) is the renderer's
  floor; drawing on iOS waits for a device run (slice 8) or a fallback path for the
  Simulator. (Shader validation cannot run there at all: it binds buffers past slot 30.)

### Still to come

- One CPU side for both renderers (milestone 5, waiting on a decision).
- Drawing on iOS: a device, or a Simulator path without indirect commands.
- The gallery's blends and shaders scenes sample their checker at 7.5 pixels a texel, so
  every edge is at a pixel centre and the backends round it differently; the Vulkan set
  and its tolerance follow the fix.
