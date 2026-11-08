module "platform" {
  deps          = { "core", "math", "pack", "inflate" },
  external      = { sdl3 = ">=3.2" },
  engine_module = true,
}
