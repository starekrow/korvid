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

### Still to come

Milestone 2: an executable that is a shared library, the native entry, logcat, the
packager.
