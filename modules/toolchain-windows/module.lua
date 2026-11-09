-- The Windows platform, with MSVC. The msvc probe finds Visual Studio's cl, lib and link
-- and the SDK's headers and libraries, and the Ninja file holds them outright, so a build
-- needs no Developer prompt. The flags hold cl to the standard (/permissive-, the
-- conforming preprocessor, a truthful __cplusplus) and match the Linux build's choices:
-- no exceptions, no RTTI, warnings at /W4 as errors. Debug information goes in each
-- object (/Z7), which parallel compiles need; one runtime, the release DLL (/MD), in every
-- config, so prebuilt libraries link whatever the config. The ISO C functions are used on
-- purpose, so the CRT's deprecations steering to Annex K's _s functions are off. C4702
-- (unreachable code) is off: the optimizer reports it after inlining, so a fallback that
-- is dead in one config and needed in another (a return after KV_VERIFY(false), which
-- does not return in dev and is nothing in ship) trips it; gcc has no such warning. String
-- literals are pooled in every config (/GF; /O2 implies it, /Od does not), so one literal
-- has one address everywhere, as gcc and its linker make it: the profiler keys zones by
-- the address of their name. Threads
-- get Linux's 8 MiB stack; the main thread too (/STACK).
module "toolchain.windows" {
  provides  = "toolchain",
  platforms = { "windows" },
  tag       = "windows",
  srcdirs   = { "win32", "windows" },  -- src/<dir>/ picked up on this platform, in this order
  probe     = "msvc",
  cxx       = "cl",
  ar        = "lib",
  link      = "link",
  cxxflags  = { "/std:c++20", "/permissive-", "/Zc:preprocessor", "/Zc:__cplusplus", "/Zc:inline", "/utf-8",
                "/EHs-c-", "/GR-", "/D_HAS_EXCEPTIONS=0", "/MD", "/bigobj", "/W4", "/WX",
                "/D_CRT_SECURE_NO_WARNINGS", "/wd4702", "/GF" },
  configs   = {
    debug = { cxxflags = { "/Od", "/Z7", "/DKV_DEBUG=1" }, ldflags = { "/DEBUG" } },
    dev   = { cxxflags = { "/O2", "/Z7", "/DKV_DEV=1" }, ldflags = { "/DEBUG" } },
    ship  = { cxxflags = { "/O2", "/DNDEBUG" } },
  },
  ldflags   = { "/INCREMENTAL:NO", "/STACK:8388608" },
  rules     = {
    cxx  = { command = '$cxx /nologo /showIncludes $cxxflags $defines $includes /c $in /Fo$out', deps = "msvc" },
    ar   = { command = "$ar /nologo /OUT:$out $in" },
    link = { command = "$link /nologo /OUT:$out $in $libs $ldflags" },
  },
  obj_suffix   = ".obj",
  exe_suffix   = ".exe",
  lib_file     = "%s.lib",
  flag_include = "/I%s",
  flag_define  = "/D%s",
  flag_lib     = "%s.lib",
  externals = {
    sdl3   = { libs = { "SDL3" } },
    vulkan = { },                      -- the loader is reached through SDL; nothing to link
  },
  bindings  = {
    ["shader-compile"] = "slang",
  },
}
