// Korvid renderer-vulkan: the Vulkan 1.3 sprite renderer behind korvid/render/renderer.h.
// Dynamic rendering, synchronization2 and descriptor indexing; no render passes, no
// framebuffers. One bindless texture array, one instance buffer per frame in flight, one
// instanced draw per blend-mode batch, two frames in flight with fences. The only inputs
// are render packets; nothing here knows about entities, components or the window pump.
// Textures and materials are pools whose index is the GPU index: a texture's pool slot is
// its bindless slot, a material's is its row. The outer engine holds the handles.
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/renderer-vulkan/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/memory.h"
#include "korvid/core/pool.h"
#include "vk_funcs.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kFramesInFlight   = 2;
constexpr u32 kMaxTextures      = 1024; // bindless array size; slot 0 is the default texture
constexpr u32 kMaxMaterials     = 4096;
constexpr u32 kMaxSprites       = 1u << 20;
constexpr u32 kDefaultTexSize   = 16;   // checkerboard texels per side
constexpr u32 kDefaultTexCell   = 2;    // texels per checker cell
constexpr u32 kInitialInstances = 256;
constexpr u32 kBlendCount       = (u32)BlendMode::Count;

// GPU-side layouts. Mirrored by sprite.slang; std430 for the instance, std140 for the push.
struct GpuInstance {
    f32 model[4]; // a, b, c, d
    f32 trans[2]; // tx, ty
    f32 pad0[2];
    f32 uv[4];    // x, y, w, h
    f32 tint[4];
    u32 tex[4];   // x = texture slot
};
static_assert(sizeof(GpuInstance) == 80, "GpuInstance must match sprite.slang");

struct PushData {
    f32 view[4];
    f32 view_trans[2];
    u32 first_instance;
    u32 pad;
};
static_assert(sizeof(PushData) == 32, "PushData must match sprite.slang");

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr;
};

// A bindless slot. Its pool index is the slot number. No image yet means the slot
// shows the default texture.
struct Texture {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    u32            width  = 0, height = 0;
    TextureFormat  format = TextureFormat::RGBA8_SRGB;
    bool           resident = false;
    char           name[32];
};

// A material row. The texture's slot is read through the texture pool each frame so a
// destroyed texture falls back to the default rather than to whatever reused the slot.
struct MaterialRecord {
    Handle    texture;
    BlendMode blend;
    color     tint;
};

// A sprite in the render world: what the sim last said, and where it was the tick before.
struct RenderSprite {
    Handle            entity;
    Placement2        prev;
    RenderSpriteState cur;
};

// Something to destroy once the frame that last used it has passed its fence.
struct Garbage {
    enum Kind : u8 { BufferKind, ImageKind, ImageViewKind, MemoryKind } kind;
    u64 handle; // the Vulkan handle, widened
};

struct FrameInFlight {
    VkCommandPool   pool    = VK_NULL_HANDLE;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkFence         fence   = VK_NULL_HANDLE;
    VkSemaphore     acquire = VK_NULL_HANDLE; // swapchain image available
    VkDescriptorSet set     = VK_NULL_HANDLE; // binding 0 is this frame's instance buffer
    Buffer          instances;
    u32             instance_cap = 0;
    Array<Garbage>  garbage;                  // destroyed after this frame's fence
    Array<u32>      dirty_slots;              // texture slots to rewrite in this frame's set
};

struct Swapchain {
    VkSwapchainKHR     handle = VK_NULL_HANDLE;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkExtent2D         extent{0, 0};
    Array<VkImage>     images;
    Array<VkImageView> views;
    Array<VkSemaphore> present; // one "rendering done" semaphore per image
};

// One instance's sort key: layer, then blend, then slot order for determinism.
struct SortKey {
    i32 layer;
    u32 blend;
    u32 sprite; // render sprite slot
};

} // namespace

struct Renderer {
    Renderer**  owner_slot = nullptr; // the module state's pointer to this; nulled on destroy
    Allocator*  general    = nullptr; // the owning module's attributed general allocator
    SDL_Window* window     = nullptr;
    bool        validation = false;
    bool        vsync      = true;

    VkInstance               instance  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR             surface   = VK_NULL_HANDLE;
    VkPhysicalDevice         gpu       = VK_NULL_HANDLE;
    u32                      queue_family = 0;
    VkDevice                 device    = VK_NULL_HANDLE;
    VkQueue                  queue     = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props{};

    Swapchain swapchain;
    bool      swapchain_dirty = true;
    u32       swapchain_generation = 0;
    u32       want_width = 0, want_height = 0;

    VkDescriptorSetLayout set_layout  = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool   = VK_NULL_HANDLE;
    VkSampler             sampler     = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout = VK_NULL_HANDLE;
    VkShaderModule        shader      = VK_NULL_HANDLE;
    VkPipeline            pipelines[kBlendCount] = {};

    Pool<Texture>        textures;  // slot = index; 0 is the default checkerboard
    Pool<MaterialRecord> materials; // row = index
    Handle               default_material;
    u16                  texture_kind = 0, material_kind = 0;

    // The persistent render world: the outer engine's entity handle maps to a slot here.
    Pool<RenderSprite>   sprites;
    Map<Handle, u32>     sprite_of_entity;
    u64                  deltas_applied = 0;

    FrameInFlight frames[kFramesInFlight];
    // Uploads run on their own command buffer and fence, so they never collide with a
    // frame in flight. One at a time, waited on: slice 1 has no transfer queue.
    VkCommandPool   upload_pool  = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd   = VK_NULL_HANDLE;
    VkFence         upload_fence = VK_NULL_HANDLE;
    u32           frame_index = 0;
    u64           frame_count = 0;

