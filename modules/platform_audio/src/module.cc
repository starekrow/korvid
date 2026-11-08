#include "korvid/platform_audio/module.h"
#include "korvid/core/engine.h"
#include <SDL3/SDL.h>

namespace kv {

namespace {

bool audio_dev_startup(Engine&, ModuleInstance&) {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    return true; // the device opens on request (platform_audio_open)
}

void audio_dev_shutdown(Engine& e, ModuleInstance&) {
    platform_audio_close(e);
    if (SDL_WasInit(0) == 0) SDL_Quit(); // the last SDL user out (platform_hid may still hold video)
}

} // namespace

KV_MODULE_DEFINE(platform_audio_module) = {
    .name     = "platform_audio",
    .startup  = audio_dev_startup,
    .shutdown = audio_dev_shutdown,
};

} // namespace kv
