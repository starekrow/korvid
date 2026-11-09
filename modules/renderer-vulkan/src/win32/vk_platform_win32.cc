// The Vulkan platform layer on Windows: the loader is vulkan-1.dll, which the GPU driver
// installs (the SDK is needed only to build); a surface is VK_KHR_win32_surface on the
// window's HWND. Presenting from the engine's thread to a window another thread owns is
// fine: Vulkan's Win32 surfaces carry no thread affinity.
#include "../vk_platform.h"
#include "korvid/core/log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>

namespace kv {

namespace {
HMODULE g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
} // namespace

PFN_vkGetInstanceProcAddr vk_platform_load() {
    if (g_gipa) return g_gipa;
    // From the system directory only: no vulkan-1.dll planted beside the executable or in
    // the working directory is loaded instead.
    g_loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_loader) {
        KV_ERROR("vulkan", "no Vulkan loader: vulkan-1.dll is not installed (error %lu); update the GPU driver",
                 (unsigned long)GetLastError());
        return nullptr;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(g_loader, "vkGetInstanceProcAddr");
    if (!g_gipa) {
        KV_ERROR("vulkan", "vulkan-1.dll has no vkGetInstanceProcAddr");
        FreeLibrary(g_loader);
        g_loader = nullptr;
    }
    return g_gipa;
}

void vk_platform_unload() {
    if (g_loader) FreeLibrary(g_loader);
    g_loader = nullptr;
    g_gipa = nullptr;
}

const char* const* vk_platform_instance_extensions(u32* count) {
    static const char* const k_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    *count = KV_COUNTOF(k_exts);
    return k_exts;
}

bool vk_platform_create_surface(void* native_window, VkInstance instance, VkSurfaceKHR* out) {
    PFN_vkCreateWin32SurfaceKHR create =
        g_gipa ? (PFN_vkCreateWin32SurfaceKHR)g_gipa(instance, "vkCreateWin32SurfaceKHR") : nullptr;
    if (!create) { KV_ERROR("vulkan", "vkCreateWin32SurfaceKHR is not available"); return false; }
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd      = (HWND)native_window;
    VkResult res = create(instance, &ci, nullptr, out);
    if (res == VK_SUCCESS) return true;
    KV_ERROR("vulkan", "vkCreateWin32SurfaceKHR: %s", vk_result_name(res));
    return false;
}

} // namespace kv
