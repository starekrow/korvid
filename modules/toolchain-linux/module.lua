-- The Linux platform: tag, source subdirectories, the compiler (a system lookup until
-- pinned compiler modules exist), flags per config, what each external means here, and
-- the default bindings for the interfaces this platform builds. The rules are gcc's (and
-- clang's): a depfile per object, ar for libraries, the compiler driver for links. No
-- multiply is fused into an add (-ffp-contract=off: gcc's default in ISO mode, clang's
-- not) and nothing reads errno after a math function (-fno-math-errno, so the sqrt
-- builtin is the instruction alone): kv's math (modules/math) gives every platform the
-- same bits on those terms. -Wshadow (a name that hides another: a local, a parameter, a
-- member, a global) is what cl's /W4 already reports (C4456 to C4459), so a name that
-- breaks the Windows build breaks this one first; vendored sources turn it off with the
-- rest of their warnings.
module "toolchain.linux" {
  provides  = "toolchain",
  platforms = { "linux" },
  tag       = "linux",
  srcdirs   = { "posix", "linux", "sdl3", "sdl3_pads", "sdl3_audio" }, -- src/<dir>/ picked up on this platform, in this order
  cxx       = "g++",
  cc        = "gcc", -- C, for vendored libraries in it: their warnings are not ours (-w)
  cflags    = { "-std=gnu11", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno", "-w" },
  ar        = "ar",
  cxxflags  = { "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno", "-Wall", "-Wextra", "-Wshadow", "-Werror" },
  configs   = {
    debug = { cxxflags = { "-g", "-O0", "-DKV_DEBUG=1" }, cflags = { "-g", "-O0" } },
    dev   = { cxxflags = { "-g", "-O2", "-DKV_DEV=1" }, cflags = { "-g", "-O2" } },
    ship  = { cxxflags = { "-O2", "-DNDEBUG" }, cflags = { "-O2", "-DNDEBUG" } },
  },
  ldflags   = { "-lm", "-lpthread" },
  rules     = {
    cxx  = { command = '$cxx $cxxflags $defines $includes -MMD -MF $out.d -c $in -o $out', depfile = "$out.d", deps = "gcc" },
    c    = { command = '$cc $cflags $defines $includes -MMD -MF $out.d -c $in -o $out', depfile = "$out.d", deps = "gcc" },
    ar   = { command = "rm -f $out && $ar crs $out $in" },
    link = { command = "$cxx -o $out -Wl,--start-group $in $libs -Wl,--end-group $ldflags" }, -- a group: render's dispatcher calls the backends that depend on it,
  },
  obj_suffix   = ".o",
  exe_suffix   = "",
  lib_file     = "lib%s.a",
  -- Code-generation profiles a module asks for by name (`codegen`). fast: the CPU renderer's
  -- kernels and the games' generated shaders. No -ffp-contract=fast (arm64 always has FMA, and
  -- contraction would make the pictures differ by platform) and never -ffast-math (its
  -- crtfastmath.o flushes denormals for the whole process): docs/design/renderer-cpu.md.
  codegen_profiles = {
    fast  = { "-O3", "-fno-trapping-math" },
    wrapv = { "-fwrapv" }, -- signed arithmetic wraps, as a codec's bit tricks assume: what a source asks for by name (codegen_sources)
  },
  -- Instruction sets a module's isa_sources are built again for, chosen at start-up. No FMA:
  -- a contracted multiply-add would change the bytes the baseline draws.
  isa = { avx2 = { "-mavx2", "-mno-fma" } },
  -- What `kormake build|test --sanitize <names>` builds every target instance with. A
  -- finding fails the run: ASan and TSan stop at their first report, and UBSan is told to
  -- (-fno-sanitize-recover). A frame pointer on each, for the reports' stacks.
  sanitizers = {
    address   = { cxxflags = { "-fsanitize=address", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=address" } },
    undefined = { cxxflags = { "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=undefined" } },
    thread    = { cxxflags = { "-fsanitize=thread", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=thread" } },
  },
  flags_generated = { "-w" }, -- code another program wrote (a shader's C++): its warnings are not ours
  flag_include = "-I%s",
  flag_define  = "-D%s",
  flag_lib     = "-l%s",
  flag_libdir  = "-L%s",
  flag_include_system = "-isystem%s",
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
