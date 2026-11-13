-- Android: a phone's arm64 (`android`) and the emulator's x86-64 (`androidemu`), with the
-- NDK's clang. The android probe finds the NDK (ANDROID_NDK_HOME, or the version `ndk` pins
-- under ANDROID_HOME/ndk/, ~/Android/Sdk when that is unset) and passes
-- --target=<arch>-<triple>, so a build needs no particular PATH. The rules are clang's (a
-- depfile per object, llvm-ar for libraries, the compiler driver for links), the flags
-- Linux's, plus:
--   triple             linux-android33: API 33, Android 13, the first release whose new
--                      devices mostly carry Vulkan 1.3 drivers (the renderer refuses a
--                      device below 1.3 at start, whatever the OS).
--   -fPIC              everything on Android is position independent; an app's executable
--                      is a shared library the OS loads.
--   -ffp-contract=off  arm64 clang fuses multiply-adds by default and the x64 build of the
--                      same source does not: here that is two builds of one platform. kv's
--                      math (modules/math) counts on it to give both the same bits.
--   -static-libstdc++  the engine uses no STL, but the compiler's own runtime calls
--                      (operator new, the guards of local statics) come from libc++; linked
--                      in, a test is one file to push and a package carries no
--                      libc++_shared.so.
-- Source directories: posix, then linux (what is the kernel's, not glibc's: bionic is a
-- third libc, and docs/design/android-port-notes.md lists what it lacks), then android.
-- No SDL: the window, input and audio are native, as on iOS and Windows.
--
-- The two platforms differ in their architecture and in how a test runs; per_platform holds
-- that. Tests are ordinary executables and run in the emulator through adb
-- (tools/android/emu_test.sh): start one first (emulator -avd korvid -gpu host).
module "toolchain.android" {
  provides  = "toolchain",
  platforms = { "android", "androidemu" },
  family    = "android",
  probe     = "android",
  ndk       = "30.0.16248370", -- r30, the LTS
  triple    = "linux-android33",
  srcdirs   = { "posix", "linux", "android" }, -- src/<dir>/ picked up on these platforms, in this order
  cxx       = "clang++",
  ar        = "llvm-ar",
  cxxflags  = { "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno",
                "-fPIC", "-Wall", "-Wextra", "-Werror" },
  configs   = {
    debug = { cxxflags = { "-g", "-O0", "-DKV_DEBUG=1" } },
    dev   = { cxxflags = { "-g", "-O2", "-DKV_DEV=1" } },
    ship  = { cxxflags = { "-O2", "-DNDEBUG" } },
  },
  -- liblog: where an app's output goes (logcat); libm and the threads are bionic's libc.
  ldflags   = { "-static-libstdc++", "-llog" },
  rules     = {
    cxx  = { command = '$cxx $cxxflags $defines $includes -MMD -MF $out.d -c $in -o $out', depfile = "$out.d", deps = "gcc" },
    ar   = { command = "rm -f $out && $ar crs $out $in" },
    link = { command = "$cxx $in -o $out $libs $ldflags" },
  },
  obj_suffix   = ".o",
  exe_suffix   = "",
  lib_file     = "lib%s.a",
  flag_include = "-I%s",
  flag_define  = "-D%s",
  flag_lib     = "-l%s",
  flag_libdir  = "-L%s",
  flag_include_system = "-isystem%s",
  externals = {
    zlib   = { libs = { "z" } },
  },
  bindings  = {
    ["shader-compile"] = "slang",
  },
  per_platform = {
    -- A device. Unrun until there is one: the runner is the emulator's, against whatever
    -- adb shows connected.
    android = {
      tag  = "android",
      arch = "aarch64",
      test_runner = { "sh", "%{root}/tools/android/emu_test.sh" },
      test_jobs   = 4,
    },
    -- The emulator on an x86-64 host: the same OS on this machine's own architecture.
    androidemu = {
      tag  = "androidemu",
      arch = "x86_64",
      test_runner = { "sh", "%{root}/tools/android/emu_test.sh" },
      test_jobs   = 4,
    },
  },
}
