-- A library on a system package that is in no directory of its own: absolute paths.
module "codec" {
  external  = { sysaudio = ">=1" },
  externals = {
    linux = { sysaudio = { include = { "/usr/include/sysaudio-1" }, libdirs = { "/usr/lib/sysaudio" }, libs = { "sysaudio" } } },
    macos = { sysaudio = { include = { "/usr/include/sysaudio-1" }, libdirs = { "/usr/lib/sysaudio" }, libs = { "sysaudio" } } },
  },
}