    // Per-frame CPU data, in the calling thread's scratch (frame lifetime).
    Span<SortKey>     keys;
    Span<GpuInstance> staging;

    u32 validation_errors   = 0;
    u32 validation_warnings = 0;
};

namespace {

#define VK_CHECK_RET(call, ret)                                                        \
    do {                                                                               \
        VkResult kv_r_ = (call);                                                       \
        if (kv_r_ != VK_SUCCESS) {                                                     \
            KV_ERROR("vulkan", "%s failed: %s (%s:%d)", #call, vk_result_name(kv_r_),   \
                     __FILE__, __LINE__);                                              \
            return ret;                                                                \
        }                                                                              \
    } while (0)
#define VK_CHECK(call) VK_CHECK_RET(call, false)

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

bool create_instance(Renderer* r) {
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

bool create_device(Renderer* r) {
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
    return true;
}

// ---- memory, buffers, images -----------------------------------------------------

u32 find_memory_type(Renderer* r, u32 type_bits, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < r->mem_props.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (r->mem_props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

void destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    if (b.buffer) vkDestroyBuffer(r->device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(r->device, b.memory, nullptr);
    b = Buffer{};
}

// Deferred destruction: queued on the frame being recorded, run after its fence passes,
// so nothing the GPU may still read is freed early. The PRD's rule, as a queue.
void defer_destroy(Renderer* r, Garbage::Kind kind, u64 handle) {
    if (handle) r->frames[r->frame_index].garbage.push(Garbage{kind, handle});
}

void defer_destroy_buffer(Renderer* r, Buffer& b) {
    if (b.mapped) vkUnmapMemory(r->device, b.memory);
    defer_destroy(r, Garbage::BufferKind, (u64)b.buffer);
    defer_destroy(r, Garbage::MemoryKind, (u64)b.memory);
    b = Buffer{};
}

void defer_destroy_texture(Renderer* r, Texture& t) {
    defer_destroy(r, Garbage::ImageViewKind, (u64)t.view);
    defer_destroy(r, Garbage::ImageKind, (u64)t.image);
    defer_destroy(r, Garbage::MemoryKind, (u64)t.memory);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

// A slot's contents changed. Each frame's set is rewritten once that frame's fence has
// passed, so no pending command buffer sees a descriptor change under it.
void mark_slot_dirty(Renderer* r, u32 slot) {
    for (FrameInFlight& f : r->frames) f.dirty_slots.push(slot);
}

void run_garbage(Renderer* r, FrameInFlight& f) {
    for (const Garbage& g : f.garbage) {
        switch (g.kind) {
        case Garbage::BufferKind:    vkDestroyBuffer(r->device, (VkBuffer)g.handle, nullptr); break;
        case Garbage::ImageKind:     vkDestroyImage(r->device, (VkImage)g.handle, nullptr); break;
        case Garbage::ImageViewKind: vkDestroyImageView(r->device, (VkImageView)g.handle, nullptr); break;
        case Garbage::MemoryKind:    vkFreeMemory(r->device, (VkDeviceMemory)g.handle, nullptr); break;
        }
    }
    f.garbage.clear();
}

// Host-visible, coherent, persistently mapped.
bool create_host_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out) {
    VkBufferCreateInfo ci{};
    ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size        = size;
    ci.usage       = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(r->device, &ci, nullptr, &out.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, out.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(r, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no host-visible memory type"); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &ai, nullptr, &out.memory));
    VK_CHECK(vkBindBufferMemory(r->device, out.buffer, out.memory, 0));
    VK_CHECK(vkMapMemory(r->device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped));
    out.size = size;
    return true;
}

void destroy_texture_now(Renderer* r, Texture& t) {
    if (t.view)   vkDestroyImageView(r->device, t.view, nullptr);
    if (t.image)  vkDestroyImage(r->device, t.image, nullptr);
    if (t.memory) vkFreeMemory(r->device, t.memory, nullptr);
    t.view = VK_NULL_HANDLE; t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
    t.resident = false;
}

// Runs recorded commands on the upload command buffer and waits for its fence. Textures
// are resident before the next frame that references them. Frames in flight are not
// disturbed: the upload has its own buffer and the queue serializes the work.
bool submit_once(Renderer* r, bool (*record)(Renderer*, VkCommandBuffer, void*), void* user) {
    VK_CHECK(vkWaitForFences(r->device, 1, &r->upload_fence, VK_TRUE, UINT64_MAX)); // the previous upload
    VK_CHECK(vkResetFences(r->device, 1, &r->upload_fence));
    VK_CHECK(vkResetCommandPool(r->device, r->upload_pool, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(r->upload_cmd, &bi));
    if (!record(r, r->upload_cmd, user)) return false;
    VK_CHECK(vkEndCommandBuffer(r->upload_cmd));

    VkCommandBufferSubmitInfo cbi{};
    cbi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = r->upload_cmd;
    VkSubmitInfo2 si{};
    si.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cbi;
    VK_CHECK(vkQueueSubmit2(r->queue, 1, &si, r->upload_fence));
    VK_CHECK(vkWaitForFences(r->device, 1, &r->upload_fence, VK_TRUE, UINT64_MAX));
    return true;
}

struct UploadJob {
    Texture* tex;
    Buffer*  staging;
    u32      width, height;
};

bool record_texture_upload(Renderer*, VkCommandBuffer cmd, void* user) {
    UploadJob& job = *(UploadJob*)user;

    VkImageMemoryBarrier2 to_dst{};
    to_dst.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_dst.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
    to_dst.srcAccessMask    = 0;
    to_dst.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_dst.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_dst.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image            = job.tex->image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_dst;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {job.width, job.height, 1};
    vkCmdCopyBufferToImage(cmd, job.staging->buffer, job.tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 to_read = to_dst;
    to_read.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

VkFormat vk_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
}

// The view a slot should show right now: its own image, or the default texture's.
VkImageView slot_view(Renderer* r, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (t && t->view) return t->view;
    return r->textures.items[0].view;
}

void write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot) {
    VkDescriptorImageInfo info{};
    info.imageView   = slot_view(r, slot);
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 2;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo      = &info;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, nullptr);
}

// Creates the GPU image for a texture record and uploads tightly packed pixels through a
// staging buffer. Any previous image is destroyed after the frames in flight.
bool upload_texture(Renderer* r, Texture& t, const u8* pixels) {
    if (t.image) defer_destroy_texture(r, t);

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vk_format(t.format);
    ici.extent        = {t.width, t.height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r->device, &ici, nullptr, &t.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, t.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { KV_ERROR("vulkan", "no device-local memory type"); return false; }
    VK_CHECK(vkAllocateMemory(r->device, &ai, nullptr, &t.memory));
    VK_CHECK(vkBindImageMemory(r->device, t.image, t.memory, 0));

    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = t.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r->device, &vci, nullptr, &t.view));

    Buffer staging;
    VkDeviceSize bytes = (VkDeviceSize)t.width * t.height * 4;
    if (!create_host_buffer(r, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) return false;
    memcpy(staging.mapped, pixels, (usize)bytes);
    UploadJob job{&t, &staging, t.width, t.height};
    bool ok = submit_once(r, record_texture_upload, &job); // waits: resident before the next frame
    destroy_buffer(r, staging);
    if (!ok) return false;
    t.resident = true;
    return true;
}

// The hardcoded default: a red and white checkerboard at slot 0; what a stale or null
// texture reference shows, and what a pending texture shows until its upload.
bool create_default_texture(Renderer* r) {
    Texture t{};
    t.width = t.height = kDefaultTexSize;
    t.format = TextureFormat::RGBA8_SRGB;
    strncpy(t.name, "default", sizeof t.name - 1);
    Handle h = r->textures.create(t);
    if (h.index() != 0) { KV_ERROR("vulkan", "default texture is not slot 0"); return false; }

    u8 pixels[kDefaultTexSize * kDefaultTexSize * 4];
    for (u32 y = 0; y < kDefaultTexSize; ++y) {
        for (u32 x = 0; x < kDefaultTexSize; ++x) {
            bool red = ((x / kDefaultTexCell) + (y / kDefaultTexCell)) & 1;
            u8* p = pixels + (y * kDefaultTexSize + x) * 4;
            p[0] = 255;
            p[1] = red ? 0 : 255;
            p[2] = red ? 0 : 255;
            p[3] = 255;
        }
    }
    if (!upload_texture(r, r->textures.items[0], pixels)) return false;
    // Nothing is pending yet: every slot of every set can point at the default now.
    for (FrameInFlight& f : r->frames)
        for (u32 slot = 0; slot < kMaxTextures; ++slot) write_texture_slot(r, f.set, slot);

    MaterialRecord m{};
    m.texture = h;
    m.blend   = BlendMode::Alpha;
    m.tint    = color_white();
    r->default_material = r->materials.create(m);
    return !r->default_material.is_null();
}

// ---- handle kinds -------------------------------------------------------------------

void* texture_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->textures.get(Handle{h}); }
int texture_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    Texture* t = r->textures.get(Handle{h});
    if (!t) return snprintf(out, cap, "stale");
    return snprintf(out, cap, "'%s' %ux%u slot %u %s", t->name, t->width, t->height, Handle{h}.index(),
                    t->resident ? "resident" : "placeholder");
}
void* material_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    MaterialRecord* m = r->materials.get(Handle{h});
    if (!m) return snprintf(out, cap, "stale");
    const char* blend = m->blend == BlendMode::Opaque ? "opaque" : m->blend == BlendMode::Additive ? "additive" : "alpha";
    return snprintf(out, cap, "%s texture slot %u", blend, renderer_texture_slot(r, m->texture));
}

bool create_resource_pools(Renderer* r) {
    kv_handle_kind_desc td{};
    td.name = "render.texture"; td.type_key = type_key<Texture>(); td.ctx = r;
    td.resolve = texture_resolve; td.describe = texture_describe; td.flags = KV_HANDLE_GENERATIONAL;
    r->texture_kind = handle_kind_register(td);
    kv_handle_kind_desc md{};
    md.name = "render.material"; md.type_key = type_key<MaterialRecord>(); md.ctx = r;
    md.resolve = material_resolve; md.describe = material_describe; md.flags = KV_HANDLE_GENERATIONAL;
    r->material_kind = handle_kind_register(md);
    if (!r->texture_kind || !r->material_kind) return false;
    r->textures.use_kind(r->texture_kind);
    r->materials.use_kind(r->material_kind);
    r->sprite_of_entity.init(r->general);
    return r->textures.init(kMaxTextures, r->general->tag) && r->materials.init(kMaxMaterials, r->general->tag) &&
           r->sprites.init(kMaxSprites, r->general->tag);
}

// ---- descriptors, pipeline -----------------------------------------------------

// Points a frame's set at its instance buffer. Called at creation and after a regrow, both
// times with the frame's fence already waited on, so no pending work reads the set.
void write_instance_descriptor(Renderer* r, const FrameInFlight& f) {
    VkDescriptorBufferInfo info{f.instances.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = f.set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &info;
    vkUpdateDescriptorSets(r->device, 1, &write, 0, nullptr);
}

// One set per frame in flight: binding 0 is that frame's instance buffer, the sampler and
// the texture array are written identically into every set. Binding numbers match
// sprite.slang; the variable-count texture array must be the highest binding.
bool create_descriptors(Renderer* r) {
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0; // instance buffer
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding         = 1; // sampler
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2; // bindless textures
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[2].descriptorCount = kMaxTextures;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags flags[3] = {
        0,
        0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo fci{};
    fci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    fci.bindingCount  = 3;
    fci.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext        = &fci;
    lci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 3;
    lci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(r->device, &lci, nullptr, &r->set_layout));

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures * kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets       = kFramesInFlight;
    pci.poolSizeCount = 3;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(r->device, &pci, nullptr, &r->desc_pool));

    u32 variable_counts[kFramesInFlight];
    VkDescriptorSetLayout layouts[kFramesInFlight];
    VkDescriptorSet sets[kFramesInFlight];
    for (u32 i = 0; i < kFramesInFlight; ++i) { variable_counts[i] = kMaxTextures; layouts[i] = r->set_layout; }
    VkDescriptorSetVariableDescriptorCountAllocateInfo vai{};
    vai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    vai.descriptorSetCount = kFramesInFlight;
    vai.pDescriptorCounts  = variable_counts;
    VkDescriptorSetAllocateInfo dai{};
    dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.pNext              = &vai;
    dai.descriptorPool     = r->desc_pool;
    dai.descriptorSetCount = kFramesInFlight;
    dai.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(r->device, &dai, sets));
    for (u32 i = 0; i < kFramesInFlight; ++i) r->frames[i].set = sets[i];

    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // uv outside [0, 1] tiles
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod       = 1.0f;
    VK_CHECK(vkCreateSampler(r->device, &sci, nullptr, &r->sampler));

    VkDescriptorImageInfo sinfo{};
    sinfo.sampler = r->sampler;
    VkWriteDescriptorSet writes[kFramesInFlight]{};
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = r->frames[i].set;
        writes[i].dstBinding      = 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[i].pImageInfo      = &sinfo;
    }
    vkUpdateDescriptorSets(r->device, kFramesInFlight, writes, 0, nullptr);
    for (u32 i = 0; i < kFramesInFlight; ++i) write_instance_descriptor(r, r->frames[i]);

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushData)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &r->set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &range;
    VK_CHECK(vkCreatePipelineLayout(r->device, &plci, nullptr, &r->pipe_layout));
    return true;
}

