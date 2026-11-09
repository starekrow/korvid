// Korvid renderer-vulkan: what differs per platform, which is little. The loader, the
// instance extensions a surface needs, and the surface itself, from the window's native
// handle: src/sdl3/ (through SDL, on Linux) and src/win32/ (vulkan-1.dll and an HWND).
#pragma once
#include "vk_funcs.h"
#include "korvid/core/types.h"

namespace kv {

// Finds and loads the Vulkan loader; its vkGetInstanceProcAddr, or null (logged).
PFN_vkGetInstanceProcAddr vk_platform_load();
void vk_platform_unload();
// What vkCreateInstance must enable for vk_platform_create_surface to work.
const char* const* vk_platform_instance_extensions(u32* count);
// A surface for HostWindow::native_handle(); false (logged) on failure. Destroyed with
// vkDestroySurfaceKHR.
bool vk_platform_create_surface(void* native_window, VkInstance instance, VkSurfaceKHR* out);

} // namespace kv
