# Android port notes

What bringing Korvid to Android found, in the manner of `macos-port-notes.md`. The plan is
`docs/PRDs/Engine-Slice-8-PRD.md`. There is no device: everything here ran in the emulator
(an x86-64 guest, Android 16, API 36, on a Linux host), and the arm64 build is built and
never run.

## Milestone 1: ground

### The tooling

Installed with `sdkmanager` under `~/Android/Sdk`: command-line tools, platform tools,
build tools 36.0.0, platform 36, NDK r30 (30.0.16248370), the emulator, and the
`google_apis` x86-64 image of Android 16. One AVD, `korvid`, a Pixel 9. No Android Studio,
no Gradle. What the installing turned up:

- `avdmanager create avd` prints "Could not load devices from .../devices.xml" for this
  image and makes the AVD all the same: the file is optional and the image has none.
- `ANDROID_HOME` must be set before the `PATH` line that uses it; set after, `emulator` and
  `adb` are looked for in `/emulator` and `/platform-tools`. kormake needs neither on
  `PATH`: the probe and the test runner fall back to `~/Android/Sdk`.
- `/dev/kvm` was already writable by the desktop's user (an ACL from the seat), so the
  emulator ran accelerated without joining the `kvm` group.

### The emulator's Vulkan: go

`adb shell cmd gpu vkjson`, read before any renderer work. The guest's device is the
host's own GPU (`AMD Radeon 780M Graphics (RADV PHOENIX)`) at Vulkan 1.3.0. Everything
`renderer-vulkan` requires is there (`dynamicRendering`, `synchronization2`,
`shaderDrawParameters`, `drawIndirectFirstInstance`, descriptor indexing with the runtime
array, non-uniform indexing, update-after-bind, partially bound and variable count, 8.4
million update-after-bind sampled images, `VK_KHR_swapchain`, `VK_KHR_android_surface`),
and the optional three (`multiDrawIndirect`, pipeline statistics, timestamps). Calibrated
timestamps are not, so the GPU's spans will not sit on the CPU's clock. The guest has no
Vulkan layers of its own. None of this says what a phone's GPU will answer.

### The build

- **`toolchain-android`** serves `android` (arm64) and `androidemu` (x86-64) from one
  module with a section each, as `toolchain-apple` serves its three. The sections hold
  only the tag, the architecture and the test runner.
- **`arch`**, a new toolchain field. Until now a platform's name carried its architecture
  silently; these two differ in nothing else. `probe_android` passes
  `--target=<arch>-<triple>` (`x86_64-linux-android33`), and `probe_xcode` reads the same
  field where it had `arm64-` written in.
- **`probe_android`** finds the NDK the toolchain's `ndk` field pins (`ANDROID_NDK_HOME`,
  else `<sdk>/ndk/<version>` under `ANDROID_HOME`, `ANDROID_SDK_ROOT` or `~/Android/Sdk`)
  and hands back its `clang++` and `llvm-ar` by absolute path. The NDK holds a sysroot for
  every architecture, so there is nothing else to find.
- **API 33** is in the triple, and the linker's default page size is 16 KiB (NDK r28 and
  later): the executables' load segments are aligned to 0x4000 with no flag of ours.
- **`-static-libstdc++`**: the engine uses no STL, but the compiler's runtime calls come
  from libc++, and linked in a test is one file to push. A test needs `liblog`, `libm`,
  `libdl` and `libc`, all the system's.
- **No `render` binding yet.** A toolchain's binding must name a module that supports the
  platform, and `renderer-vulkan` joins at milestone 4.
- **The `android` root** is the twelve modules below the window: lua, tool, core, math,
  inflate, image, pack, xml, platform, cli, save, bulletml.

### Bionic

Nothing. The posix and linux sources compiled for bionic unchanged under
`-Wall -Wextra -Werror`, in debug, dev and ship, for both architectures: what the macOS
port made portable (the `mmap` flags, `accept` and `fcntl`, the paths behind
`os_base_path`) was what bionic would have tripped on. `/proc/self/exe` is right for a
test run from adb's shell; an app's paths are milestone 3's.

**io_uring is refused, cleanly.** The emulator answers `io_uring_setup` with `EPERM` to
adb's shell (an app is refused by seccomp as well), the queue logs "io_uring unavailable
(Permission denied): synchronous reads" and its default backend is sync. Nothing to change;
the thread ring is R4's.

### The tests

`tools/android/emu_test.sh` is both platforms' `test_runner`: it pushes the executable to
`/data/local/tmp/korvid/<target>/bin/` with the build's `assets/` and `shaders/` (adb's
sync, under a lock, so suites that start together send them once), runs it from the
directory above with `TMPDIR` beside it, and passes on exit code 0 and the suite's own
"checks passed" line together. It refuses, by name, a device of the other architecture.
Four suites at once.

- `kormake test androidemu-debug`, `-dev` and `-ship`: all twelve suites pass, with the
  check counts Linux has (core 6163, inflate 3344, pack 1168, platform 200, math 131,
  save 95, cli 75, image 55, xml 49, bulletml 48, lua 26, tool 8).
- **The math.** `math_tests` and `lua_tests` hold kv's golden hashes and pass: a third
  libc, a different compiler from Linux's (clang against g++), the same bits.
- **The stress loop.** `core_tests` (the job pool, the parallel suites, the remote frees
  among them) a hundred times in dev and a hundred in debug, in the emulator's four cores:
  200 passes, no failure. This is x86-64 again, so it says nothing new about weak memory;
  arm64 was macOS's to prove.
- `kormake build` for `android-debug`, `-dev` and `-ship` builds and links the same twelve
  for arm64. Not run.
- Linux: `kormake test linux-debug` and `linux-dev` pass as before, and kormake's own
  tests (`tools/kormake/tests/run.sh`).

## Milestone 2: an app

`window` builds as `libwindow.so`, `kormake package androidemu-debug window` writes
`dist/window/window.apk`, and it installs, launches, logs and survives being sent away.

### The build

- **An app is a shared library.** Three toolchain fields: `app_module` (`platform_hid`),
  `app_file` (`lib%s.so`) and `app_ldflags` (`-shared -Wl,--no-undefined
  -Wl,-u,ANativeActivity_onCreate`). An executable whose closure holds the window's module
  links that way; the rest, every test among them, stay programs for adb's shell. `main`
  stays `main`: the entry calls it by name.
- **`package = "apk"`** (`package_apk.lua`): a generated manifest (`org.korvid.<exe>`, API
  33 to 36, one `NativeActivity`, `hasCode="false"`, `extractNativeLibs="false"`,
  `configChanges` for turns and resizes, debuggable unless ship), then `aapt package` over
  the staged `lib/<abi>/` and `assets/` with every entry stored, `zipalign -P 16`,
  `apksigner` with the debug key (made by `keytool` the first time). Three tools from the
  build tools and the Java runtime; no Gradle, no `javac`.
- **`aapt`, not `aapt2`.** `aapt2 link` takes a manifest, resources and an assets directory
  and has no way to take `lib/`; `aapt add` has no "store this" switch and deflated the
  library, which a package with `extractNativeLibs="false"` may not hold. `aapt package`
  does all of it in one call. It is the old tool, still in build tools 36; if it leaves,
  the replacement is appending one stored entry to `aapt2`'s output ourselves.
- `apksigner` on Java 25 prints four "restricted method" warnings about its own native
  library and signs correctly; they go to `build/<target>/package.log`.

### The entry