// Reads <exe dir>/shaders/<name>. SPIR-V is precompiled by the build; nothing compiles here.
bool load_shader(Renderer* r, const char* name) {
    const char* base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sshaders/%s", base ? base : "", name);
    FILE* f = fopen(path, "rb");
    if (!f) { KV_ERROR("vulkan", "cannot open shader '%s'", path); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { fclose(f); KV_ERROR("vulkan", "bad SPIR-V size in '%s'", path); return false; }
    MemScope scope; // the words are needed only until the module is created
    u32* words = arena_push_n<u32>(*scope.arena, (u32)(len / 4));
    bool ok = words && fread(words, 1, (usize)len, f) == (usize)len;
    fclose(f);
    if (!ok) { KV_ERROR("vulkan", "short read on '%s'", path); return false; }

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (usize)len;
    ci.pCode    = words;
    VkResult res = vkCreateShaderModule(r->device, &ci, nullptr, &r->shader);
    if (res != VK_SUCCESS) { KV_ERROR("vulkan", "vkCreateShaderModule: %s", vk_result_name(res)); return false; }
    KV_INFO("vulkan", "shader '%s' loaded (%ld bytes)", name, len);
    return true;
}

VkPipelineColorBlendAttachmentState blend_state(BlendMode mode) {
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (mode) {
    case BlendMode::Opaque:
        s.blendEnable = VK_FALSE;
        break;
    case BlendMode::Alpha:
        s.blendEnable         = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.colorBlendOp        = VK_BLEND_OP_ADD;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.alphaBlendOp        = VK_BLEND_OP_ADD;
        break;
    case BlendMode::Additive:
        s.blendEnable         = VK_TRUE;
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.colorBlendOp        = VK_BLEND_OP_ADD;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.alphaBlendOp        = VK_BLEND_OP_ADD;
        break;
    default:
        break;
    }
    return s;
}

// One pipeline per blend mode; everything else is shared. Needs the swapchain format.
bool create_pipelines(Renderer* r) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r->shader;
    stages[0].pName  = "vs_main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r->shader;
    stages[1].pName  = "fs_main";

    VkPipelineVertexInputStateCreateInfo vertex_input{}; // no vertex buffers at all
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = KV_COUNTOF(dynamic);
    dynamic_state.pDynamicStates    = dynamic;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &r->swapchain.format;

    for (u32 i = 0; i < kBlendCount; ++i) {
        VkPipelineColorBlendAttachmentState attachment = blend_state((BlendMode)i);
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &attachment;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext               = &rendering;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertex_input;
        ci.pInputAssemblyState = &assembly;
        ci.pViewportState      = &viewport;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState   = &multisample;
        ci.pColorBlendState    = &blend;
        ci.pDynamicState       = &dynamic_state;
        ci.layout              = r->pipe_layout;
        VK_CHECK(vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &ci, nullptr, &r->pipelines[i]));
    }
    return true;
}

