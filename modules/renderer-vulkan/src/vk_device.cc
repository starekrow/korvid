// Korvid renderer-vulkan: instance, debug messenger, physical device, device and queue.
#include "renderer_internal.h"
#include <string.h>

namespace kv {

namespace {

// ---- debug messenger ---------------------------------------------------------

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user) {
    Renderer* r = (Renderer*)user;
    const char* msg = data && data->pMessage ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++r->validation_errors;
        KV_ERROR("vulkan", "%s", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++r->validation_warnings;
        KV_WARN("vulkan", "%s", msg);
    } else {
        KV_DEBUG_LOG("vulkan", "%s", msg);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debug_messenger_info(Renderer* r) {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
    ci.pUserData       = r;
    return ci;
}


// ---- instance and device -------------------------------------------------------

bool has_layer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    Array<VkLayerProperties> props;
    props.push_n(n);
    vkEnumerateInstanceLayerProperties(&n, props.data);
    bool found = false;
    for (u32 i = 0; i < n && !found; ++i) found = strcmp(props[i].layerName, name) == 0;
    props.free();
    return found;
}

bool has_instance_extension(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    Array<VkExtensionProperties> props;
    props.push_n(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data);
    bool found = false;
    for (u32 i = 0; i < n && !found; ++i) found = strcmp(props[i].extensionName, name) == 0;
    props.free();
    return found;
}

} // namespace

bool rv_create_instance(Renderer* r) {
    u32 sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
        KV_ERROR("vulkan", "SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return false;
    }

    Array<const char*> exts;
    for (u32 i = 0; i < sdl_ext_count; ++i) exts.push(sdl_exts[i]);
    Array<const char*> layers;

    bool debug_utils = false;
    if (r->validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) layers.push("VK_LAYER_KHRONOS_validation");
        else KV_WARN("vulkan", "validation requested but VK_LAYER_KHRONOS_validation is not installed");
        if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            exts.push(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debug_utils = true;
        }
    }

    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "korvid";
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName        = "korvid";
    app.engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_3;

    // Synchronization validation is a layer setting (VK_EXT_layer_settings).
    const VkBool32 vk_true = VK_TRUE;
    VkLayerSettingEXT settings[] = {
        {"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &vk_true},
        {"VK_LAYER_KHRONOS_validation", "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &vk_true},
    };
    VkLayerSettingsCreateInfoEXT layer_settings{};
    layer_settings.sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layer_settings.settingCount = KV_COUNTOF(settings);
    layer_settings.pSettings    = settings;

    VkDebugUtilsMessengerCreateInfoEXT dbg = debug_messenger_info(r);
    if (layers.len) dbg.pNext = &layer_settings;

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pNext                   = debug_utils ? (const void*)&dbg : (layers.len ? (const void*)&layer_settings : nullptr);
    ci.pApplicationInfo        = &app;
    ci.enabledLayerCount       = layers.len;
    ci.ppEnabledLayerNames     = layers.data;
    ci.enabledExtensionCount   = exts.len;
    ci.ppEnabledExtensionNames = exts.data;

    VkResult res = vkCreateInstance(&ci, nullptr, &r->instance);
    exts.free();
    layers.free();
    if (res != VK_SUCCESS) {
        KV_ERROR("vulkan", "vkCreateInstance failed: %s", vk_result_name(res));
        return false;
    }
    if (!vk_load_instance(r->instance, debug_utils)) return false;

    if (debug_utils) {
        VkDebugUtilsMessengerCreateInfoEXT mi = debug_messenger_info(r);
        VK_CHECK(vkCreateDebugUtilsMessengerEXT(r->instance, &mi, nullptr, &r->messenger));
    }
    return true;
}

// Scores a device for our needs: 1.3 API, graphics+present queue, swapchain, features.

namespace {

// Returns -1 when unusable, higher is better. Fills queue_family.
int score_device(Renderer* r, VkPhysicalDevice gpu, u32* queue_family) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return -1;

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) return -1;
    if (!f2.features.drawIndirectFirstInstance) return -1; // indirect draws start at the bucket's offset
    if (!f11.shaderDrawParameters) return -1; // SV_InstanceID in the sprite shader
    if (!f12.descriptorIndexing || !f12.runtimeDescriptorArray ||
        !f12.shaderSampledImageArrayNonUniformIndexing ||
        !f12.descriptorBindingSampledImageUpdateAfterBind ||
        !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingVariableDescriptorCount)
        return -1;

    u32 ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    Array<VkExtensionProperties> exts;
    exts.push_n(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data);
    bool has_swapchain = false;
    for (u32 i = 0; i < ext_count; ++i)
        if (strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) has_swapchain = true;
    exts.free();
    if (!has_swapchain) return -1;

    u32 qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, nullptr);
    Array<VkQueueFamilyProperties> qfs;
    qfs.push_n(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, qfs.data);
    bool found = false;
    for (u32 i = 0; i < qf_count && !found; ++i) {
        if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, r->surface, &present);
        if (present) { *queue_family = i; found = true; }
    }
    qfs.free();
    if (!found) return -1;

    int score = 1;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)        score = 3;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 2;
    return score;
}

} // namespace

bool rv_create_device(Renderer* r) {
    u32 count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(r->instance, &count, nullptr));
    if (count == 0) { KV_ERROR("vulkan", "no Vulkan devices"); return false; }
    Array<VkPhysicalDevice> gpus;
    gpus.push_n(count);
    VK_CHECK(vkEnumeratePhysicalDevices(r->instance, &count, gpus.data));

    int best = -1;
    for (u32 i = 0; i < count; ++i) {
        u32 qf = 0;
        int s = score_device(r, gpus[i], &qf);
        if (s > best) { best = s; r->gpu = gpus[i]; r->queue_family = qf; }
    }
    gpus.free();
    if (best < 0) {
        KV_ERROR("vulkan", "no device supports Vulkan 1.3 with dynamic rendering, sync2, "
                 "descriptor indexing, draw parameters and presentation");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(r->gpu, &props);
    KV_INFO("vulkan", "device '%s', driver %u.%u.%u, API %u.%u.%u", props.deviceName,
            VK_API_VERSION_MAJOR(props.driverVersion), VK_API_VERSION_MINOR(props.driverVersion),
            VK_API_VERSION_PATCH(props.driverVersion), VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(r->gpu, &r->mem_props);

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext                                        = &f13;
    f12.descriptorIndexing                           = VK_TRUE;
    f12.runtimeDescriptorArray                       = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingPartiallyBound              = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount     = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.pNext                = &f12;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11;
    f2.features.drawIndirectFirstInstance = VK_TRUE;
    {
        VkPhysicalDeviceFeatures have;
        vkGetPhysicalDeviceFeatures(r->gpu, &have);
        r->multi_draw_indirect = have.multiDrawIndirect == VK_TRUE; // one call per run of buckets, else one per bucket
        f2.features.multiDrawIndirect = have.multiDrawIndirect;
    }

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = r->queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &f2;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = KV_COUNTOF(exts);
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(r->gpu, &ci, nullptr, &r->device));
    if (!vk_load_device(r->device)) return false;
    vkGetDeviceQueue(r->device, r->queue_family, 0, &r->queue);
    KV_INFO("vulkan", "multi-draw indirect %s", r->multi_draw_indirect ? "on" : "off");
    return true;
}

} // namespace kv
