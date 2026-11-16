// Korvid renderer-vulkan: what differs per platform, which is little. The loader, the
// instance extensions a surface needs, and the surface itself, from the window's native
// handle: src/sdl3/ (through SDL, on Linux) and src/win32/ (vulkan-1.dll and an HWND).
#pragma once
#include "vk_funcs.h"
#include "korvid/core/types.h"

namespace kv {

// Set by the module's start-up once it has a Vulkan 1.3 loader (renderer_probe reads it).
extern bool g_vulkan_ready;

// Finds and loads the Vulkan loader; its vkGetInstanceProcAddr, or null (logged).
PFN_vkGetInstanceProcAddr vk_platform_load();
void vk_platform_unload();
// What vkCreateInstance must enable for vk_platform_create_surface to work.
const char* const* vk_platform_instance_extensions(u32* count);
// A surface for HostWindow::native_handle(); false (logged) on failure. Destroyed with
// vkDestroySurfaceKHR.
bool vk_platform_create_surface(void* native_window, VkInstance instance, VkSurfaceKHR* out);
// Whether objects may be given debug names (vkSetDebugUtilsObjectNameEXT). True wherever
// the extension is offered, except in the Android emulator, whose guest driver offers it
// and then dereferences null inside it for a query pool or an image (emulator 37.2,
// vulkan.ranchu.so: vk_common_SetDebugUtilsObjectNameEXT).
#if defined(__ANDROID__)
bool vk_platform_object_names();
#else
inline bool vk_platform_object_names() { return true; }
#endif

} // namespace kv
