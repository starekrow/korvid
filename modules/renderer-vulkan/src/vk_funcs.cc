#include "vk_funcs.h"
#include "korvid/core/log.h"

#define KV_VK_DEFINE(name) PFN_##name name = nullptr;
KV_VK_GLOBAL_FUNCS(KV_VK_DEFINE)
KV_VK_INSTANCE_FUNCS(KV_VK_DEFINE)
KV_VK_INSTANCE_DEBUG_FUNCS(KV_VK_DEFINE)
KV_VK_DEVICE_FUNCS(KV_VK_DEFINE)
#undef KV_VK_DEFINE

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

namespace kv {

bool vk_load_global(PFN_vkGetInstanceProcAddr gipa) {
    vkGetInstanceProcAddr = gipa;
    if (!gipa) return false;
    bool ok = true;
#define KV_VK_LOAD(name)                                                             \
    name = (PFN_##name)gipa(nullptr, #name);                                         \
    if (!name) { KV_ERROR("vulkan", "missing global entry point %s", #name); ok = false; }
    KV_VK_GLOBAL_FUNCS(KV_VK_LOAD)
#undef KV_VK_LOAD
    return ok;
}

bool vk_load_instance(VkInstance instance, bool debug_utils) {
    bool ok = true;
#define KV_VK_LOAD(name)                                                             \
    name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);                       \
    if (!name) { KV_ERROR("vulkan", "missing instance entry point %s", #name); ok = false; }
    KV_VK_INSTANCE_FUNCS(KV_VK_LOAD)
    if (debug_utils) { KV_VK_INSTANCE_DEBUG_FUNCS(KV_VK_LOAD) }
#undef KV_VK_LOAD
    return ok;
}

bool vk_load_device(VkDevice device) {
    bool ok = true;
#define KV_VK_LOAD(name)                                                             \
    name = (PFN_##name)vkGetDeviceProcAddr(device, #name);                           \
    if (!name) { KV_ERROR("vulkan", "missing device entry point %s", #name); ok = false; }
    KV_VK_DEVICE_FUNCS(KV_VK_LOAD)
#undef KV_VK_LOAD
    return ok;
}

void vk_unload() {
#define KV_VK_CLEAR(name) name = nullptr;
    KV_VK_GLOBAL_FUNCS(KV_VK_CLEAR)
    KV_VK_INSTANCE_FUNCS(KV_VK_CLEAR)
    KV_VK_INSTANCE_DEBUG_FUNCS(KV_VK_CLEAR)
    KV_VK_DEVICE_FUNCS(KV_VK_CLEAR)
#undef KV_VK_CLEAR
    vkGetInstanceProcAddr = nullptr;
}

const char* vk_result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_NOT_READY:                      return "VK_NOT_READY";
    case VK_TIMEOUT:                        return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
    default:                                return "VK_ERROR_(other)";
    }
}

} // namespace kv