void destroy_pipelines(Renderer* r) {
    for (u32 i = 0; i < kBlendCount; ++i) {
        if (r->pipelines[i]) vkDestroyPipeline(r->device, r->pipelines[i], nullptr);
        r->pipelines[i] = VK_NULL_HANDLE;
    }
}

// ---- swapchain -------------------------------------------------------------------

void destroy_swapchain(Renderer* r) {
    Swapchain& s = r->swapchain;
    for (VkSemaphore sem : s.present) vkDestroySemaphore(r->device, sem, nullptr);
    for (VkImageView v : s.views) vkDestroyImageView(r->device, v, nullptr);
    if (s.handle) vkDestroySwapchainKHR(r->device, s.handle, nullptr);
    s.present.free();
    s.views.free();
    s.images.free();
    s.handle = VK_NULL_HANDLE;
    s.extent = {0, 0};
}

// Builds the swapchain for the current surface size. Returns true with a zero extent when
// the window has no area (minimized); the caller then skips the frame.
bool create_swapchain(Renderer* r) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->gpu, r->surface, &caps));

    // Wayland leaves currentExtent undefined; the window's pixel size decides then.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width  = r->want_width;
        extent.height = r->want_height;
    }
    extent.width  = extent.width  < caps.minImageExtent.width  ? caps.minImageExtent.width  :
                    extent.width  > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : extent.width;
    extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height :
                    extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
    if (extent.width == 0 || extent.height == 0) {
        destroy_swapchain(r);
        return true;
    }

    u32 fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, r->surface, &fmt_count, nullptr);
    Array<VkSurfaceFormatKHR> formats;
    formats.push_n(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r->gpu, r->surface, &fmt_count, formats.data);
    VkSurfaceFormatKHR chosen = formats[0];
    for (u32 i = 0; i < fmt_count; ++i) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = formats[i]; break; }
    }
    formats.free();

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, r->surface, &mode_count, nullptr);
    Array<VkPresentModeKHR> modes;
    modes.push_n(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->gpu, r->surface, &mode_count, modes.data);
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // always available
    if (!r->vsync) {
        for (u32 i = 0; i < mode_count; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) present_mode = modes[i];
        if (present_mode == VK_PRESENT_MODE_FIFO_KHR)
            for (u32 i = 0; i < mode_count; ++i)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) present_mode = modes[i];
    }
    modes.free();

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    VkSwapchainKHR old = r->swapchain.handle;
    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = r->surface;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen.format;
    ci.imageColorSpace  = chosen.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(r->device, &ci, nullptr, &sc);
    destroy_swapchain(r); // drops the old one and its views/semaphores
    if (res != VK_SUCCESS) {
        KV_ERROR("vulkan", "vkCreateSwapchainKHR: %s", vk_result_name(res));
        return false;
    }
    Swapchain& s = r->swapchain;
    s.handle = sc;
    s.format = chosen.format;
    s.extent = extent;

    u32 n = 0;
    vkGetSwapchainImagesKHR(r->device, sc, &n, nullptr);
    s.images.push_n(n);
    vkGetSwapchainImagesKHR(r->device, sc, &n, s.images.data);
    for (u32 i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image            = s.images[i];
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = s.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK_CHECK(vkCreateImageView(r->device, &vci, nullptr, &view));
        s.views.push(view);

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore sem;
        VK_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &sem));
        s.present.push(sem);
    }
    // A live resize rebuilds this every frame; only the first one is worth an INFO line.
    log_write(r->swapchain_generation++ == 0 ? LogLevel::Info : LogLevel::Debug, "vulkan",
              "swapchain %ux%u, %u images, format %d, %s", extent.width, extent.height, n,
              (int)s.format, present_mode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" :
              present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
    return true;
}

