module "platform" {
  deps          = { "core" },
  external      = { sdl3 = ">=3.2" },
  engine_module = true,
}