`platform_hid/src/android/host_window_android.cc`, our own, in place of the NDK's
`native_app_glue`. `ANativeActivity_onCreate` (the OS's thread) sets the callbacks and
starts a thread that calls the game's `main`; `platform_app_main` is a plain call. The
callbacks post to a ring and wake an `ALooper` through a pipe; `pump` drains both and the
input queue. The window and the input queue are handed over under a lock with the OS's
thread waiting, since on its return the old one is gone.

- **Arguments**: the intent's string extra `args`, split at spaces.
  `adb shell 'am start -n org.korvid.window/android.app.NativeActivity --es args
  "--run-seconds 14"'`: the outer quotes matter, adb's shell splits the line again without
  them (the first try launched with `--run-seconds` alone and "6" as a package).
- **Output**: core gained `output_set_sink` (format.h): `print`, `print_err` and the log
  go through one `output_write`, to the C streams unless a sink is set. The Android entry
  sets one that cuts lines for logcat, tag `korvid` (`adb logcat -s korvid`). Core knows
  nothing of Android.
- **The end**: when `main` returns the activity is finished and the process `_exit`s with
  its result (logged as `exit N`). A library stays loaded in a process that lives on, and
  the next launch would meet the last one's statics.
- **Full screen**: the system's bars hidden through the window's insets controller and the
  window laid out under the cutout, by JNI on the OS's thread, again each time focus
  returns. The window's shape follows what was asked (`setRequestedOrientation`, sensor
  landscape or portrait) and `open` waits for the turn: `window` asked for 960x540 and got
  2424x1080.

### What it found

- **io_uring kills an app.** Under adb's shell `io_uring_setup` answered `EPERM` and the
  queue fell back, which is what milestone 1 saw. In an app the seccomp policy answers
  with `SIGSYS`: "Fatal signal 31 (SIGSYS), code 1 (SYS_SECCOMP), syscall 425". There is
  no error to fall back from, so `io_uring.cc` is compiled out on Android
  (`__linux__ && !__ANDROID__`) and the queue has no ring there until R4's.
- **The system's "Viewing full screen" tip takes the focus.** The first time any app hides
  the bars, Android shows a tip in a window of its own (`ImmersiveModeConfirmation`) and
  the app loses window focus until it is dismissed: `window` logged "focus gained, focus
  lost" in its first second and took no key. That is the system working, and the sim
  holding under it is right. In the emulator, once:
  `adb shell settings put secure immersive_mode_confirmations confirmed`.