// ---- frames in flight ----------------------------------------------------------

bool create_frames(Renderer* r) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        FrameInFlight& f = r->frames[i];
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = r->queue_family;
        VK_CHECK(vkCreateCommandPool(r->device, &pci, nullptr, &f.pool));

        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = f.pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(r->device, &cai, &f.cmd));

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the first wait passes
        VK_CHECK(vkCreateFence(r->device, &fci, nullptr, &f.fence));

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(r->device, &sci, nullptr, &f.acquire));

        if (!create_host_buffer(r, sizeof(GpuInstance) * kInitialInstances,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, f.instances)) return false;
        f.instance_cap = kInitialInstances;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = r->queue_family;
    VK_CHECK(vkCreateCommandPool(r->device, &pci, nullptr, &r->upload_pool));
    VkCommandBufferAllocateInfo cai{};
    cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool        = r->upload_pool;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(r->device, &cai, &r->upload_cmd));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(r->device, &fci, nullptr, &r->upload_fence));
    return true;
}

void destroy_frames(Renderer* r) {
    if (r->upload_fence) vkDestroyFence(r->device, r->upload_fence, nullptr);
    if (r->upload_pool)  vkDestroyCommandPool(r->device, r->upload_pool, nullptr);
    r->upload_fence = VK_NULL_HANDLE; r->upload_pool = VK_NULL_HANDLE; r->upload_cmd = VK_NULL_HANDLE;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        FrameInFlight& f = r->frames[i];
        run_garbage(r, f); // the device is idle here
        f.garbage.free();
        f.dirty_slots.free();
        destroy_buffer(r, f.instances);
        if (f.acquire) vkDestroySemaphore(r->device, f.acquire, nullptr);
        if (f.fence)   vkDestroyFence(r->device, f.fence, nullptr);
        if (f.pool)    vkDestroyCommandPool(r->device, f.pool, nullptr); // frees the command buffer
        f = FrameInFlight{};
    }
}

