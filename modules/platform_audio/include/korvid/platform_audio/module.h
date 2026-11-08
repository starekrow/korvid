// Korvid platform_audio: module record. The device itself is audio.h's; the module's
// shutdown closes it, whoever opened it.
#pragma once
#include "korvid/core/module.h"
#include "korvid/platform_audio/audio.h"

namespace kv {

KV_MODULE_DECLARE(platform_audio_module);

} // namespace kv