- **Home in under a second does not take the surface.** A loop of Home and relaunch at
  0.4 s a step never destroyed the window (the launcher's animation had not finished); at
  a second a step each round does.

### What ran

- `window` in the emulator: opened, key A from `adb shell input keyevent`, Home ("focus
  lost", "no surface: waiting"), relaunched ("a surface again", "focus gained"), the
  recents screen and back, Back arriving as Escape, which closes it: `exit 0`. About 243
  frames a second with gaps of 4 to 7 ms while on screen.
- **The lifecycle loop**: Home and relaunch fifty times: 50 surfaces lost, 50 regained, the
  process alive and taking a key at the end, no ANR and no fatal signal.
- `kormake test` for `androidemu-debug`, `-dev` and `-ship`: thirteen suites now
  (`platform_hid_tests` with the Android key table), all passing. arm64 builds the same and
  packages `lib/arm64-v8a/libwindow.so`; not run.
- Linux: both suites' configs pass, kormake's own tests pass, `rocks --demo --hash` runs
  through the changed log and print.

### Not done here

- Touches reach `Input` (the code is iOS's shape) but `window` logs only the mouse, so
  nothing has shown them yet: milestone 5, with the touchpad.
- The insets read `142 137 0 63` in landscape; whether the top is the real status bar there
  or the portrait one read too early is checked with the touchpad on screen.
- A ship library is not stripped separately: ship compiles without `-g`.

## Milestone 3: files

An app's files are entries of its package, and the package is a zip: the engine's own pack
code reads it. No `AAssetManager`, one read path on every platform.

- **The layout.** What a desktop build has beside its executable is under `assets/` in the
  package: `assets/assets/<module>/...` (loose assets), `assets/shaders/`,
  `assets/<exe>.kpak`, the licenses. Every entry stored; `zipalign -P 16 4096`, so the
  library is on a 16 KiB boundary and every other entry on a 4 KiB one, which keeps a
  pack's own entries where it aligned them.
- **Two general forms of a mount** (`io.h`): `mount_pack(path, prefix)` shows only the
  entries under a prefix, named without it; `mount_pack_at(path, offset, size)` mounts an
  archive lying inside a file, every read at its offset. `io_archive_entry` finds a stored
  entry's bytes for the second. Opening an archive no longer needs a queue (`archive_open`
  inside io.cc), so the two helpers below have none.
- **`io_read_bundled(name)`**: a file that ships with the program, read now:
  `<exe dir>/<name>` on the desktop, the package's `assets/<name>` on Android (stored or
  deflated, the CRC checked). `renderer-vulkan` loads its SPIR-V through it instead of
  building a path from the base path. Metal and D3D12 still build theirs, which is right
  where they run; they were not touched, being unbuildable here.
- **Startup** (`platform/src/module.cc`): when `os_bundle_archive()` names an archive, the
  first mount is its `assets/assets/` and the game's pack is mounted in place from
  `assets/<exe>.kpak`. Elsewhere `os_bundle_archive` is an inline null.
- **The paths** (`platform/src/android/paths_android.cc`; the toolchain's source
  directories are now `posix` and `android`, since the one file under `linux` was the
  paths). `platform_android_app(package, internal)` (`korvid/platform/android.h`) is called
  by platform_hid's entry with `getPackageResourcePath()` and the activity's internal
  directory. An app has no base path, and its preference directory is
  `<internal>/<org>/<app>/`. A program that was never told (a test) keeps
  `/proc/self/exe`, and its preference directory is under `TMPDIR`.

### What ran

- `platform_tests` grew from 200 to 233 checks: an outer archive holding a pack and loose
  files under `assets/`, the pack mounted in place and read on both backends, the prefix
  hiding what is outside it, a prefix over a plain zip's deflated entry, the refusals. They
  pass on Linux and in the emulator.
- `window --bundled LICENSE --asset window/hello.txt`, packaged and launched: "mounted
  pack .../base.apk (under assets/assets/)", "bundled 'LICENSE': 11358 bytes", "asset
  'window/hello.txt': 30 bytes". The same two options on Linux read
  `shaders/sprite.spv` and the loose file.
- Linux: every suite, `rocks --demo --hash` unchanged (`4fb2e4cb04ff9ee4` at tick 600)
  with its shaders loaded through `io_read_bundled`, and from `rocks.kpak` in ship.

### Not shown yet

- A game's pack inside a package: no Android executable has a pack until the games build
  (milestone 5). The mount is tested with a made archive, not an installed one.
- The log's "9 unaligned" for the package is the pack format's own measure (its 4 KiB
  rule read from the central directory), not a fault.

## Milestone 4: drawing

`testbed` bounces its balls in the emulator at 60 fps, 2424x1080, and the gallery's nine
pictures are the Vulkan references' to the byte.

### The renderer

- **`vk_platform_android.cc`**: `dlopen("libvulkan.so")`, `VK_KHR_android_surface`, a
  surface from the `ANativeWindow`. `renderer-vulkan` is the toolchain's `render` binding;
  the headers are the NDK's (1.4.335).
- **The surface taken and given back.** New in the render interface:
  `renderer_set_surface(r, native)`. With null the Vulkan backend waits for the device,
  destroys the swapchain and every retired one in the frames' garbage (they were made from
  the surface), then the surface; with a window it makes a surface, checks the queue still
  presents to it, and the next frame builds the swapchain. A frame without a surface runs
  its uploads and presents nothing. Metal and D3D12 answer true and do nothing (two
  one-line functions, not compiled here).