// Grows a frame's instance buffer. The old one is queued for destruction after this
// frame's fence rather than freed now, so the growth does not depend on where in the
// frame it happens.
bool ensure_instance_capacity(Renderer* r, FrameInFlight& f, u32 needed) {
    if (needed <= f.instance_cap) return true;
    u32 cap = f.instance_cap ? f.instance_cap : kInitialInstances;
    while (cap < needed) cap *= 2;
    defer_destroy_buffer(r, f.instances);
    if (!create_host_buffer(r, sizeof(GpuInstance) * cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, f.instances))
        return false;
    f.instance_cap = cap;
    write_instance_descriptor(r, f);
    return true;
}

// ---- packets to instances -------------------------------------------------------

// A sprite's material row, or the default's when the handle is null or stale.
const MaterialRecord& sprite_material(Renderer* r, const RenderSpriteState& s) {
    const MaterialRecord* m = r->materials.get(s.material);
    return m ? *m : *r->materials.get(r->default_material);
}

int compare_keys(const void* a, const void* b) {
    const SortKey& x = *(const SortKey*)a;
    const SortKey& y = *(const SortKey*)b;
    if (x.layer != y.layer) return x.layer < y.layer ? -1 : 1;
    if (x.blend != y.blend) return x.blend < y.blend ? -1 : 1;
    return x.sprite < y.sprite ? -1 : (x.sprite > y.sprite ? 1 : 0);
}

// Walks the render world, sorts by layer then blend mode, interpolates by alpha and
// writes GPU instances in order. Keys and staging are scratch: they live until the next
// frame's engine_frame.
void build_instances(Renderer* r, const RenderView& view) {
    Arena& scratch = *mem_scratch_arena();
    u32 n = r->sprites.live;
    r->keys    = Span<SortKey>(arena_push_n<SortKey>(scratch, n), n);
    r->staging = Span<GpuInstance>(arena_push_n<GpuInstance>(scratch, n), n);
    u32 kn = 0;
    for (u32 i = 0; i < r->sprites.len && kn < n; ++i) {
        if (!r->sprites.is_alive(i)) continue;
        const RenderSprite& s = r->sprites.items[i];
        r->keys[kn++] = SortKey{s.cur.layer, (u32)sprite_material(r, s.cur).blend, i};
    }
    if (n > 1) qsort(r->keys.data, n, sizeof(SortKey), compare_keys);

    u32 out = 0;
    for (const SortKey& k : r->keys) {
        const RenderSprite& s = r->sprites.items[k.sprite];
        const RenderSpriteState& p = s.cur;
        const MaterialRecord& m = sprite_material(r, p);
        Placement2 at = lerp(s.prev, p.placement, view.alpha);
        affine2 model = affine2_trs(at.position, at.rotation, at.scale * p.size);
        color tint = p.tint * m.tint;
        GpuInstance g;
        g.model[0] = model.a; g.model[1] = model.b; g.model[2] = model.c; g.model[3] = model.d;
        g.trans[0] = model.tx; g.trans[1] = model.ty;
        g.pad0[0] = g.pad0[1] = 0.0f;
        g.uv[0] = p.uv.x; g.uv[1] = p.uv.y; g.uv[2] = p.uv.w; g.uv[3] = p.uv.h;
        g.tint[0] = tint.r; g.tint[1] = tint.g; g.tint[2] = tint.b; g.tint[3] = tint.a;
        g.tex[0] = renderer_texture_slot(r, m.texture);
        g.tex[1] = g.tex[2] = g.tex[3] = 0;
        r->staging[out++] = g;
    }
}

// ---- the frame -----------------------------------------------------------------

