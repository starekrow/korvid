# Engine Slice 7 — macOS, Metal, and the Apple family

Nov 10, 2026 · @starekrow

## Goal and exit criteria

Linux and Windows run the whole engine. Slice 7 brings up macOS on Apple silicon with a native Metal renderer, and shapes everything it touches so that iOS is a bring-up, not a rewrite. The requirements doc planned macOS through KosmicKrisp and Metal only for iOS; iOS is now firm, so macOS goes native: one Metal backend serves both, and nothing translates Vulkan at run time.

The spine does not change. The sim, the module model, the render interface (`korvid/render/renderer.h`) and every game stay as they are. What arrives: a macOS toolchain and a build that knows the Apple family (kormake's first `.mm` sources and `-framework` links), the posix code made portable to macOS, `renderer-metal` behind the same interface, Slang's shaders compiled to Metal libraries from the one set of sources, a macOS packager, and the `ios` and `iossim` platforms in kormake with a simulator smoke test.

It is done when:

- The `macos` root builds every module `windows` builds, plus `example_sockit2m`, with their tests, in debug, dev and ship on Apple silicon. Every suite passes. `core`'s job and parallel suites and `example_cpukiller` run 100 times in a loop with no failure: this is the first weakly ordered CPU the lock-free code has met.
- `example_window`, `testbed`, `stones`, `rocks`, `arcanum`, `cpukiller` and `arcfire` run. `rocks`, `arcanum` and arcfire's Shoreline and Orbit demos hash alike at 1 and 16 threads, across runs. A replay recorded on Linux or Windows is expected to diverge, as Windows's does (each platform keeps its libm), and the notes say so.
- The Metal backend implements every function of `renderer.h`: the persistent sprite world with per-tick deltas, the five-kernel compute sort and its keyed sort, indirect draws, bindless textures, materials with game shaders, the full-frame post pass, `--screenshot`, `--check-sort`, GPU timing, labels in Xcode's debugger, resize, vsync, zero-size windows. Debug builds run with Metal's API and shader validation on and report nothing.
- Rendering matches Vulkan's. A render gallery scene (a fixed delta stream: every blend, layer, sort mode, filter, interpolation alpha, screen-space sprites, the post pass, each arcfire shader) screenshots on macOS and on Linux, and a compare tool puts the two within a tolerance fixed after the first comparison and never loosened afterwards.
- `kormake package macos-ship <exe>` writes `dist/<exe>/<exe>.app`: it runs from Finder with no source tree, reads its pack from the bundle, writes saves under `~/Library/Application Support/`, and carries its licenses.
- `kormake gen iossim-dev` and `kormake gen ios-dev` resolve; the code in `src/apple/` includes nothing macOS-only; `kormake build iossim-dev` builds `example_testbed` as an app that launches in the iOS Simulator and draws its balls through the Metal backend (P1: may slide to slice 8).
- Linux and Windows builds are unchanged in behaviour: the sim hashes, and the Vulkan screenshots of the gallery scene, match what they were before the slice began.
- Every changed module's README and header docs say what changed; `docs/design/renderer.md` gains a Metal section; the README gains "Building on macOS"; `docs/design/macos-port-notes.md` lists every place the engine assumed Linux or x64 and what was done, in the manner of `arcanum-notes.md`; arcfire's performance notes gain a macOS section.

## What the machine and the code already have

- **Host.** arm64 macOS 26.7; Xcode 26 installed, still downloading its iOS platform; `xcode-select` points at the Command Line Tools until `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer` is run. `slangc` 2026.22 in `~/tools/slang/bin`, on PATH only in interactive shells. SDL3 3.4.20 from Homebrew, the version `toolchain-windows` pins.
- **kormake** already bootstraps and runs on macOS (`os_posix.c` has an `__APPLE__` branch). It stops at "no toolchain module provides platform 'macos'".
- **Reusable as is.** `platform_hid` and `platform_audio` are SDL3, and SDL3 has Cocoa and UIKit backends; the `sdl3` and `sdl3_pads` source directories serve macOS. `platform`'s io_uring is guarded by `__linux__` and falls back to the sync reader.
- **Linux in the "posix" sources**, to be found and fixed (R2): `MAP_NORESERVE` and a `MADV_DONTNEED` that does not release memory on macOS in `core/src/posix/os_memory.cc`; `accept4`, `SOCK_NONBLOCK`, `SOCK_CLOEXEC` and `MSG_NOSIGNAL` in `platform/src/posix/net_posix.cc`; `/proc/self/exe` in `platform/src/linux/paths_linux.cc` and `example_sockit2m/src/torture.cc`. The allocator's page assumptions are checked against 16 KiB pages, which this machine has.
- **The renderer's shaders** are the hard part, and the reason milestone 1 is a spike: `sprite_sort.slang` uses `groupshared` memory with `InterlockedAdd` and group barriers; `sprite_fragment.slangh` samples `Texture2D gTextures[]`, an unbounded array indexed with `NonUniformResourceIndex`, beside a two-sampler array; the vertex shader reads `SV_StartInstanceLocation`; every stage uses `[[vk::binding]]` and a push-constant block; the compute passes write the indirect draw commands. Metal can express all of it (tier 2 argument buffers, `base_instance`, a draw-arguments struct with `VkDrawIndirectCommand`'s layout), but how well Slang's Metal target maps the unbounded texture array is not known until it is tried.

