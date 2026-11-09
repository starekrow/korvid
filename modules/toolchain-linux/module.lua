-- The Linux platform: tag, source subdirectories, the compiler (a system lookup until
-- pinned compiler modules exist), flags per config, what each external means here, and
-- the default bindings for the interfaces this platform builds. The rules are gcc's (and
-- clang's): a depfile per object, ar for libraries, the compiler driver for links.
module "toolchain.linux" {
  provides  = "toolchain",
  platforms = { "linux" },
  tag       = "linux",
  srcdirs   = { "posix", "linux" },   -- src/<dir>/ picked up on this platform, in this order
  cxx       = "g++",
  ar        = "ar",
  cxxflags  = { "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-Wall", "-Wextra", "-Werror" },
  configs   = {
    debug = { cxxflags = { "-g", "-O0", "-DKV_DEBUG=1" } },
    dev   = { cxxflags = { "-g", "-O2", "-DKV_DEV=1" } },
    ship  = { cxxflags = { "-O2", "-DNDEBUG" } },
  },
  ldflags   = { "-lm", "-lpthread" },
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
  externals = {
    sdl3   = { libs = { "SDL3" } },
    zlib   = { libs = { "z" } },
    vulkan = { },                      -- the loader is reached through SDL; nothing to link
  },
  bindings  = {
    render           = "renderer-vulkan",
    ["shader-compile"] = "slang",
  },
}
