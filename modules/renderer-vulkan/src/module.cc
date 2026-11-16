// Module record. Startup finds the Vulkan loader (vk_platform.h) and loads the global entry
// points; the renderer itself is created by renderer_create() once a window exists.
// No events, resources or systems: the renderer runs at frame rate from the frame loop.
#include "korvid/renderer-vulkan/module.h"
#include "korvid/render/renderer.h"
#include "korvid/core/log.h"
#include "korvid/platform/module.h"
#include "korvid/platform_hid/module.h"
#include "vk_funcs.h"
#include "vk_platform.h"

#ifndef KV_BIND_RENDER_COUNT
#define KV_BIND_RENDER_COUNT 1
#endif

namespace kv {

bool g_vulkan_ready = false;

namespace {

// Without Vulkan the module fails, and the engine with it, unless the build binds another
// backend to fall back on (renderer-cpu): then it starts, empty, and renderer_probe says no.
bool unavailable(const char* why) {
    if (KV_BIND_RENDER_COUNT < 2) return false;
    KV_INFO("vulkan", "unavailable (%s): another renderer will draw", why);
    return true;
}

bool renderer_vulkan_startup(Engine& e, ModuleInstance& self) {
    if (!platform_video_up(e)) return false; // the display first: SDL finds the loader through its video driver
    PFN_vkGetInstanceProcAddr gipa = vk_platform_load();
    if (!gipa) return unavailable("no Vulkan loader");
    if (!vk_load_global(gipa)) {
        vk_platform_unload();
        return unavailable("the loader gave no entry points");
    }
    u32 version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&version);
    KV_INFO("vulkan", "loader reports Vulkan %u.%u.%u",
            VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
    if (version < VK_API_VERSION_1_3) {
        KV_ERROR("vulkan", "Vulkan 1.3 is required");
        vk_platform_unload();
        return unavailable("the loader is older than 1.3");
    }
    RendererVulkanState* s = arena_push<RendererVulkanState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    g_vulkan_ready = true;
    return true;
}

void renderer_vulkan_shutdown(Engine&, ModuleInstance& self) {
    RendererVulkanState* s = self.state_as<RendererVulkanState>();
    if (s && s->renderer) renderer_destroy(s->renderer); // clears s->renderer
    vk_unload();
    vk_platform_unload();
}

} // namespace

KV_MODULE_DEFINE(renderer_vulkan_module) = {
    .name     = "renderer-vulkan",
    .startup  = renderer_vulkan_startup,
    .shutdown = renderer_vulkan_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv
