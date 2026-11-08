-- The audio device: one playback stream that pulls frames from a callback on its own
-- realtime thread. SDL3 is the backend. A tool that makes noise links this without a window.
module "platform_audio" {
  deps          = { "core", "platform" },
  external      = { sdl3 = ">=3.2" },
  engine_module = true,
}
