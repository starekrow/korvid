-- The Apple family: macOS, iOS and the iOS Simulator, on Apple silicon, with Apple's clang.
-- The xcode probe asks xcrun for the compiler and the platform's SDK, so a build needs
-- neither a particular PATH nor a Developer shell, and passes -target arm64-<triple>. The
-- rules are clang's (a depfile per object, ar for libraries, the compiler driver for
-- links), the flags Linux's, plus:
--   triple             Apple silicon only; macOS 26 and iOS 26 and later (older releases can
--                      wait; iOS 26 is the first the SDK's floor and the Metal floor, A14 /
--                      MTLGPUFamilyApple7, agree on well enough: A13 devices run iOS 26, and
--                      the renderer refuses them at start).
--   -ffp-contract=off  arm64 clang fuses multiply-adds by default and the x64 builds of the
--                      same source do not; off keeps a difference in a hash or a screenshot
--                      from being the compiler's choice. kv's math (modules/math) counts on
--                      it to give every platform the same bits.
--   -fno-math-errno    nothing reads errno after a math function; without it the sqrt
--                      builtin keeps a call into libm for a negative argument.
--   -Wno-nullability-completeness  the SDK's headers, where this tool cannot reach them.
-- Source directories: posix and the family's shared code (apple: nothing AppKit or UIKit,
-- which building for iOS checks), then the platform's own; macOS adds the SDL3 backends
-- (window and keyboard, gamepads) that Linux uses too. iOS is native (UIKit, Core Audio,
-- GameController), as Windows' window is: no SDL there.
--
-- The three platforms differ in their SDK, their triple, their own source directory, how
-- they package and where SDL3 comes from; per_platform holds that (kormake lays a section
-- over the fields above it: lists appended, maps merged). The rest is shared.
local os_min = "26.0"
module "toolchain.apple" {
  provides  = "toolchain",
  platforms = { "macos", "ios", "iossim" },
  family    = "apple",
  probe     = "xcode",
  os_min    = os_min,
  srcdirs   = { "posix", "apple" }, -- src/<dir>/ picked up on these platforms, in this order
  cxx       = "clang++",
  ar        = "ar",
  cxxflags  = { "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno",
                "-Wall", "-Wextra", "-Werror", "-Wno-nullability-completeness" },
  configs   = {
    debug = { cxxflags = { "-g", "-O0", "-DKV_DEBUG=1" } },
    dev   = { cxxflags = { "-g", "-O2", "-DKV_DEV=1" } },
    ship  = { cxxflags = { "-O2", "-DNDEBUG" } },
  },
  -- CoreFoundation: platform_hid's run-loop timer for live resize. Every process on an
  -- Apple platform loads it already; linking it names what the engine calls directly.
  ldflags   = { "-framework", "CoreFoundation" },
  rules     = {
    cxx  = { command = '$cxx $cxxflags $defines $includes -MMD -MF $out.d -c $in -o $out', depfile = "$out.d", deps = "gcc" },
    ar   = { command = "rm -f $out && $ar crs $out $in" },
    link = { command = "$cxx $in -o $out $libs $ldflags" },
  },
  obj_suffix   = ".o",
  exe_suffix   = "",
  lib_file     = "lib%s.a",
  -- Code-generation profiles a module asks for by name (`codegen`). fast: the CPU renderer's
  -- kernels and the games' generated shaders. No -ffp-contract=fast (arm64 always has FMA, and
  -- contraction would make the pictures differ by platform) and never -ffast-math (its
  -- crtfastmath.o flushes denormals for the whole process): docs/design/renderer-cpu.md.
  codegen_profiles = { fast = { "-O3", "-fno-trapping-math" } },
  flags_generated = { "-w" }, -- code another program wrote (a shader's C++): its warnings are not ours
  flag_include = "-I%s",
  flag_define  = "-D%s",
  flag_lib     = "-l%s",
  flag_libdir  = "-L%s",
  flag_include_system = "-isystem%s",
  flag_framework = "-framework %s",
  externals = {
    zlib   = { libs = { "z" } },
    vulkan = { },                      -- nothing builds against Vulkan on these platforms
    -- Metal and the layer it presents through: the SDK's frameworks, nothing to install.
    metal  = { frameworks = { "Metal", "QuartzCore", "Foundation" } },
  },
  bindings  = {
    render             = "renderer-metal",
    ["shader-compile"] = "slang",
  },
  per_platform = {
    macos = {
      tag     = "macos",
      sdk     = "macosx",
      triple  = "apple-macos" .. os_min,
      srcdirs = { "macos", "sdl3", "sdl3_pads", "sdl3_audio" },
      -- CoreServices: FSEvents, the file watcher's native backend (platform's macos/).
      ldflags = { "-framework", "CoreServices" },
      -- kormake package writes an .app: the executable, its resources, SDL3 bundled, signed.
      package = "app",
      externals = {
        -- SDL3: Homebrew's package (brew install sdl3), or wherever KORMAKE_SDL3_DIR points.
        -- Pinned to the version the Windows toolchain pins. Its dylib carries an absolute
        -- install name, so a build runs where it was made; packaging bundles it.
        sdl3 = {
          dir     = "/opt/homebrew/opt/sdl3",
          include = { "include" },
          check   = "include/SDL3/SDL.h",
          libdirs = { "lib" },
          libs    = { "SDL3" },
          fetch   = "Install SDL3 3.4.20 with Homebrew (brew install sdl3), or set KORMAKE_SDL3_DIR to a folder that holds\n" ..
                    "include/SDL3/SDL.h and lib/libSDL3.dylib. README.md, \"Building on macOS\", has the steps.",
        },
      },
    },
    -- iOS on a device. Tests run on the one connected device, each signed with the
    -- development profile and installed as the same app, KORMAKE_IOS_TEST_BUNDLE_ID
    -- (kormake's scripts/ios.lua has the rest).
    ios = {
      tag     = "ios",
      sdk     = "iphoneos",
      triple  = "apple-ios" .. os_min,
      srcdirs = { "ios" },
      test_runner = { "%{kormake}", "_ios_test" },
      test_jobs   = 1,
      externals = {
        -- The window, its view and the lifecycle; the audio unit. The SDK's, nothing to install.
        uikit     = { frameworks = { "UIKit", "QuartzCore", "Foundation" } },
        coreaudio = { frameworks = { "AudioToolbox", "AVFAudio" } },
      },
      package = "iosapp",
    },
    -- The iOS Simulator: an iOS build for this Mac's own arm64. Tests run in the booted
    -- simulator (xcrun simctl boot "<device>", or open Simulator.app, first).
    iossim = {
      tag     = "iossim",
      sdk     = "iphonesimulator",
      triple  = "apple-ios" .. os_min .. "-simulator",
      srcdirs = { "ios" },
      externals = {
        -- The window, its view and the lifecycle; the audio unit. The SDK's, nothing to install.
        uikit     = { frameworks = { "UIKit", "QuartzCore", "Foundation" } },
        coreaudio = { frameworks = { "AudioToolbox", "AVFAudio" } },
      },
      package = "iosapp",
      test_runner = { "xcrun", "simctl", "spawn", "booted" },
    },
  },
}
