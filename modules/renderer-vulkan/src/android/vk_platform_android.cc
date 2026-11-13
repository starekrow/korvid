// The Vulkan platform layer on Android: the loader is the system's libvulkan.so, a surface
// is VK_KHR_android_surface on the activity's ANativeWindow (HostWindow::native_handle()).
// Android takes that window away when the app leaves the screen and gives a new one when it
// returns; the renderer drops its surface and makes another (renderer_set_surface).
#include "../vk_platform.h"
#include "korvid/core/log.h"
#include <android/native_window.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/system_properties.h>
#include <vulkan/vulkan_android.h>

namespace kv {

namespace {
void* g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
} // namespace

PFN_vkGetInstanceProcAddr vk_platform_load() {
    if (g_gipa) return g_gipa;
    g_loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
        KV_ERROR("vulkan", "no Vulkan loader: %s", dlerror());
        return nullptr;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_loader, "vkGetInstanceProcAddr");
    if (!g_gipa) {
        KV_ERROR("vulkan", "libvulkan.so has no vkGetInstanceProcAddr");
        dlclose(g_loader);
        g_loader = nullptr;
    }
    return g_gipa;
}

void vk_platform_unload() {
    if (g_loader) dlclose(g_loader);
    g_loader = nullptr;
    g_gipa = nullptr;
}

bool vk_platform_object_names() {
    static int emulator = -1;
    if (emulator < 0) {
        char value[PROP_VALUE_MAX] = "";
        __system_property_get("ro.boot.qemu", value);
        emulator = strcmp(value, "1") == 0 ? 1 : 0;
        if (emulator) KV_INFO("vulkan", "the Android emulator: no debug names on objects (its driver crashes setting them)");
    }
    return emulator == 0;
}

const char* const* vk_platform_instance_extensions(u32* count) {
    static const char* const k_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    *count = KV_COUNTOF(k_exts);
    return k_exts;
}

bool vk_platform_create_surface(void* native_window, VkInstance instance, VkSurfaceKHR* out) {
    PFN_vkCreateAndroidSurfaceKHR create =
        g_gipa ? (PFN_vkCreateAndroidSurfaceKHR)g_gipa(instance, "vkCreateAndroidSurfaceKHR") : nullptr;
    if (!create) { KV_ERROR("vulkan", "vkCreateAndroidSurfaceKHR is not available"); return false; }
    if (!native_window) { KV_ERROR("vulkan", "no window to make a surface for"); return false; }
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = (ANativeWindow*)native_window;
    VkResult res = create(instance, &ci, nullptr, out);
    if (res == VK_SUCCESS) return true;
    KV_ERROR("vulkan", "vkCreateAndroidSurfaceKHR: %s", vk_result_name(res));
    return false;
}

} // namespace kv
