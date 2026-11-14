// Korvid renderer-vulkan: what the renderer's source files share. The Renderer struct,
// the GPU-side layouts the shaders mirror, and the private functions each file provides:
//   vk_device.cc       instance, debug messenger, physical device, device and queue
//   vk_resources.cc    memory, buffers, the frame ring, garbage, textures, materials,
//                      the handle kinds and pools, descriptor layouts and sets
//   vk_swapchain.cc    swapchain, frames in flight, fenced recreation
//   vk_sprite_pass.cc  shaders, pipelines, and the frame's commands: uploads, the compute
//                      sort, the indirect draws
//   vk_post_pass.cc    the full-frame pass: the world's target, the post pipeline
//   renderer.cc        the public API behind korvid/render/renderer.h
// This header and the sources next to it are the only place a Vulkan header is included.
#pragma once
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/render/core/layout.h"

// What this backend tells the render core (korvid/render/core/core.h).
#define KV_RB_NAME       "vulkan"
#define KV_RB_CLIP_Y_UP  0
#define KV_RB_SHADER_EXT ".spv"
#define KV_RB_SURFACE    RendererSurface::Vulkan
#define KV_RB_FRAME_SCOPE 
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"
#include "vk_funcs.h"
#include "vk_platform.h"

namespace kv {

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr; // host-visible buffers only
};

// Something to destroy once the frame that last used it has passed its fence.
struct Garbage {
    enum Kind : u8 { BufferKind, ImageKind, ImageViewKind, MemoryKind, SwapchainKind, SemaphoreKind, PipelineKind } kind;
    u64 handle; // the Vulkan handle, widened
};

// What the render core holds of this backend's (korvid/render/core/core.h).
using RbBuffer  = Buffer;
using RbGarbage = Garbage;

} // namespace kv

#include "korvid/render/core/core.h"

namespace kv {

// ---- CPU-side records ---------------------------------------------------------------------

// A bindless slot. Its pool index is the slot number. No image yet means the slot
// shows the default texture.
struct Texture : TextureCore {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
};

// A game fragment shader with a blend: a pipeline slot of its own. Made when a material
// first names the pair; a shader that will not load or link is `failed`, and its slot draws
// with the engine's pipeline of its blend.
struct GamePipe : GamePipeCore {
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline     pipeline = VK_NULL_HANDLE;
};

// A staged texture upload, recorded into the next frame's command buffer.
struct PendingUpload {
    u32          slot;   // texture slot; skipped if the slot's image is no longer `image`
    VkImage      image;
    VkBuffer     src;    // the ring, or a one-off staging buffer already queued as garbage
    VkDeviceSize offset;
    u32          width, height;
};

struct FrameInFlight : FrameCore {
    VkCommandPool   pool    = VK_NULL_HANDLE;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkFence         fence   = VK_NULL_HANDLE;
    VkSemaphore     acquire = VK_NULL_HANDLE; // swapchain image available
    VkDescriptorSet tables  = VK_NULL_HANDLE; // set 0: sprite, material and sort buffers
    VkDescriptorSet textures = VK_NULL_HANDLE; // set 1: the sampler and the bindless array
    u32             image = 0;                  // the swapchain image this frame draws into (rb_frame_acquire)
    bool            push_stale = false;         // the post pass displaced the sprites' push block
    bool            tables_dirty = true;        // the table buffers changed: rewrite set 0 once the fence passes
    Buffer          layers;                     // the layers' offsets, previous and current: float4 each, host-visible
    // GPU timing (vk_gpu_profile.cc): whether this frame wrote its timestamps and its
    // statistics, when the CPU submitted it, and how many pixels it drew.
    bool            gpu_stamped = false, gpu_counted = false;
    u64             submit_ticks = 0;
    u64             gpu_pixels = 0;
};

struct GpuProfiler {
    bool            timestamps = false;    // the queue can, and this is a profiling build
    bool            statistics = false;    // pipelineStatisticsQuery
    bool            calibrated = false;    // VK_KHR (or EXT) calibrated timestamps, with the CPU clock's domain
    VkTimeDomainKHR cpu_domain = VK_TIME_DOMAIN_DEVICE_KHR;
    PFN_vkGetCalibratedTimestampsKHR get_calibrated = nullptr;
    f64             period_ns  = 1.0;      // a timestamp tick, in nanoseconds
    u64             valid_mask = ~0ull;    // the queue's timestampValidBits
    VkQueryPool     stamps = VK_NULL_HANDLE; // kGpuMarks per frame in flight
    VkQueryPool     stats  = VK_NULL_HANDLE; // one per frame in flight
    u64             cal_gpu = 0, cal_cpu = 0; // a GPU timestamp and the CPU's ticks at the same moment
    u32             cal_age = 0;           // frames since the last calibration (none yet while cal_cpu is 0)
    u32             track = 0;             // the profiler's "gpu" track
    RendererGpuStats last;
};

// The full-frame pass (renderer_post_layer): the world's color target, the swapchain's
// size and format, and the game's post shader drawn from it to the window. Its own
// pipeline layout: set 0 the world's image, a clamped linear sampler and the parameters
// (a span of the frame ring, rewritten into the frame's set each frame), and a push block.
struct PostPass : PostCore {
    VkShaderModule        vertex = VK_NULL_HANDLE; // the engine's vs_post (post.spv), loaded on first use
    VkShaderModule        module = VK_NULL_HANDLE; // the game's
    VkPipeline            pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout      layout = VK_NULL_HANDLE;
    VkDescriptorPool      pool = VK_NULL_HANDLE;
    VkDescriptorSet       sets[kFramesInFlight] = {};
    VkSampler             sampler = VK_NULL_HANDLE;
    VkImage               image = VK_NULL_HANDLE;
    VkDeviceMemory        memory = VK_NULL_HANDLE;
    VkImageView           view = VK_NULL_HANDLE;
    VkExtent2D            extent{0, 0};
    VkFormat              format = VK_FORMAT_UNDEFINED;
    Buffer                param_buffers[kFramesInFlight]; // host-visible, KV_POST_PARAMS float4s each
};

struct Swapchain {
    VkSwapchainKHR     handle = VK_NULL_HANDLE;
    VkFormat           format = VK_FORMAT_UNDEFINED;
    VkExtent2D         extent{0, 0};
    Array<VkImage>     images;
    Array<VkImageView> views;
    Array<VkSemaphore> present; // one "rendering done" semaphore per image
    bool               unrotated = false; // identity on a turned screen: suboptimal by choice, never rebuilt for it
};

struct Renderer : RenderCore {
    void*       window     = nullptr; // HostWindow::native_handle()

