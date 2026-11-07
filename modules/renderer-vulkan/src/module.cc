// Module record. Startup finds the Vulkan loader through SDL and loads the global entry
// points; the renderer itself is created by renderer_create() once a window exists.
// No events, resources or systems: the renderer runs at frame rate from the frame loop.
#include "korvid/renderer-vulkan/module.h"
#include "korvid/render/renderer.h"
#include "korvid/core/log.h"
#include "vk_funcs.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace kv {

namespace {

bool renderer_vulkan_startup(Engine&, ModuleInstance& self) {
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
    RendererVulkanState* s = arena_push<RendererVulkanState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    return true;
}

void renderer_vulkan_shutdown(Engine&, ModuleInstance& self) {
    RendererVulkanState* s = self.state_as<RendererVulkanState>();
    if (s && s->renderer) renderer_destroy(s->renderer); // clears s->renderer
    vk_unload();
    SDL_Vulkan_UnloadLibrary();
}

} // namespace

KV_MODULE_DEFINE(renderer_vulkan_module) = {
    .name     = "renderer-vulkan",
    .startup  = renderer_vulkan_startup,
    .shutdown = renderer_vulkan_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
