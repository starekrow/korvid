// Korvid platform_audio: what module.cc asks of the backend, one per platform, chosen by the
// toolchain's source directories: src/sdl3_audio/ (SDL3, the desktop) or src/ios/ (Core Audio).
// Each also implements audio.h.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// At module startup, before any device: true to go on.
bool audio_backend_startup();
// At module shutdown, after platform_audio_close.
void audio_backend_shutdown();

} // namespace kv
