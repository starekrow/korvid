#include "korvid/platform_audio/module.h"
#include "korvid/core/engine.h"
#include "audio_backend.h"

namespace kv {

namespace {

bool audio_dev_startup(Engine&, ModuleInstance&) {
    return audio_backend_startup(); // the device opens on request (platform_audio_open)
}

void audio_dev_shutdown(Engine& e, ModuleInstance&) {
    platform_audio_close(e);
    audio_backend_shutdown();
}

} // namespace

KV_MODULE_DEFINE(platform_audio_module) = {
    .name     = "platform_audio",
    .startup  = audio_dev_startup,
    .shutdown = audio_dev_shutdown,
};

} // namespace kv