## Build and platform

**The Apple toolchain (R1).** `toolchain-macos` (module `toolchain.macos`, tag `macos`) in the manner of the Linux and Windows modules: clang from the active developer directory, found by a new `probe_xcode` the way `probe_msvc` finds `cl` (`xcrun` for the compiler, the SDK path and Ninja's location), so no particular shell is needed. Apple silicon only; deployment target macOS 26 (decided Nov 10: older releases can wait, and it matches Homebrew's SDL3 build). Flags follow Linux's (`-std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`) plus `-ffp-contract=off`: arm64 clang fuses multiply-adds by default and x86 builds of the same source do not, and a build that matches the other two as closely as possible makes every cross-platform difference in a hash or a screenshot a real one (decision for review). Source directories `posix`, `apple`, `macos`, `sdl3`, `sdl3_pads`, in that order. SDL3 is an external found at Homebrew's prefix or `KORMAKE_SDL3_DIR`, pinned to 3.4.20 and checked as Windows's is, with `libSDL3.0.dylib` as a runtime file copied beside executables. `slangc` is found through `PATH` and `KORMAKE_SLANGC`. kormake grows: `.mm` sources (Objective-C++ with ARC, in a unity file of their own, since a `.cc` and a `.mm` cannot share one), `-framework` and `-rpath` link flags, and a `macos` entry in `platforms`. The master file gains a `macos` root listing the modules as `windows` does.