- **Who calls it.** `HostWindow::on_surface` and `platform_surface_hook` (platform_hid):
  the frame loop registers the renderer after creating it. On Android the hook runs inside
  the window's handoff, on the game's thread: let go, then tell the OS's thread it may
  return, then take the new one. Nothing else calls it.
- **Rotation.** The swapchain passed the surface's `currentTransform` back as its
  `preTransform`, a promise to draw pre-rotated that nothing kept. It now asks for identity
  when the surface is turned and supports it, and the compositor turns the image: a
  landscape game on the Pixel's portrait panel is upright. Such a swapchain is "suboptimal"
  at every present for as long as it lives, so that answer no longer marks it dirty (it
  would be rebuilt every frame) and is not logged as an error. True pre-rotation is still
  R7's P1.
- **Composite alpha** is opaque where offered, else inherit.
- **A refused GPU is told why**: `score_device` logs the device and the one requirement it
  failed, each by name, and now checks the update-after-bind sampled-image limit against
  the texture slots.
- **The package** of a game that draws declares `android.hardware.vulkan.version` 1.3 and
  level 1, required.

### What it found

- **The emulator's driver crashes on debug names.** `vkSetDebugUtilsObjectNameEXT` on a
  query pool, and on an image, is a null dereference inside the guest's
  `vulkan.ranchu.so` (`vk_common_SetDebugUtilsObjectNameEXT+283`, emulator 37.2.13, API
  36 image revision 7). The extension is offered, so the renderer named its objects, as it
  does everywhere. `vk_platform_object_names()` is false when `ro.boot.qemu` is 1 and
  `rv_name` asks it; labels on command buffers are untouched and work. A device is
  unaffected, and the names come back by themselves there.
- **`wm size` gives the gallery its size.** The window is the screen, so the gallery drew
  at 2424x1080, comparable with nothing. `adb shell wm size 540x960` makes the emulator's
  display 960x540 in landscape; `wm size reset` after.
- **The gallery's `--out DIR` wants the directory to exist**; `--out .` is the app's
  external files directory.
- The insets in landscape read `142 137 0 63`, and `0 137 142 63` after the emulator is
  turned the other way up: the cutout follows the turn. The 137 at the top is what Android
  reports for the status bar there even while it is hidden.

### What ran

- `testbed --check-sort --screenshot shot.ppm`: 16 sort checks passed, 0 failed; the
  screenshot (2424x1080) pulled from `/sdcard/Android/data/org.korvid.testbed/files/`; 0
  validation errors (the layer was not there: see below).
- **The lifecycle loop with the renderer drawing**: Home and relaunch fifty times: "surface
  released" 50, "surface taken" 50, 60 fps and passing sort checks after, the sim's tick
  count showing it held while away, no error and no fatal signal. The emulator turned
  through all four ways up, drawing throughout.
- **The gallery** at 960x540 against `references/vulkan`: nine of nine, largest channel
  difference 0. The same GPU drew both, through a different OS, loader, surface and
  swapchain; that is what this proves.
- `kormake test` for the three `androidemu` configs: eighteen suites (asset, sprite, scene,
  text and touchpad joined). arm64 builds and packages. Linux: both configs' suites,
  kormake's tests, the gallery against its references, `rocks` and `arcanum` hashes
  unchanged (`4fb2e4cb04ff9ee4` at 600, `a3256024dcf1f933` at 1864).

### Not done

- **The validation layer is not downloaded**, so "debug builds run with it and it reports
  nothing" is unproved. The packager is ready: a debug package of a game that draws takes
  `<abi>/libVkLayer_khronos_validation.so` from `KORMAKE_ANDROID_VK_LAYERS` or
  `~/Android/Sdk/vulkan-validation-layers/` and says in its last line whether it did.
- Pre-rotation (P1).

### Still to come

Milestone 5: AAudio, touch shown working through the touchpad, the games, the hashes.