bool record_frame(Renderer* r, FrameInFlight& f, u32 image_index, const RenderView& view) {
    Swapchain& s = r->swapchain;
    VkCommandBuffer cmd = f.cmd;

    VK_CHECK(vkResetCommandPool(r->device, f.pool, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // Undefined -> color attachment. Waiting on the acquire semaphore happens at
    // COLOR_ATTACHMENT_OUTPUT, so that is where the barrier starts too.
    VkImageMemoryBarrier2 to_color{};
    to_color.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_color.srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color.srcAccessMask       = 0;
    to_color.dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color.dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color.image               = s.images[image_index];
    to_color.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_color;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = s.views[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{view.clear_color.r, view.clear_color.g, view.clear_color.b, view.clear_color.a}};
    VkRenderingInfo ri{};
    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea           = {{0, 0}, s.extent};
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{0.0f, 0.0f, (f32)s.extent.width, (f32)s.extent.height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, s.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe_layout, 0, 1, &f.set, 0, nullptr);

    affine2 proj = ortho2d(view.view);
    PushData push{};
    push.view[0] = proj.a; push.view[1] = proj.b; push.view[2] = proj.c; push.view[3] = proj.d;
    push.view_trans[0] = proj.tx; push.view_trans[1] = proj.ty;

    // One instanced draw per run of equal blend mode (the sort put layers in order already).
    u32 start = 0;
    while (start < r->keys.len) {
        u32 blend = r->keys[start].blend;
        u32 end = start + 1;
        while (end < r->keys.len && r->keys[end].blend == blend) ++end;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipelines[blend]);
        push.first_instance = start;
        vkCmdPushConstants(cmd, r->pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
        vkCmdDraw(cmd, 6, end - start, 0, 0);
        start = end;
    }

    vkCmdEndRendering(cmd);

    VkImageMemoryBarrier2 to_present = to_color;
    to_present.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = 0;
    to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dep.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(cmd, &dep);

    VK_CHECK(vkEndCommandBuffer(cmd));
    return true;
}

bool recreate_swapchain(Renderer* r) {
    vkDeviceWaitIdle(r->device);
    VkFormat before = r->swapchain.format;
    if (!create_swapchain(r)) return false;
    r->swapchain_dirty = false;
    if (r->swapchain.handle && (before != r->swapchain.format || !r->pipelines[0])) {
        destroy_pipelines(r);
        if (!create_pipelines(r)) return false;
    }
    return true;
}

} // namespace

// ---- public API ---------------------------------------------------------------

Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    RendererVulkanState* owner = engine_module_state_as<RendererVulkanState>(engine, "renderer-vulkan");
    if (!owner || !vkGetInstanceProcAddr) {
        KV_ERROR("vulkan", "renderer-vulkan module is not started");
        return nullptr;
    }
    if (owner->renderer) {
        KV_ERROR("vulkan", "a renderer already exists");
        return nullptr;
    }
    Allocator* ga = owner->general;
    Renderer* r = (Renderer*)ga->alloc(ga, sizeof(Renderer), alignof(Renderer));
    *r = Renderer{}; // plain data: default member values, no constructor to run
    r->general      = ga;
    r->owner_slot   = &owner->renderer;
    owner->renderer = r;
    for (FrameInFlight& f : r->frames) { f.garbage.alloc = ga; f.dirty_slots.alloc = ga; }
    r->window      = (SDL_Window*)desc.native_window;
    r->validation  = desc.validation;
    r->vsync       = desc.vsync;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = create_instance(r);
    if (ok && !SDL_Vulkan_CreateSurface(r->window, r->instance, nullptr, &r->surface)) {
        KV_ERROR("vulkan", "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        ok = false;
    }
    ok = ok && create_device(r);
    ok = ok && create_frames(r);
    ok = ok && create_descriptors(r);
    ok = ok && load_shader(r, "sprite.spv");
    ok = ok && create_resource_pools(r);
    ok = ok && create_default_texture(r);
    ok = ok && recreate_swapchain(r); // also builds the pipelines, which need the format
    if (!ok) {
        renderer_destroy(r);
        return nullptr;
    }
    KV_INFO("vulkan", "renderer up: %u frames in flight, %u texture slots, validation %s",
            kFramesInFlight, kMaxTextures, r->validation ? "on" : "off");
    return r;
}

void renderer_destroy(Renderer* r) {
    if (!r) return;
    if (r->device) {
        vkDeviceWaitIdle(r->device);
        destroy_pipelines(r);
        destroy_swapchain(r);
        for (u32 i = 0; i < r->textures.len; ++i)
            if (r->textures.is_alive(i)) destroy_texture_now(r, r->textures.items[i]);
        if (r->shader)      vkDestroyShaderModule(r->device, r->shader, nullptr);
        if (r->pipe_layout) vkDestroyPipelineLayout(r->device, r->pipe_layout, nullptr);
        if (r->sampler)     vkDestroySampler(r->device, r->sampler, nullptr);
        if (r->desc_pool)   vkDestroyDescriptorPool(r->device, r->desc_pool, nullptr); // frees the set
        if (r->set_layout)  vkDestroyDescriptorSetLayout(r->device, r->set_layout, nullptr);
        destroy_frames(r);
        vkDestroyDevice(r->device, nullptr);
    }
    if (r->surface)   SDL_Vulkan_DestroySurface(r->instance, r->surface, nullptr);
    if (r->messenger) vkDestroyDebugUtilsMessengerEXT(r->instance, r->messenger, nullptr);
    if (r->instance)  vkDestroyInstance(r->instance, nullptr);
    KV_INFO("vulkan", "renderer down after %llu frames; %u validation errors, %u warnings",
            (unsigned long long)r->frame_count, r->validation_errors, r->validation_warnings);
    if (r->texture_kind)  kv_handle_kind_unregister(r->texture_kind);
    if (r->material_kind) kv_handle_kind_unregister(r->material_kind);
    r->textures.free();
    r->materials.free();
    r->sprites.free();
    r->sprite_of_entity.free();
    if (r->owner_slot) *r->owner_slot = nullptr;
    Allocator* ga = r->general;
    ga->free(ga, r, sizeof(Renderer));
}

Renderer* renderer_get(Engine& engine) {
    RendererVulkanState* s = engine_module_state_as<RendererVulkanState>(engine, "renderer-vulkan");
    return s ? s->renderer : nullptr;
}

RenderCaps renderer_caps(const Renderer*) {
    RenderCaps c;
    c.backend          = "vulkan";
    c.max_textures     = kMaxTextures;
    c.max_materials    = kMaxMaterials;
    c.frames_in_flight = kFramesInFlight;
    c.srgb_output      = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_SRGB]  = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_UNORM] = true;
    return c;
}

Handle renderer_texture_create(Renderer* r, const TextureDesc& desc) {
    if (!r || (u32)desc.format >= (u32)TextureFormat::Count) return Handle{};
    Texture t{};
    t.format = desc.format;
    strncpy(t.name, desc.name ? desc.name : "", sizeof t.name - 1);
    Handle h = r->textures.create(t);
    if (h.is_null()) { KV_ERROR("vulkan", "out of texture slots (%u)", kMaxTextures); return h; }
    mark_slot_dirty(r, h.index()); // shows the default until the upload lands
    return h;
}

bool renderer_texture_upload(Renderer* r, Handle texture, u32 width, u32 height, const void* pixels, usize bytes) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0 || !pixels) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || bytes != (usize)width * height * 4) {
        KV_ERROR("vulkan", "texture '%s': %ux%u with %llu bytes is not a tightly packed RGBA8 image",
                 t->name, width, height, (unsigned long long)bytes);
        return false;
    }
    t->width  = width;
    t->height = height;
    if (!upload_texture(r, *t, (const u8*)pixels)) return false;
    mark_slot_dirty(r, texture.index());
    return true;
}

