-- The audio device: one playback stream that pulls frames from a callback on its own
-- realtime thread. SDL3 is the backend on the desktop (src/sdl3_audio/), Core Audio on iOS
-- (src/ios/). A tool that makes noise links this without a window.
module "platform_audio" {
  deps          = { "core", "platform" },
  external      = { sdl3 = { ">=3.2", platforms = { "linux", "windows", "macos" } }, coreaudio = { ">=1", platforms = { "ios", "iossim" } } },
  engine_module = true,
}
