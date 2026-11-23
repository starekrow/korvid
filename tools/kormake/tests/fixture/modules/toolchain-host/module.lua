-- A toolchain whose commands are never run: only the Ninja file it shapes matters.
module "toolchain.host" {
  provides  = "toolchain",
  platforms = { "linux", "macos", "windows" },
  tag       = "host",
  srcdirs   = { "host" },
  cxx       = "fakecc",
  ar        = "fakear",
  cxxflags  = { "-std=c++20" },
  configs   = { dev = { cxxflags = { "-O1" } }, ship = { cxxflags = { "-O2" } } },
  ldflags   = { "-lm" },
  rules     = {
    cxx  = { command = "$cxx $cxxflags $defines $includes -c $in -o $out", depfile = "$out.d", deps = "gcc" },
    ar   = { command = "$ar $out $in" },
    link = { command = "$cxx $in -o $out $libs $ldflags" },
  },
  obj_suffix   = ".o",
  exe_suffix   = "",
  lib_file     = "lib%s.a",
  flag_include = "-I%s",
  flag_define  = "-D%s",
  codegen_profiles = { fast = { "-O3" }, wrapv = { "-fwrapv" } },
  -- What --sanitize builds the target with; the cross toolchain has none, and refuses it.
  sanitizers = {
    address   = { cxxflags = { "-fsanitize=address", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=address" } },
    undefined = { cxxflags = { "-fsanitize=undefined", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=undefined" } },
    thread    = { cxxflags = { "-fsanitize=thread", "-fno-omit-frame-pointer" }, ldflags = { "-fsanitize=thread" } },
  },
  flag_lib     = "-l%s",
  flag_libdir  = "-L%s",
  flag_include_system = "-isystem%s",
  externals = {
    sys     = { libs = { "sys" } },
    pkg     = { dir = "packages/pkg", include = { "include" }, libs = { "pkg" }, libdirs = { "lib" }, runtime = { "lib/pkg.dll" } },
    missing = { dir = "packages/missing", fetch = "Fetch it into packages/missing." },
  },
  bindings  = { gfx = "gfx-soft" },
}
