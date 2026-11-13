# Engine Slice 8 — Android

Nov 13, 2026 · @starekrow

## Goal and exit criteria

Linux, Windows, macOS and iOS run the whole engine. Slice 8 brings up Android: the Vulkan backend Linux and Windows already draw with, a native window, input and audio in the manner of iOS and Windows (no SDL, no Gradle, no Android Studio), an APK that kormake writes itself, and the test suites running inside the emulator as they run inside the iOS Simulator.

There is no Android device on hand. Everything in this slice is proved in the emulator on this Linux machine, which is an x86-64 guest whose Vulkan is the host's GPU passed through. So the slice has two platforms, as iOS has: `androidemu` (x86-64, what runs here) and `android` (arm64, what a phone runs). The arm64 build is built, linked and packaged in every milestone and never run; the PRD says at each place what that leaves unproved, and a device run is slice 9's first act, as it was for iOS after slice 7.

The spine does not change. The sim, the module model, the render interface and every game stay as they are. What arrives: `toolchain-android` and its probe, the posix sources checked against bionic, `platform_hid`, `platform_audio` and `renderer-vulkan` each gaining a `src/android/`, the I/O queue reading out of the APK, an `apk` packager, and an emulator test runner.

It is done when:

- `kormake gen`, `build` and `test` are clean for `androidemu-debug`, `-dev` and `-ship`; the `android` root is every module the `ios` root holds (with `renderer-vulkan` for `renderer-metal`), and every suite passes inside the running emulator. `kormake build android-dev` (arm64) builds and links the same set.
- `kormake package androidemu-dev <game>` writes `dist/<game>/<game>.apk`, signed with a debug key, which `adb install` takes. `window`, `testbed`, `stones`, `rocks`, `arcanum`, `arcfire` and `gallery` launch from the emulator's launcher and from `adb shell am start`, draw, play sound, and take touches through the touchpad module.
- `rocks`, `arcanum` and arcfire's Shoreline and Orbit demos hash alike at 1 and 8 threads in the emulator, **and match the hashes Linux records** (kv's math is in since slice 7 closed; this is the first new platform that must agree with the others bit for bit from its first run). arcfire's recorded replays play to their end.
- The gallery's screenshots, taken in the emulator at the reference size, are within the committed tolerance of the Vulkan references. (The emulator draws with this machine's own GPU, so this proves the Android surface, swapchain and shader path, not a phone's GPU.)
- Lifecycle: Home and back again, the recents screen, a rotation and the screen turning off each leave the game running where it was, the sim held (`HoldFocus`) while it was away, no frame drawn and no GPU work without a surface. Fifty rounds of `am start` / Home in a loop end with the game drawing and no validation message.
- Debug packages carry the Khronos validation layer and run with it on; it reports nothing.
- Linux, Windows, macOS and iOS are unchanged in behaviour: hashes and gallery references as they were.
- `README.md` gains "Android" (what to install, the four commands from source to a running game); `docs/design/android-port-notes.md` lists what the port found, in the manner of the macOS notes; the changed modules' READMEs and header docs say what changed.

## What the machine and the code already have

