module "platform" {
  deps          = { "core", "pack", "inflate" },
  external      = { sdl3 = ">=3.2" },
  engine_module = true,
}
