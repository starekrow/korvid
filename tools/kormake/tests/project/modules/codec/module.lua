-- A library on a system package that is in no directory of its own: absolute paths. Its
-- decoder wants wrapping arithmetic: a profile on one source, which leaves the unity file,
-- beside a list of profiles for the rest. A vendored library in C, wrapped by two .c files
-- of its own: a unity file of C beside the C++ one, built with the toolchain's cc.
module "codec" {
  external  = { sysaudio = ">=1" },
  codegen   = { "fast" },
  codegen_sources = { wrapv = { "src/decode.cc" } },
  externals = {
    linux = { sysaudio = { include = { "/usr/include/sysaudio-1" }, libdirs = { "/usr/lib/sysaudio" }, libs = { "sysaudio" } } },
    macos = { sysaudio = { include = { "/usr/include/sysaudio-1" }, libdirs = { "/usr/lib/sysaudio" }, libs = { "sysaudio" } } },
  },
}