void renderer_texture_destroy(Renderer* r, Handle texture) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0) return; // the default stays
    defer_destroy_texture(r, *t);
    u32 slot = texture.index();
    r->textures.destroy(texture);
    mark_slot_dirty(r, slot); // back to the default, in each set once its frame has passed
}

u32 renderer_texture_slot(const Renderer* r, Handle texture) {
    return r && r->textures.valid(texture) ? texture.index() : 0;
}

Handle renderer_material_create(Renderer* r, const MaterialDesc& desc) {
    if (!r) return Handle{};
    MaterialRecord m{};
    m.texture = desc.texture;
    m.blend   = (u32)desc.blend < (u32)BlendMode::Count ? desc.blend : BlendMode::Alpha;
    m.tint    = desc.tint;
    Handle h = r->materials.create(m);
    if (h.is_null()) KV_ERROR("vulkan", "out of materials (%u)", kMaxMaterials);
    return h;
}

bool renderer_material_set(Renderer* r, Handle material, const MaterialDesc& desc) {
    MaterialRecord* m = r ? r->materials.get(material) : nullptr;
    if (!m) return false;
    m->texture = desc.texture;
    m->blend   = (u32)desc.blend < (u32)BlendMode::Count ? desc.blend : BlendMode::Alpha;
    m->tint    = desc.tint;
    return true;
}

void renderer_material_destroy(Renderer* r, Handle material) {
    if (r && material != r->default_material) r->materials.destroy(material);
}

Handle renderer_default_material(const Renderer* r) { return r ? r->default_material : Handle{}; }

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width      = width;
    r->want_height     = height;
    r->swapchain_dirty = true;
}

void renderer_apply(Renderer* r, Span<const SpriteDelta> deltas) {
    if (!r) return;
    for (const SpriteDelta& d : deltas) {
        ++r->deltas_applied;
        switch (d.kind) {
        case SpriteDeltaKind::Tick:
            // A new tick: where everything is now is where it was.
            for (u32 i = 0; i < r->sprites.len; ++i)
                if (r->sprites.is_alive(i)) r->sprites.items[i].prev = r->sprites.items[i].cur.placement;
            break;
        case SpriteDeltaKind::Set: {
            if (u32* slot = r->sprite_of_entity.find(d.entity)) {
                r->sprites.items[*slot].cur = d.state;
            } else {
                RenderSprite s{};
                s.entity = d.entity;
                s.prev   = d.state.placement; // born here: no history, no interpolation jump
                s.cur    = d.state;
                Handle h = r->sprites.create(s);
                if (h.is_null()) { KV_ERROR("vulkan", "render world is full (%u sprites)", kMaxSprites); break; }
                r->sprite_of_entity.set(d.entity, h.index());
            }
            break;
        }
        case SpriteDeltaKind::Remove: {
            if (u32* slot = r->sprite_of_entity.find(d.entity)) {
                r->sprites.destroy(r->sprites.handle_at(*slot));
                r->sprite_of_entity.remove(d.entity);
            }
            break;
        }
        }
    }
}

u32 renderer_sprite_count(const Renderer* r) { return r ? r->sprites.live : 0; }

bool renderer_render(Renderer* r, const RenderView& view) {
    if (!r) return false;
    FrameInFlight& f = r->frames[r->frame_index];

    // Wait for this slot's previous frame; its buffers are free to reuse after this,
    // and whatever that frame queued for destruction can go.
    VK_CHECK(vkWaitForFences(r->device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    run_garbage(r, f);
    for (u32 slot : f.dirty_slots) write_texture_slot(r, f.set, slot);
    f.dirty_slots.clear();

    if (r->swapchain_dirty && !recreate_swapchain(r)) return false;
    if (!r->swapchain.handle) return false; // zero-size window

    u32 image_index = 0;
    VkResult acq = vkAcquireNextImageKHR(r->device, r->swapchain.handle, UINT64_MAX, f.acquire,
                                         VK_NULL_HANDLE, &image_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        r->swapchain_dirty = true;
        return false;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        KV_ERROR("vulkan", "vkAcquireNextImageKHR: %s", vk_result_name(acq));
        return false;
    }
    if (acq == VK_SUBOPTIMAL_KHR) r->swapchain_dirty = true; // rebuild next frame; draw this one

    build_instances(r, view);
    if (!ensure_instance_capacity(r, f, r->staging.len ? r->staging.len : 1)) return false;
    if (r->staging.len) memcpy(f.instances.mapped, r->staging.data, sizeof(GpuInstance) * r->staging.len);

    if (!record_frame(r, f, image_index, view)) return false;
    VK_CHECK(vkResetFences(r->device, 1, &f.fence)); // only once the submit is certain

    VkSemaphoreSubmitInfo wait{};
    wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = f.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{};
    signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = r->swapchain.present[image_index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{};
    cbi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{};
    si.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount   = 1;
    si.pWaitSemaphoreInfos      = &wait;
    si.commandBufferInfoCount   = 1;
    si.pCommandBufferInfos      = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos    = &signal;
    VK_CHECK(vkQueueSubmit2(r->queue, 1, &si, f.fence));

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &r->swapchain.present[image_index];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &r->swapchain.handle;
    pi.pImageIndices      = &image_index;
    VkResult pres = vkQueuePresentKHR(r->queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) r->swapchain_dirty = true;
    else if (pres != VK_SUCCESS) KV_ERROR("vulkan", "vkQueuePresentKHR: %s", vk_result_name(pres));

    r->frame_index = (r->frame_index + 1) % kFramesInFlight;
    ++r->frame_count;
    return true;
}

} // namespace kv