    VkInstance               instance  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR             surface   = VK_NULL_HANDLE;
    VkPhysicalDevice         gpu       = VK_NULL_HANDLE;
    u32                      queue_family = 0;
    VkDevice                 device    = VK_NULL_HANDLE;
    VkQueue                  queue     = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props{};
    bool                     multi_draw_indirect = false;
    bool                     debug_utils = false; // VK_EXT_debug_utils: names and labels for capture tools
    GpuProfiler              gpu_prof;

    Swapchain swapchain;
    bool      swapchain_dirty = true;
    u32       swapchain_generation = 0;

    VkDescriptorSetLayout tables_layout   = VK_NULL_HANDLE; // set 0
    VkDescriptorSetLayout textures_layout = VK_NULL_HANDLE; // set 1
    VkDescriptorPool      desc_pool       = VK_NULL_HANDLE;
    VkSampler             sampler         = VK_NULL_HANDLE; // nearest
    VkSampler             sampler_linear  = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout     = VK_NULL_HANDLE; // sets 0 and 1, the push block; graphics and compute
    VkShaderModule        sprite_shader   = VK_NULL_HANDLE;
    VkShaderModule        sort_shader     = VK_NULL_HANDLE;
    VkPipeline            pipelines[kBlendCount] = {};
    GamePipe              game_pipes[kGameSlots];
    PostPass              post;
    VkPipeline            cs_count = VK_NULL_HANDLE, cs_scan = VK_NULL_HANDLE,
                          cs_bucket_scan = VK_NULL_HANDLE, cs_scatter = VK_NULL_HANDLE, cs_key_sort = VK_NULL_HANDLE;

    Pool<Texture>        textures;  // slot = index; 0 is the default checkerboard
    Array<PendingUpload> uploads;   // staged, not yet recorded



    FrameInFlight frames[kFramesInFlight];

    // A requested screenshot: the next presented frame is copied to a host buffer after
    // its draws and written as a PPM once its fence has passed. A dev facility; it stalls.
    // A requested sort check: the sort's outputs are read back after the frame and
    // verified against the rows' buckets. Dev only; stalls like the screenshot.