- **Host.** Ubuntu 26.04, 16 threads, 59 GB, Radeon 780M (RADV, Vulkan 1.4). `/dev/kvm` is present and writable by this user, so the emulator runs accelerated. OpenJDK 25 is installed as a runtime only (no `javac`). The SDK of "Tooling" below is installed under `~/Android/Sdk` (Nov 13: command-line tools, platform tools, build tools 36.0.0, platform 36, NDK r30, the emulator, the Android 16 `google_apis` x86-64 image) with one AVD, `korvid`, a Pixel 9; the validation layer and the JDK are still to get.
- **The renderer** is Vulkan 1.3 and wants `drawIndirectFirstInstance`, descriptor indexing with a runtime array, partially bound, update-after-bind, variable-count sampled images (1024 textures), compute, and what 1.3 makes core. `multiDrawIndirect`, timestamps and pipeline statistics are already optional. The swapchain already takes `R8G8B8A8_SRGB` (Android's usual), and already passes the surface's `currentTransform` as its `preTransform`, which on Android means it promises to draw pre-rotated and does not: R6.
- **What per-platform seams exist.** `vk_platform.h` (the loader, the surface extension, the surface) has an SDL and a Win32 implementation. `hid_backend.h`, `platform_app_main`, `hid_backend_holds_unfocused` and the pump that waits while the app is in the background were all made for iOS and fit Android's lifecycle as they are. `audio_backend.h` has SDL and RemoteIO. `io_ring.h` has io_uring, Windows rings and the dispatch ring. `os_paths.h` has one source per OS. The toolchain's `per_platform` sections, `triple`, `family`, `test_runner` and `test_jobs` came with the Apple family.
- **Touch** reaches `Input` as `TouchPoint`s with safe-area insets, and `touchpad` turns them into a pad every game already binds. Nothing in it is iOS's.
- **16 KiB pages** were met on macOS; Android 15 and later may run them, and NDK r28 and later link for them by default.
- **The pack** is a zip with stored entries aligned to 4 KiB, "as Android's `zipalign` does" (slice 5): an APK is a zip too, and that is R5's whole idea.
- **Shaders** are read by the renderer from `<base path>/shaders/` with a direct file read, not through the mounts. An Android app has no directory beside its executable: R5.
- **Output.** `print` and the log write to stdout and stderr, which an Android app does not have: R3.

## Tooling

What to install, once. No Android Studio, no Gradle, no CMake, no SDL: kormake drives the NDK's clang and the SDK's packaging tools directly, as it drives Xcode's clang without an Xcode project.

| What | Package | Why |
| --- | --- | --- |
| Command-line tools | `commandlinetools-linux-15859902_latest.zip` from developer.android.com/studio (bottom of the page) | `sdkmanager` and `avdmanager`, which fetch the rest |
| Platform tools | `platform-tools` | `adb`: install, launch, logcat, the test runner |
| Build tools | `build-tools;36.0.0` (or the newest `sdkmanager --list` shows) | `aapt2`, `zipalign`, `apksigner`: the whole APK pipeline; `d8` for R9 |
| Platform | `platforms;android-36` | `android.jar`, which `aapt2 link` checks the manifest against |
| NDK | `ndk;30.0.16248370` (r30, the current LTS) | clang, bionic's headers and libraries for both ABIs, `lldb`, `simpleperf` |
| Emulator | `emulator` | the device |
| System image | `system-images;android-36;google_apis;x86_64` | Android 16 for the emulator; `google_apis`, not `google_apis_playstore`, so `adb root` works |
| Validation layer | `android-binaries-<version>.zip` from the KhronosGroup/Vulkan-ValidationLayers releases | Android has no system layers; a debug package carries its own |
| JDK (R9 only) | `sudo apt install openjdk-25-jdk-headless` | `javac`, for the one Java class of R9. Everything before R9 runs on the JRE already here |

About 3 GB to download and 12 to 15 GB on disk, most of it the NDK, the system image and the emulator's data partition. Laid out under `~/Android/Sdk` with `ANDROID_HOME` pointing there; `probe_android` finds the NDK at `$ANDROID_HOME/ndk/<the pinned version>` or `ANDROID_NDK_HOME`, the build tools at the pinned version or the newest present, and the layer at `KORMAKE_ANDROID_VK_LAYERS`, and says which of these is missing and how to get it, as the SDL3 external does on macOS.

The emulator is one AVD, made once (`avdmanager create avd -n korvid -k "system-images;android-36;google_apis;x86_64" -d pixel_9`) and started with `emulator -avd korvid -gpu host`.

For later, not this slice: a device (any phone from 2022 on with a Vulkan 1.3 driver; one Adreno and one Mali would be the pair to own), Android GPU Inspector for GPU profiling on it, and `bundletool` for the Play Store's `.aab`.

## Build and platform

**The toolchain (R1).** `toolchain-android` (module `toolchain.android`, `family = "android"`, `probe = "android"`) in the manner of `toolchain-apple`: the NDK's clang, one section per platform.

- `android`: `triple = "linux-android33"`, arm64 (`aarch64-linux-android33`), ABI directory `arm64-v8a`.
- `androidemu`: the same triple on x86-64 (`x86_64-linux-android33`), ABI directory `x86_64`, with a `test_runner`.

kormake's platform names have carried the architecture silently until now (every Apple target is arm64, every Linux and Windows one x64). These two differ only in it, so the section gains an `arch` field and `probe_android` composes `--target=<arch>-<triple>`; `probe_xcode`'s hard-coded `arm64-` reads the same field. Flags are Linux's (`-ffp-contract=off -fno-math-errno` matter more here than anywhere: arm64 and x64 builds of one platform), plus `-fPIC` and, for links, `-static-libstdc++` or no C++ runtime at all if nothing asks for one (the engine uses no STL; M1 finds out). Source directories `posix`, `linux` where the file is truly Linux's and not glibc's (R2), then `android`. The floor is API 33, Android 13 (for review): it is the first release whose new devices mostly carry Vulkan 1.3 drivers, it has AAudio and everything else used here, and the renderer refuses a device below 1.3 at start whatever the OS.

**An executable is a shared library (R1).** Android starts an app by loading `lib<name>.so` into a process it made. A new toolchain field, `exe_link = "shared"` with `exe_file = "lib%s.so"`, makes an `output = "exe"` module that links the window (anything that reaches `platform_hid`) a shared library on these platforms, its `main` kept and exported. Tests and headless tools stay ordinary executables (PIE, bionic), which is what lets them run from `adb shell` (R12).

**Bionic (R2).** The posix sources were written for glibc and then made portable to macOS; bionic is a third libc. Expected, to be found by building: no `pthread_cancel`, affinity through `sched_setaffinity`, `sysconf(_SC_NPROCESSORS_ONLN)` reporting only the cores awake (the job pool's default thread count should use the configured count, and what a big.LITTLE phone wants is a device question, noted not solved); `paths_linux.cc`'s `/proc/self/exe` is true for a test and meaningless for an app (R5); io_uring is refused to apps by Android's seccomp policy, so `io_uring.cc`'s create must fail cleanly there and not be the default (R4).

**The entry, the log and the arguments (R3).** `NativeActivity`, with `android:hasCode="false"`: no Java in the package until R9. `platform_hid/src/android/` implements `ANativeActivity_onCreate` itself, small and ours, not the NDK's `native_app_glue` (it is 400 lines of someone else's threading under our lifecycle; for review): the callbacks arrive on Android's main thread and post to a queue, and a thread of our own, made in `onCreate`, calls the game's `main(argc, argv)`, which goes through `frameloop_main` to `platform_app_main` as on every platform. `platform_app_main` there is a plain call; `pump` drains the queue and the input queue through an `ALooper`. So `frameloop` is unchanged, as on iOS.

- **Arguments** come from the launching intent's string extra `args` (`adb shell am start -n net.korvid.rocks/android.app.NativeActivity --es args "--demo --hash"`), read once through JNI in `onCreate` and split as a shell would split them. A launch from the launcher has none.
- **Output.** On Android, core's `print` and the log go to logcat (`__android_log_write`, tag `korvid`, one call a line) when the process is an app, and to stdout when it is not (a test under `adb shell`): decided by whether `ANativeActivity_onCreate` ran. `--hash` lines, `--overlay`, the profiler's summaries all arrive in `adb logcat -s korvid`.
- **The working directory** is the app's external files directory (`/sdcard/Android/data/<package>/files`), as an iOS app starts in its `tmp/`: `--screenshot` and `--profile` land where `adb pull` reaches them without root.

**Async I/O (R4).** No kernel ring for apps. `--io sync` is the first backend and the suites run on it. Then `platform/src/android/io_ring_android.cc`: the Apple dispatch ring's shape with a small pool of our own threads in place of GCD's (blocking `pread`/`pwrite` into the queue's buffer, a completion list, a semaphore). If that file turns out to be `io_ring_apple.cc` with the dispatch calls swapped, the two become one `src/posix/io_ring_threads.cc` and Apple keeps GCD only as its thread source. P1: measured with `sockit2m --compare` in the emulator for what an emulator's numbers are worth, which is the shape and not the size.

**Files inside the APK (R5).** An app's assets are entries in its APK, and the APK is a zip the engine can already read.

- The packager stores, uncompressed, the game's pack as `assets/<exe>.kpak`, or with no pack its modules' loose assets as `assets/<module>/...`, and the shaders as `assets/shaders/<name>.spv`. `zipalign -P 16` leaves the pack on a page boundary, so the 4 KiB alignment inside it survives.
- `IoQueue::mount_pack` gains two forms, both general: a pack at an offset inside a file (`mount_pack_at(path, offset, size)`: the game's pack inside the APK, read with the same ranged reads, no copy and no `AAssetManager`), and a prefix on a foreign zip (`mount_pack(path, "assets/")`: the APK itself, for loose assets). `platform`'s startup on Android does the one that applies; the APK's own path comes from the activity (`getPackageResourcePath`, one JNI call, kept by `src/android/paths_android.cc`).
- `platform_base_path` has no meaning in an app. The renderer's shader read stops building a path from it: `renderer-vulkan` (and metal, and d3d12, which do the same) ask `platform` for a bundled file by name (`platform_read_bundled("shaders/sprite.spv", ...)`), which is today's direct read everywhere else and a stored APK entry here.
- `os_pref_path` is the activity's internal data path (`/data/data/<package>/files/<org>/<app>/`): saves and high scores.

Tests are not apps: they run from a directory with `assets/` and `shaders/` beside them (R12), on the Linux code paths.

## The window, input and sound

**The window and the lifecycle (R6, with the renderer).** `platform_hid/src/android/host_window_android.cc`: the window is the activity's `ANativeWindow` (`native_handle()`), its size the surface's, `pixel_scale` from the display's density, the safe-area insets from the window's insets (one JNI read, again on rotation) so the touchpad stays clear of cutouts and the gesture bar. Immersive full screen. Orientation follows the shape the game asked for, as iOS's does: wider than tall is landscape either way up (`setRequestedOrientation` through JNI), and `open` waits for the turn.

Android takes the surface away. `onPause` and loss of window focus are `FocusChanged` (and `hid_backend_holds_unfocused` is true, so the sim holds); `onNativeWindowDestroyed` is new to the engine: the window's handle becomes null, and it gets a new one on return. `HostWindow` gains a surface generation the renderer watches: on a change, `renderer-vulkan` waits for the device to idle, destroys the swapchain and the `VkSurfaceKHR`, and makes both again from the new handle when there is one. While there is none, `pump` blocks in the looper, as iOS's does in UIKit's loop: no frame, no GPU work. The main-thread callback for a destroyed window must not return until the render thread has let go of it; the handoff is one condition variable and is the part of this slice most likely to deadlock, so the lifecycle loop in the exit criteria exists from the day it is written.

A process in the background may be killed without a word. Nothing new is promised: a game that saves, saves when it does today. Saving on `onPause` is a game's choice and arcanum may take it (P2).

**Vulkan on Android (R7).** `renderer-vulkan/src/android/vk_platform_android.cc`: `dlopen("libvulkan.so")`, `VK_KHR_android_surface`, a surface from the `ANativeWindow`. `module.lua` adds the two platforms. Beyond that:

- **Rotation.** With `preTransform = currentTransform` the app must rotate its own output, or it draws sideways the moment the surface's natural orientation is not the window's. P0: pass `IDENTITY` when the surface supports it (the compositor rotates; correct, and a cost on a real device). P1: true pre-rotation, a 90-degree turn folded into the sprite pass's projection and the post pass's full-screen triangle, the swapchain made at the rotated extent. The emulator rotates, so both are testable here.
- **Extent.** `currentExtent` is the surface's; a resize is a new surface generation or `VK_SUBOPTIMAL_KHR`, both of which already rebuild.
- **Composite alpha.** `OPAQUE` is asked for today; Android surfaces often offer only `INHERIT`. Take what `supportedCompositeAlpha` has.
- **Validation.** No system layers: `kormake package` puts `libVkLayer_khronos_validation.so` for the ABI in a debug package's `lib/<abi>/`, where the loader finds it. Ship and dev packages do not carry it.
- **What a phone will say and the emulator will not.** The emulator answers with RADV's features. A Mali or an Adreno may not offer `descriptorBindingVariableDescriptorCount`, 1024 update-after-bind sampled images, or `drawIndirectFirstInstance`, and a tile-based GPU prices the compute sort and the post pass differently. This slice does not guess: the device check logs exactly which requirement a GPU failed (it returns -1 silently today), so the first run on a phone names its problem, and fallbacks wait for a phone to need them.

**Input (R8).** `input_android.cc`, from the activity's `AInputQueue`:

- Touch: `AMotionEvent` pointers to `TouchPoint`s in framebuffer pixels, an ended one kept one pump, exactly what the iOS view feeds. The touchpad module turns on where the window's screen is touched, so every game plays. The emulator's mouse is a finger; `adb shell input` scripts taps and swipes for a test.
- Keys: `AKeyEvent` key codes through a table to the engine's keys (the keymap test gains the Android table). The host's keyboard reaches the emulator as a hardware keyboard. Back is Escape, which every example already treats as "close or go up a menu".
- Gamepads (P1): `AINPUT_SOURCE_GAMEPAD`/`JOYSTICK` events are a pad's buttons and axes with no library; one pad per device id. Built against the documentation and unverified until a pad is paired with a real device, since the emulator passes none through.

**Text input (R9, P1).** `NativeActivity` can raise the soft keyboard but gets raw key events, not composed text: fine for a name in ASCII, wrong for everything `Input::text` promises (layouts, dead keys, input methods). The road, built here only if the earlier milestones leave room: one Java class, `KorvidActivity extends NativeActivity`, that owns an `InputConnection` and hands committed text and backspaces to native through one JNI method; kormake compiles it with `javac --release 17` and `d8` into the package's `classes.dex` (a per-package step, still no Gradle), and the manifest drops `hasCode="false"`. That class is also where haptics and anything else Java-only would go later. Until it exists `platform_text_input` raises the keyboard and delivers ASCII from key events, and the notes say so.

**Audio (R10).** `platform_audio/src/android/audio_aaudio.cc`: an AAudio output stream, float, the mixer's rate or the device's with the stream's own conversion, low-latency performance mode, a data callback on AAudio's realtime thread calling the same pull the SDL and RemoteIO backends call. The stream is stopped on pause and started on resume, and remade on `AAUDIO_ERROR_DISCONNECTED` (a headset pulled). `audio_tests` pass in the emulator; rocks' callbacks are counted for drops as they were on the iPhone. The emulator's latency says nothing about a phone's.

## Shipping and testing

**The packager (R11).** `package = "apk"` (`tools/kormake/scripts/package_apk.lua`):

1. A generated `AndroidManifest.xml`: package `<org>.<exe>` from the module's fields, `minSdkVersion` 33, `targetSdkVersion` 36, `NativeActivity` with `android.app.lib_name`, `hasCode="false"`, `configChanges` covering orientation, size and keyboard so a rotation does not restart the process, the Vulkan 1.3 `uses-feature` (level and version, required), `INTERNET` only for a module that asks (sockit2m). A label and, where the module has one, an icon.
2. `aapt2 link` against `android.jar` makes the base APK from the manifest.
3. The library goes in as `lib/<abi>/lib<exe>.so` (stripped in ship; the unstripped one kept in `dist/` for the debugger and for symbolicating a crash), the validation layer in debug, then `assets/` as R5 lays it out, the licenses among them, all stored.
4. `zipalign -P 16 4`, then `apksigner` with a debug keystore kormake makes once (`keytool`, under `~/.android/`, the standard debug key). A release key comes from the environment, as the Apple signing identity does.

One ABI a package: `androidemu` writes an x86-64 APK and `android` an arm64 one. A package holding both, the `.aab` and Play's requirements are out of scope.

**The tests (R12).** `tools/android/emu_test.sh` is `androidemu`'s `test_runner`: it pushes the test executable with its `assets/` and `shaders/` to a directory of its own under `/data/local/tmp/korvid/`, runs it with `adb shell` there, and passes only on exit code 0 and the suite's own "checks passed" line together (devicectl taught that). `test_jobs` is whatever the emulator stands (start at 4, each suite in its own directory). It needs one emulator running and says so if `adb devices` shows none. Tests that open a window or the audio device are already headless or skip; what the iPhone found (suites writing beside the executable or in the working directory) was fixed then. The `android` platform's runner is the same script against a connected device, there for the day there is one.

**The emulator's Vulkan, first (R13).** Before any renderer work: `adb shell cmd gpu vkjson` in the running AVD, read against the list in "What the machine and the code already have". If the guest shows 1.3 and the descriptor-indexing features, the slice is as written. If it does not (an old gfxstream, a feature the passthrough drops), the choices in order are a newer emulator or system image, `-gpu swiftshader_indirect` (software, 1.3, slow but honest), or cutting the slice's drawing criteria down to "builds, tests pass, window opens" as the iOS Simulator forced. This is the go or no-go and it costs ten minutes once the tooling is in.

**Read on Nov 13: go.** The guest's device is this machine's own (`AMD Radeon 780M Graphics (RADV PHOENIX)`) at Vulkan 1.3.0, Android 16. Every requirement is there: `dynamicRendering`, `synchronization2`, `shaderDrawParameters`, `drawIndirectFirstInstance`, descriptor indexing with the runtime array, non-uniform indexing, update-after-bind, partially bound and variable count, 8,388,606 update-after-bind sampled images against the 1024 asked, `VK_KHR_swapchain` and `VK_KHR_android_surface`. The optional three (`multiDrawIndirect`, pipeline statistics, timestamps at 64 valid bits) are there too; calibrated timestamps are not, so the GPU's spans will not sit on the CPU's clock. Two things to carry: the guest runs 4 KiB pages (the 16 KiB case is macOS's to keep proving, or a `google_apis_ps16k` image's), and it has no Vulkan layers of its own, as expected.

## Out of scope

A device, and with it performance on a mobile GPU, thermal and battery behaviour, big.LITTLE scheduling, real audio latency, gamepads verified; fallbacks for GPUs below the renderer's requirements; the Play Store (`.aab`, `bundletool`, release signing, target-API policy, Play Asset Delivery, 64-bit and 16 KiB policy checks beyond what the NDK already gives); `GameActivity` and the AGDK (Swappy frame pacing, GameTextInput, Paddleboat, Oboe: each is revisited when a device shows the need it answers); 32-bit ABIs; multi-window, foldables, Chromebooks and Android TV; a second ABI in one package; SDL on Android; Android Studio and Gradle projects; the arm64 translation the x86 images carry (it may run the `android` build's tests in the emulator one day, and is not relied on).

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `toolchain-android`, `probe_android`, `arch`, the two platforms, executables as shared libraries, the `android` root | P0 |
| R2 | The posix sources on bionic; io_uring refused cleanly | P0 |
| R3 | The native entry on a thread of our own, arguments from the intent, output to logcat, the working directory | P0 |
| R4 | I/O on the sync backend; a thread ring after it | P0 / P1 |
| R5 | Assets and shaders from inside the APK: `mount_pack_at`, prefixed zips, `platform_read_bundled`, the paths | P0 |
| R6 | The window: `ANativeWindow`, insets, orientation, focus onto the hold, the surface lost and regained | P0 |
| R7 | `renderer-vulkan` on Android: loader, surface, rotation (identity P0, pre-rotation P1), composite alpha, the packaged validation layer, the device check that names what failed | P0 |
| R8 | Touch and keys; gamepads | P0 / P1 |
| R9 | Composed text through `KorvidActivity`, `javac` and `d8` | P1 |
| R10 | AAudio output, pause and resume, reconnection | P0 |
| R11 | `kormake package`: manifest, `aapt2`, `zipalign`, `apksigner`, debug key | P0 |
| R12 | The emulator test runner; every suite passing in it | P0 |
| R13 | The emulator's Vulkan read before the renderer is touched | P0 |
| R14 | Hashes equal to Linux's; replays play; gallery within tolerance | P0 |
| R15 | README, the port notes, module docs | P0 |

## Milestones

Each leaves every other platform building and Android running as far as it has got. Both Android platforms build at every step; only `androidemu` runs.

1. **Ground** (R1, R2, R12, R13): the tooling installed and the AVD up; the emulator's Vulkan read and the go or no-go written down. The toolchain, the probe, `arch`; every module below the window builds for both platforms; `emu_test.sh`; those suites pass in the emulator in debug and dev, `core`'s job and parallel suites a hundred times in a loop. kv's math golden hashes match (the first check of R14, and the first time they are run on a third libc).
2. **An app** (R3, R6 without the renderer, R11): shared-library executables, the entry and its thread, logcat, arguments, the packager. `window` installs, launches, logs its events and frame gaps, survives Home and back and a rotation. No drawing yet.
3. **Files** (R4's sync half, R5): the mounts into the APK, bundled reads, the paths. `platform`'s tests pass in the emulator; a packaged `window` reads an asset and a shader blob and logs their sizes.
4. **Drawing** (R6, R7): the loader, the surface, the swapchain's Android answers, the surface lost and regained, the validation layer packaged. `testbed` bounces its balls, `--check-sort` passes, the lifecycle loop is clean, the gallery compares.
5. **Playing** (R8, R10, R14): touch, keys, AAudio. Every game runs with the touchpad and sound; demos hash as Linux's do at 1 and 8 threads; replays play.
6. **Ship** (R4's ring, R7's pre-rotation, R8's gamepads, R15): the P1s that fit, the full root in three configs, the README section, the notes, and the list of what only a device can settle.
7. **Text** (R9): `KorvidActivity`. P1: if the slice is long by milestone 6, this is slice 9's, beside the device.

## For review

Decisions the milestones lean on, in the order a wrong answer costs most:

1. **Native, no SDL and no Gradle**, as on iOS and Windows. SDL3 on Android brings its Java activity and a Gradle project with it; `GameActivity` brings AndroidX. The cost is R3, R6, R8 and R10 written by hand, each of them a few hundred lines against a stable NDK API, and R9's one Java class when composed text is wanted.
2. **`NativeActivity` with our own glue**, not `native_app_glue` and not `GameActivity`. Google steers games to `GameActivity` for its text input and frame pacing; both are answerable later (R9; Swappy if a device shows uneven pacing) without changing the entry.
3. **Two platforms that differ by architecture** (`android`, `androidemu`), and an `arch` field, where a single platform with an ABI list would be the Android way. It follows `ios`/`iossim`, keeps a target one compiler invocation, and leaves a two-ABI package for the day there is a store to ask for one.
4. **API 33 as the floor.** Lower reaches older phones whose drivers the renderer refuses anyway; higher (35) would drop phones that can run it.
5. **The APK read as a zip by the engine's own pack code**, not `AAssetManager`. One read path on every platform, ranged reads into the queue's buffers, and the pack's alignment kept.
6. **No drawing fallbacks without a device.** The emulator will pass what a phone may fail, and a fallback written against a guess is written twice.
7. **Prerequisites on this machine:** the "Tooling" table installed, `ANDROID_HOME` set where kormake's non-interactive shell sees it, one AVD created.
