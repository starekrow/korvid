-- The macOS platform, on Apple silicon, with Apple's clang. The xcode probe asks xcrun for
-- the compiler and the SDK, so a build needs neither a particular PATH nor a Developer
-- shell. The rules are clang's (a depfile per object, ar for libraries, the compiler
-- driver for links), the flags Linux's, plus:
--   -arch arm64 / -mmacosx-version-min  Apple silicon only, macOS 26 and later (older releases can wait).
--   -ffp-contract=off  arm64 clang fuses multiply-adds by default and the x64 builds of the
--                      same source do not; off keeps a difference in a hash or a screenshot
--                      from being the compiler's choice. Each platform still keeps its libm.
--   -Wno-nullability-completeness  the SDK's headers, where this tool cannot reach them.
-- Source directories: posix and the Apple family's shared code, then macOS's own, then the
-- SDL3 backends (window and keyboard, gamepads) that Linux uses too.
module "toolchain.macos" {
  provides  = "toolchain",
  platforms = { "macos" },
  tag       = "macos",
  sdk       = "macosx",
  probe     = "xcode",
  srcdirs   = { "posix", "apple", "macos", "sdl3", "sdl3_pads" }, -- src/<dir>/ picked up on this platform, in this order
  cxx       = "clang++",
  ar        = "ar",
  cxxflags  = { "-std=c++20", "-arch", "arm64", "-mmacosx-version-min=26.0", "-fno-exceptions", "-fno-rtti",
                "-fno-strict-aliasing", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-Wno-nullability-completeness" },
  configs   = {
    debug = { cxxflags = { "-g", "-O0", "-DKV_DEBUG=1" } },
    dev   = { cxxflags = { "-g", "-O2", "-DKV_DEV=1" } },
    ship  = { cxxflags = { "-O2", "-DNDEBUG" } },
  },
  ldflags   = { "-arch", "arm64", "-mmacosx-version-min=26.0" },
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
    -- SDL3: Homebrew's package (brew install sdl3), or wherever KORMAKE_SDL3_DIR points.
    -- Pinned to the version the Windows toolchain pins. Its dylib carries an absolute
    -- install name, so a build runs where it was made; packaging bundles it (slice 7, M6).
    sdl3 = {
      dir     = "/opt/homebrew/opt/sdl3",
      include = { "include" },
      check   = "include/SDL3/SDL.h",
      libdirs = { "lib" },
      libs    = { "SDL3" },
      fetch   = "Install SDL3 3.4.20 with Homebrew (brew install sdl3), or set KORMAKE_SDL3_DIR to a folder that holds\n" ..
                "include/SDL3/SDL.h and lib/libSDL3.dylib. README.md, \"Building on macOS\", has the steps.",
    },
    zlib   = { libs = { "z" } },
    vulkan = { },                      -- nothing builds against Vulkan on this platform
  },
  bindings  = {
    ["shader-compile"] = "slang",
  },
}
