-- The Windows platform, with MSVC. The msvc probe finds Visual Studio's cl, lib and link
-- and the SDK's headers and libraries, and the Ninja file holds them outright, so a build
-- needs no Developer prompt. The flags hold cl to the standard (/permissive-, the
-- conforming preprocessor, a truthful __cplusplus) and match the Linux build's choices:
-- no exceptions, no RTTI, warnings at /W4 as errors. Debug information goes in each
-- object (/Z7), which parallel compiles need; one runtime, the release DLL (/MD), in every
-- config, so prebuilt libraries link whatever the config. The ISO C functions are used on
-- purpose, so the CRT's deprecations steering to Annex K's _s functions are off. UNICODE:
-- every Windows macro that has a narrow and a wide form names the wide one. C4702
-- (unreachable code) is off: the optimizer reports it after inlining, so a fallback that
-- is dead in one config and needed in another (a return after KV_VERIFY(false), which
-- does not return in dev and is nothing in ship) trips it; gcc has no such warning. String
-- literals are pooled in every config (/GF; /O2 implies it, /Od does not), so one literal
-- has one address everywhere, as gcc and its linker make it: the profiler keys zones by
-- the address of their name. Floating point is /fp:precise, which is cl's default, said
-- outright because kv's math (modules/math) counts on it: no multiply fused into an add,
-- nothing reordered, so every platform gets the same bits. Threads
-- get Linux's 8 MiB stack; the main thread too (/STACK). The system DLLs the platform
-- modules use for one thing each (shell32 and ole32 for the preference directory, ws2_32
-- for sockets, imm32 for an input method) are delay-loaded, opened on the first call:
-- imported outright, shell32 brings user32, gdi32 and the rest of the shell with it, 4 ms
-- of every process's start, which a tool that starts many (goldberg's converters) pays
-- thousands of times. LNK4199 is that a program never calls one of them.
module "toolchain.windows" {
  provides  = "toolchain",
  platforms = { "windows" },
  tag       = "windows",
  srcdirs   = { "win32", "windows", "sdl3_pads", "sdl3_audio" }, -- src/<dir>/ picked up on this platform, in this order
  probe     = "msvc",
  cxx       = "cl",
  ar        = "lib",
  link      = "link",
  cxxflags  = { "/std:c++20", "/permissive-", "/Zc:preprocessor", "/Zc:__cplusplus", "/Zc:inline", "/utf-8",
                "/EHs-c-", "/GR-", "/D_HAS_EXCEPTIONS=0", "/MD", "/bigobj", "/W4", "/WX",
                "/D_CRT_SECURE_NO_WARNINGS", "/DUNICODE", "/D_UNICODE", "/wd4702", "/GF", "/fp:precise" },
  configs   = {
    debug = { cxxflags = { "/Od", "/Z7", "/DKV_DEBUG=1" }, ldflags = { "/DEBUG" } },
    dev   = { cxxflags = { "/O2", "/Z7", "/DKV_DEV=1" }, ldflags = { "/DEBUG" } },
    ship  = { cxxflags = { "/O2", "/DNDEBUG" } },
  },
  ldflags   = { "/INCREMENTAL:NO", "/STACK:8388608", "delayimp.lib", "/DELAYLOAD:shell32.dll",
                "/DELAYLOAD:ole32.dll", "/DELAYLOAD:ws2_32.dll", "/DELAYLOAD:imm32.dll", "/IGNORE:4199" },
  rules     = {
    cxx  = { command = '$cxx /nologo /showIncludes $cxxflags $defines $includes /c $in /Fo$out', deps = "msvc" },
    ar   = { command = "$ar /nologo /OUT:$out $in" },
    link = { command = "$link /nologo /OUT:$out $in $libs $ldflags" },
  },
  obj_suffix   = ".obj",
  exe_suffix   = ".exe",
  lib_file     = "%s.lib",
  flags_generated = { "/w" }, -- code another program wrote (a shader's C++): its warnings are not ours
  flag_include = "/I%s",
  flag_define  = "/D%s",
  flag_lib     = "%s.lib",
  flag_libdir  = "/LIBPATH:%s",
  flag_include_system = "/external:I%s",
  externals = {
    -- SDL3: the SDL project's prebuilt Visual C++ package, unpacked under vendor/ (which git
    -- ignores), or wherever KORMAKE_SDL3_DIR points. Pinned: a new version is a new dir
    -- here and in the README, and every machine fetches it again.
    sdl3 = {
      dir     = "vendor/SDL3-3.4.20",
      include = { "include" },
      check   = "include/SDL3/SDL.h",
      libdirs = { "lib/x64" },
      libs    = { "SDL3" },
      runtime = { "lib/x64/SDL3.dll" },
      fetch   = "Download SDL3-devel-3.4.20-VC.zip (16.1 MB, SHA-256 1ec3009655ee37b1f73e52b3b120acc5afd8abaf9532f3303b1bf1dfe91b5969)\n" ..
                "from https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20 and unpack it into vendor/,\n" ..
                "so that vendor/SDL3-3.4.20/include/SDL3/SDL.h exists. README.md, \"Building on Windows\", has the steps.",
    },
    -- Vulkan: the headers from the LunarG Vulkan SDK, whose installer sets VULKAN_SDK (and
    -- puts slangc, the shader compiler, on PATH). Nothing to link: the loader,
    -- vulkan-1.dll, comes with the GPU driver and is opened at run time.
    vulkan = {
      env     = "VULKAN_SDK",
      include = { "Include" },
      check   = "Include/vulkan/vulkan.h",
      fetch   = "Install the Vulkan SDK (winget install KhronosGroup.VulkanSDK, or\n" ..
                "https://vulkan.lunarg.com/sdk/home), then open a new prompt so VULKAN_SDK and PATH are set.\n" ..
                "README.md, \"Building on Windows\", has the steps.",
    },
    -- Direct3D 12 (renderer-d3d12, when kormake.lua binds it): the Windows SDK's headers,
    -- which the msvc probe already puts on every compile, and its import libraries. The
    -- runtime is the system's d3d12.dll and dxgi.dll.
    d3d12 = { libs = { "d3d12", "dxgi", "dxguid" } },
  },
  bindings  = {
    render             = "renderer-vulkan",
    ["shader-compile"] = "slang",
  },
}
