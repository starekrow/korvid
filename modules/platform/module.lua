-- The operating system with no display and no devices: the I/O queue and its mounts, the
-- executable's and the user's directories, sockets. No external library, so a tool can
-- link it on a bare build machine. The window and input are platform_hid; the audio device
-- is platform_audio.
module "platform" {
  deps          = { "core", "pack", "inflate" },
  engine_module = true,
}
