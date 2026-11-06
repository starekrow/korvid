// Module record. Startup finds the Vulkan loader through SDL and loads the global entry
// points; the renderer itself is created by renderer_create() once a window exists.
#include "korvid/renderer-vulkan/module.h"
#include "korvid/core/log.h"
#include "vk_funcs.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace kv {

static bool renderer_vulkan_startup(Engine&) {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        KV_ERROR("vulkan", "no Vulkan loader: %s", SDL_GetError());
        return false;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!vk_load_global(gipa)) {
        SDL_Vulkan_UnloadLibrary();
        return false;
    }
    u32 version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&version);
    KV_INFO("vulkan", "loader reports Vulkan %u.%u.%u",
            VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
    if (version < VK_API_VERSION_1_3) {
        KV_ERROR("vulkan", "Vulkan 1.3 is required");
        SDL_Vulkan_UnloadLibrary();
        return false;
    }
    return true;
}

static void renderer_vulkan_shutdown(Engine&) {
    vk_unload();
    SDL_Vulkan_UnloadLibrary();
}

ModuleDesc renderer_vulkan_module() {
    ModuleDesc m;
    m.name     = "renderer-vulkan";
    m.startup  = renderer_vulkan_startup;
    m.shutdown = renderer_vulkan_shutdown;
    return m;
}

} // namespace kv
