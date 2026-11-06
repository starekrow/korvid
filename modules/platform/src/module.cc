#include "korvid/platform/module.h"
#include "korvid/core/log.h"
#include <SDL3/SDL.h>

namespace kv {

static bool platform_startup(Engine&) {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        KV_ERROR("platform", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("platform", "SDL %d.%d.%d, video driver '%s'",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver),
            SDL_GetCurrentVideoDriver());
    return true;
}

static void platform_shutdown(Engine&) { SDL_Quit(); }

ModuleDesc platform_module() {
    ModuleDesc m;
    m.name     = "platform";
    m.startup  = platform_startup;
    m.shutdown = platform_shutdown;
    return m;
}

} // namespace kv
