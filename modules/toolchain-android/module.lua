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
-- Source directories: posix and android. Not linux: the one file there is the paths, and
-- an Android app's are its own (platform/src/android). The posix sources met bionic, a
-- third libc, unchanged (docs/design/android-port-notes.md).
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
  srcdirs   = { "posix", "android" }, -- src/<dir>/ picked up on these platforms, in this order
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
  flags_generated = { "-w" }, -- code another program wrote (a shader's C++): its warnings are not ours
  flag_include = "-I%s",
  flag_define  = "-D%s",
  flag_lib     = "-l%s",
  flag_libdir  = "-L%s",
  flag_include_system = "-isystem%s",
  externals = {
    zlib    = { libs = { "z" } },
    -- The NDK's own: the activity, its window, the looper and the input queue.
    android = { libs = { "android" } },
    -- The audio device: AAudio, the system's since API 26.
    aaudio  = { libs = { "aaudio" } },
    -- The system's libvulkan.so, opened at run time; the headers are the NDK's.
    vulkan  = { },
  },
  -- An executable that links the window is an app: the OS loads lib<name>.so into a process
  -- of its own making and calls ANativeActivity_onCreate (platform_hid/src/android), which
  -- runs the game's main on a thread. Everything else, the tests among them, is a program
  -- for adb's shell. --no-undefined: a shared link would otherwise leave a missing symbol
  -- for the device's loader to find.
  app_module  = "platform_hid",
  app_file    = "lib%s.so",
  app_ldflags = { "-shared", "-Wl,--no-undefined", "-Wl,-u,ANativeActivity_onCreate" },
  -- kormake package writes an .apk (package_apk.lua) with the SDK's build tools, checked
  -- against this API level.
  package     = "apk",
  build_tools = "36.0.0",
  target_api  = "36",
  os_min      = "33",
  bindings  = {
    render             = "renderer-vulkan",
    ["shader-compile"] = "slang",
  },
  per_platform = {
    -- A device. Unrun until there is one: the runner is the emulator's, against whatever
    -- adb shows connected.
    android = {
      tag  = "android",
      arch = "aarch64",
      abi  = "arm64-v8a",
      test_runner = { "sh", "%{root}/tools/android/emu_test.sh" },
      test_jobs   = 4,
    },
    -- The emulator on an x86-64 host: the same OS on this machine's own architecture.
    androidemu = {
      tag  = "androidemu",
      arch = "x86_64",
      abi  = "x86_64",
      test_runner = { "sh", "%{root}/tools/android/emu_test.sh" },
      test_jobs   = 4,
    },
  },
}
