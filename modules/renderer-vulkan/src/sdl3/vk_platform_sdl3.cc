// The Vulkan platform layer through SDL3, where the window is SDL's: SDL finds the loader
// through its video driver and knows which surface extensions that driver needs.
#include "../vk_platform.h"
#include "korvid/core/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace kv {

PFN_vkGetInstanceProcAddr vk_platform_load() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        KV_ERROR("vulkan", "no Vulkan loader: %s", SDL_GetError());
        return nullptr;
    }
    return (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
}

void vk_platform_unload() { SDL_Vulkan_UnloadLibrary(); }

const char* const* vk_platform_instance_extensions(u32* count) {
    Uint32 n = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!exts) KV_ERROR("vulkan", "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
    *count = exts ? (u32)n : 0;
    return exts;
}

bool vk_platform_create_surface(void* native_window, VkInstance instance, VkSurfaceKHR* out) {
    if (SDL_Vulkan_CreateSurface((SDL_Window*)native_window, instance, nullptr, out)) return true;
    KV_ERROR("vulkan", "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    return false;
}

} // namespace kv