**Portable posix (R2).** Each fix keeps Linux's behaviour. Reserve and decommit use `mmap` flags and calls that exist on both (`MAP_NORESERVE` only where defined; decommit by re-mapping `PROT_NONE` over the range, which releases the pages on both, or `MADV_FREE` where it is available); sockets set non-blocking and close-on-exec with `fcntl` after `accept` and set `SO_NOSIGPIPE` where `MSG_NOSIGNAL` is missing; the executable's directory comes from `_NSGetExecutablePath` in `src/macos/` (and from the bundle's `Resources` when the executable is in a `.app`, via `src/apple/`), the preference directory from `~/Library/Application Support/<org>/<app>/`, where SDL puts it; `torture.cc` takes the platform's `os_base_path` instead of reading `/proc`, which fixes it for Windows too. Page-size assumptions in the allocator and arenas are checked against 16 KiB and the test suites run under it.

**Weak memory (R3).** `atomic.h` already takes explicit orders on every access, and Linux and Windows have only ever run on x64, where ordinary loads and stores are ordered by the hardware. Every lock-free structure (the job pool's queues, the allocator's remote frees, the event buffers, the I/O queue's completion list) is audited against its comment's claimed orders; any access the x64 build got away with is fixed, with a test that fails on arm64 without the fix where one can be written. The stress loop in the exit criteria is the proof.

**The window and input on Cocoa (R4).** `WindowSurface` gains `Metal`; the SDL3 backend creates the window with `SDL_WINDOW_METAL`. `pixel_scale` and `width/height` report Retina's backing pixels, `renderer_resize` follows the drawable. Cmd+Q and the red button post the same close request Escape does in the examples; full screen is the system's; the keymap test gains the Mac keys (Command, Option, the function row). The gamepad path is SDL's, unchanged. Text input uses SDL3's on-screen and IME events, unchanged.

**I/O (R5).** macOS runs on the sync backend, as the requirements say Apple dispatch I/O is the eventual mapping. Whether the sync backend's cost shows in arcfire's load or the pack's reads is measured in the notes; a `dispatch_io` backend is P2 and waits for a measurement that asks for it.

## The Metal renderer

**One backend for the Apple family (R6).** `renderer-metal` (`provides = "render"`, `platforms = { "macos", "ios", "iossim" }`) implements `renderer.h` with no change to the interface, in Objective-C++ with ARC, and the only module that includes a Metal header. It uses no API that exists on macOS only: shared storage everywhere (Apple silicon and iOS GPUs have unified memory), no managed or discrete-GPU paths, `MTLGPUFamilyApple7` and tier 2 argument buffers as the floor (M1 and A14, iPhone 12). The surface is a `CAMetalLayer` that SDL3 supplies, through a small `src/sdl3/` file in the manner of `vk_platform_sdl3.cc`.

- **Tables.** Sprite rows, material rows, sort scratch and the indirect commands are `MTLBuffer`s in shared storage with the same layouts (`GpuSprite` 112 bytes, `GpuMaterial` 64, `PushData` 112, as `renderer_internal.h` pins them and `sprite_common.slangh` mirrors). Dirty rows copy into the frame ring as now, with a blit encoder where Vulkan used `vkCmdCopyBuffer`; on shared memory the ring may become a direct write when measurement says so.
- **Frames.** Two frames in flight and a semaphore, as Vulkan's fences do; deferred destruction on the frame that last used a resource; the frame ring's contiguity rules carry over.
- **Compute and draws.** The five compute kernels dispatch as written. The indirect draws use `MTLDrawPrimitivesIndirectArguments`, whose fields are `VkDrawIndirectCommand`'s in the same order, with one `drawPrimitives(indirectBuffer:)` per run of buckets (Metal has no multi-draw, so the existing fallback is the only path).
- **Textures.** A texture is an `MTLTexture`; slot descriptors live in an argument buffer whose entries are written when a slot's upload completes, so a new slot shows the default texture until then, as the interface promises. `kMaxTextures` is a compile-time size (1024 today), which Metal's fixed-size argument arrays need and Vulkan's variable-count binding did not.
- **Output.** `BGRA8Unorm_sRGB` layer pixel format (`srgb_output` true), `displaySyncEnabled` for vsync, three drawables, `presentDrawable`; ProMotion's 120 Hz arrives through the same `renderer_last_wait` slack the frame loop already reads.
- **Dev facilities.** `--screenshot` and `--check-sort` as now. GPU timing uses counter sample buffers at stage boundaries (Apple GPUs sample there, not mid-pass), which maps to the five spans the profiler's `gpu` track expects, with invocation counts where the device counts them and zeros where it does not. Every pass, buffer and texture carries a debug group or a label, so an Xcode GPU capture reads like the code. Debug builds turn on Metal API validation and shader validation, in process before the device is created where the OS honours that and through the launcher's environment where it does not.

**Shaders (R7).** There is one set of `.slang` sources. The `slang` module's rule takes its target from the toolchain (`spirv` on Linux and Windows, Metal on the Apple family) instead of hard-coding it, and writes `bin/shaders/<stem>.metallib` beside where `.spv` goes today, so a game's shaders load by name as they do now. Slang emits MSL; the Metal compiler (`xcrun metal` and `metallib`, from Xcode's Metal toolchain, which Xcode 26 downloads as a separate component) turns it into a library. Shader changes the port forces are made in the shared sources and tested on Vulkan too. The unbounded texture array and `NonUniformResourceIndex` are the expected trouble: if Slang's Metal target cannot express them, the fallback is a fixed-size array in a `ParameterBlock` (a sized argument-buffer array, `kMaxTextures` long), chosen by one `#if`, never by a second copy of the shader. Milestone 1 settles this before any renderer code is written.

**The shared CPU side (R8).** Much of `renderer.cc` and `vk_resources.cc` never touches Vulkan: the render world's mirror of the sprite pool, dirty bitmaps, per-bucket counts, material and texture records and slot assignment, key computation for keyed sorts, the garbage queue's shape. Written twice they will drift. The order is deliberate: the Metal backend is written first against a copy of what it needs, and once both backends run, the extraction into a shared header-and-source module (`render_world`) is made from what the two diffs show is shared, not guessed before the second backend exists. Vulkan's behaviour does not change and the gallery's Vulkan screenshots prove it.

**Parity (R9).** `example_gallery` (new, small) draws a fixed delta stream through every renderer feature at fixed interpolation alphas and takes `--screenshot`s; `tools/` gains a PPM compare that reports per-channel maximum, the fraction of pixels past a threshold and the first differing pixel. The first Linux and macOS comparison sets the tolerance; differences in linear filtering and blend rounding between GPUs are expected and counted, structural differences (an ordering, a missing layer, a wrong blend) are not. The references are committed, so a Windows or Linux regression shows too.

## Shipping and the Apple family

**Packaging (R10).** `kormake package macos-ship <exe>` writes `dist/<exe>/<exe>.app`: `Contents/Info.plist` (bundle id, version, minimum system, high-resolution capable), the executable in `MacOS/`, the pack, shaders and licenses in `Resources/`, `libSDL3.dylib` in `Frameworks/` with its install name and the executable's rpath rewritten, and an ad-hoc signature (arm64 binaries do not run unsigned). Developer ID signing and notarization need an account and are out of scope; the packager takes a signing identity from the environment so that they are not blocked on code.

**The tests and examples (R11).** The `macos` root, the suites, the examples and the exit criteria's loops, in debug, dev and ship; `example_sockit2m` joins through R2's fix; arcfire's performance notes gain the macOS measurements (the tick, the extract per delta, GPU time against Linux and Windows) as they have for Windows.

**The Apple family in kormake (R12).** Platforms `macos`, `ios` and `iossim` (no second dash in a name: a target is `<platform>-<config>`). A toolchain module may carry per-platform sections, because the three differ in `-target`, `-isysroot` and the SDK `probe_xcode` finds (`macosx`, `iphoneos`, `iphonesimulator`) and not in much else. Code shared by the three lives in `src/apple/` (bundle paths through `NSBundle`, the preference directory through `NSSearchPathForDirectoriesForDomains`, the memory calls); code that is the desktop's alone in `src/macos/`; and a check in the build or the tests keeps AppKit out of `src/apple/`. `kormake gen iossim-dev` resolves from milestone 6 on, so a module that breaks the iOS graph is found when it is written.

**The iOS simulator smoke (R13).** `example_testbed` builds for `iossim` as an app bundle and launches in the iOS Simulator: the assets read from the bundle, the SDL3 UIKit entry (the executable's `main` includes `SDL_main.h`; the frame loop runs inside SDL's iOS app model, which is verified rather than assumed), the Metal backend drawing the balls, the app's background and foreground events mapped onto the engine's existing `Focus` hold so the sim holds while the app is not on screen. SDL3 for iOS is built from the pinned release source by a script under `tools/` into `vendor/` (Homebrew has no iOS build). No touch, no device, no provisioning, no App Store: those are slice 8, and this slice only proves the road to them is open.

## Out of scope

Intel and universal binaries; Developer ID signing, notarization and the App Store; iOS on a device, touch input, the on-screen keyboard's layout handling, and iOS audio session policy; MoltenVK and KosmicKrisp (decided against, above); a `dispatch_io` backend (P2); Metal ray tracing, mesh shaders and MetalFX; HDR and EDR output; tvOS, visionOS and Mac Catalyst; Xcode project generation (kormake drives `clang`, and Xcode opens the GPU captures, not the build).

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `toolchain-macos`, `probe_xcode`, `.mm` sources, framework and rpath flags, pinned SDL3, the `macos` root | P0 |
| R2 | Portable posix: memory, sockets, paths, `torture.cc`; 16 KiB pages | P0 |
| R3 | Weak-memory audit of the lock-free code; the arm64 stress loop | P0 |
| R4 | Cocoa window: Metal surface, Retina scale, Mac keys, quit behaviour | P0 |
| R5 | I/O on the sync backend, measured; `dispatch_io` deferred | P1 |
| R6 | `renderer-metal` implementing `renderer.h`: world, compute sort, indirect draws, bindless textures, materials, post pass, dev facilities, validation | P0 |
| R7 | One set of Slang sources compiled to SPIR-V or Metal libraries by target | P0 |
| R8 | The shared CPU side extracted after both backends run | P1 |
| R9 | The render gallery, the compare tool and committed references | P0 |
| R10 | `kormake package macos-ship`: the `.app`, ad-hoc signature | P0 |
| R11 | Every suite and example on macOS; the performance notes | P0 |
| R12 | `ios` and `iossim` platforms, per-platform toolchain sections, `src/apple/` | P0 |
| R13 | The iOS Simulator smoke on `example_testbed` | P1 |

## Milestones

Each leaves Linux and Windows building and the engine running on macOS as far as it has got.

1. **Ground** (R1–R3): the toolchain, `probe_xcode`, the posix fixes and the weak-memory audit. Every module with no window or renderer builds on macOS and its suite passes in debug and dev; `cpukiller` runs its tables; the stress loop is clean.
2. **Window and shader spike** (R4, R7): `platform_hid` on Cocoa, `example_window` opens, shows its events and the frame loop's gaps. Every `.slang` file in the engine and in arcfire compiles to MSL and to a Metal library, and a one-page result in `docs/design/renderer-metal.md` records how the texture array, the base instance and the push constants mapped, with the fallbacks for what did not. This is the go or no-go for the shape of R6.
3. **Clear and sprites** (R6, partly): `renderer-metal` makes its device, queue, layer and frames; tables, textures, materials, the compute sort and the indirect draws; `testbed` bounces its balls in all three blends; `--check-sort` passes; resize, vsync and zero-size windows work.
4. **Parity** (R6, R7, R9): the post pass, game shaders, `--screenshot`, GPU timing, labels, validation; the gallery and its compare tool; `rocks`, `arcanum` and arcfire run and hash alike at 1 and 16 threads.
5. **One CPU side** (R8): the shared module extracted; Vulkan on Linux and Windows re-verified against the committed references and the sim hashes.
6. **Ship** (R10, R11, R5's measurement): the full `macos` root and its suites in all three configs, `sockit2m` included, the packager, "Building on macOS", the notes, the performance section.
7. **The Apple family** (R12): `ios` and `iossim` resolve; per-platform toolchain sections; `src/apple/` split and its check; `kormake gen` clean for both.
8. **The simulator** (R13): SDL3 for iOS built by script, `testbed` built, bundled, launched in the Simulator and drawing; lifecycle onto the hold. P1: if the entry-point or lifecycle work is bigger than a milestone, the slice closes at 7 and this is slice 8's first.

## For review

Decisions the milestones lean on, in the order a wrong answer costs most:

1. **Native Metal for macOS, no Vulkan layer** (decided in conversation). The cost is R6, which is a second backend; the saving is no loader, no translation layer to ship and one GPU API per Apple platform.
2. **Apple silicon only; macOS 26 as the floor** (decided: earlier releases can wait). Intel Macs would need a second GPU path (discrete and managed storage); the floor sets which Metal features may be assumed. iOS's floor is set at milestone 7.
3. **Objective-C++ (`.mm`, ARC) for the backend and the bundle paths**, not metal-cpp: Cocoa paths need Objective-C anyway, and one language for the Apple code is simpler than two. Cost: kormake learns `.mm`.
4. **`-ffp-contract=off` on macOS**, so that arm64 and x64 builds agree as closely as the code allows.
5. **The shared CPU side extracted after the second backend runs**, accepting some duplication for four milestones.
6. **The simulator smoke is P1 in this slice**, because the road to iOS is cheaper to find now than after the macOS port has set; it may move.
7. **Prerequisites on this machine:** `sudo xcode-select -s` to Xcode, the license accepted, the iOS platform finished downloading, the Metal toolchain component present (`xcrun -find metal` works), `slangc` reachable by kormake in a non-interactive shell.
