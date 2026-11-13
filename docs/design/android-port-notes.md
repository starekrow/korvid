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

### Still to come

Milestone 3: assets and shaders from inside the package.