    u32 validation_errors   = 0;
    u32 validation_warnings = 0;
};

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

// ---- vk_device.cc -----------------------------------------------------------------------
bool rb_create_instance(Renderer* r);
bool rb_create_device(Renderer* r);

// ---- vk_resources.cc --------------------------------------------------------------------
u32  rb_find_memory_type(Renderer* r, u32 type_bits, VkMemoryPropertyFlags flags);
bool rb_create_host_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
bool rb_create_device_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
void rb_destroy_buffer(Renderer* r, Buffer& b);
// Deferred destruction: queued on the slot of the next frame to be submitted, run after
// that frame's fence passes.
void rb_defer_destroy(Renderer* r, Garbage::Kind kind, u64 handle);
void rb_defer_destroy_buffer(Renderer* r, Buffer& b);

// The frame ring. Allocation is for the frame being recorded; release happens at that
// frame's fence. A span is valid until the frame is submitted; its buffer is kept alive
// through the garbage queue if the ring grows meanwhile.

// Textures.
void rb_destroy_texture_now(Renderer* r, Texture& t);
bool rb_create_default_texture(Renderer* r);
void rb_write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot);
VkFormat rb_vk_format(TextureFormat f);


// Pools, handle kinds, tables, descriptors.
bool rb_create_static_buffers(Renderer* r);   // material table, sort meta, indirect
bool rb_create_descriptors(Renderer* r);
void rb_write_tables_set(Renderer* r, FrameInFlight& f);
void rb_destroy_resources(Renderer* r); // everything above, device idle

// ---- vk_swapchain.cc --------------------------------------------------------------------
bool rb_create_frames(Renderer* r);
void rb_destroy_frames(Renderer* r);
bool rb_recreate_swapchain(Renderer* r); // fenced: the old chain retires with the frame
void rb_destroy_swapchain_now(Renderer* r);

// ---- vk_sprite_pass.cc ------------------------------------------------------------------
bool rb_load_shaders(Renderer* r);
bool rb_create_pipelines(Renderer* r);  // graphics: needs the swapchain format
void rb_defer_destroy_pipelines(Renderer* r);
void rb_destroy_pipelines_now(Renderer* r);
bool rb_create_compute_pipelines(Renderer* r);
bool rb_load_shader(Renderer* r, const char* name, VkShaderModule* out); // <exe>/shaders/<name>

// ---- vk_post_pass.cc --------------------------------------------------------------------
bool rb_post_active(const Renderer* r);      // a post shader set and loaded, and a target to draw into
bool rb_post_prepare(Renderer* r, u32 width, u32 height);         // the target for the swapchain's size and format; false: draw straight
void rb_post_begin_world(Renderer* r, VkCommandBuffer cmd, const RenderView& view); // rendering into the target
void rb_post_end_world(Renderer* r, VkCommandBuffer cmd); // ends it; the target becomes readable
void rb_post_draw(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, const RenderView& view); // the target to the window, inside its rendering
bool rb_post_create_pipeline(Renderer* r);   // needs the swapchain format
void rb_post_defer_destroy_pipeline(Renderer* r);
void rb_post_destroy_now(Renderer* r);

// vk_gpu_profile.cc. Names and labels (VK_EXT_debug_utils, when the instance has it: a
// no-op unless a capture tool or layer listens). Timing: query pools made after the
// frames, a frame's queries reset and its marks and statistics written while it records
// (while the profiler records), read once its fence has passed (never waited for).
void rb_name(Renderer* r, VkObjectType type, u64 handle, const char* name);
void rb_label_begin(Renderer* r, VkCommandBuffer cmd, const char* name);
void rb_label_end(Renderer* r, VkCommandBuffer cmd);
bool rb_gpu_profile_create(Renderer* r);
void rb_gpu_profile_destroy(Renderer* r);
void rb_gpu_frame_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd); // resets, then the start mark
void rb_gpu_mark(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, GpuMark mark);
void rb_gpu_stats_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd);
void rb_gpu_stats_end(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd);
void rb_gpu_collect(Renderer* r, FrameInFlight& f); // after its fence
// Writes the screenshot buffer the frame filled; the frame's fence has passed.
// Verifies the sort outputs the frame read back; the frame's fence has passed.

// ---- renderer.cc ------------------------------------------------------------------------
// The pipeline slot for a material: its blend's, or a game shader's (assigned here, its
// module loaded and its pipeline made once the swapchain's format is known).
bool rb_create_game_pipeline(Renderer* r, GamePipe& gp);

} // namespace kv
