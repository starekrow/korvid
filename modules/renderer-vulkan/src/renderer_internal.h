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
#include "korvid/render/core/core.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"
#include "vk_funcs.h"
#include "vk_platform.h"

namespace kv {

// ---- CPU-side records ---------------------------------------------------------------------

struct Buffer {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
    void*          mapped = nullptr; // host-visible buffers only
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
    Name           name;   // TextureDesc::name, or "texture:<n>"
};

// A game fragment shader with a blend: a pipeline slot of its own. Made when a material
// first names the pair; a shader that will not load or link is `failed`, and its slot draws
// with the engine's pipeline of its blend.
struct GamePipe {
    Name           shader;
    BlendMode      blend  = BlendMode::Alpha;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline     pipeline = VK_NULL_HANDLE;
    bool           used = false, failed = false;
};

// Something to destroy once the frame that last used it has passed its fence.
struct Garbage {
    enum Kind : u8 { BufferKind, ImageKind, ImageViewKind, MemoryKind, SwapchainKind, SemaphoreKind, PipelineKind } kind;
    u64 handle; // the Vulkan handle, widened
};

// A staged texture upload, recorded into the next frame's command buffer.
struct PendingUpload {
    u32          slot;   // texture slot; skipped if the slot's image is no longer `image`
    VkImage      image;
    VkBuffer     src;    // the ring, or a one-off staging buffer already queued as garbage
    VkDeviceSize offset;
    u32          width, height;
};

// A span of host-visible memory for the frame being recorded.
struct RingSpan {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void*        ptr    = nullptr;
};

// The frame ring: one host-visible buffer; a frame's allocations are released when that
// frame's fence has passed. Growth retires the old buffer through the garbage queue.
struct FrameRing {
    Buffer       buf;
    VkDeviceSize head  = 0; // next free byte
    VkDeviceSize tail  = 0; // first byte still owned by a frame in flight
    VkDeviceSize used  = 0; // bytes between tail and head, gaps included
    VkDeviceSize frame_bytes = 0; // taken by the frame being recorded so far
    VkDeviceSize peak  = 0;
    u32          grows = 0;
};

struct FrameInFlight {
    VkCommandPool   pool    = VK_NULL_HANDLE;
    VkCommandBuffer cmd     = VK_NULL_HANDLE;
    VkFence         fence   = VK_NULL_HANDLE;
    VkSemaphore     acquire = VK_NULL_HANDLE; // swapchain image available
    VkDescriptorSet tables  = VK_NULL_HANDLE; // set 0: sprite, material and sort buffers
    VkDescriptorSet textures = VK_NULL_HANDLE; // set 1: the sampler and the bindless array
    bool            tables_dirty = true;        // the table buffers changed: rewrite set 0 once the fence passes
    // Destroyed once this slot's next frame has passed its fence: what was queued up to its
    // submit (garbage_owned) goes at the slot's next wait; what was queued after it (between
    // frames: a staging buffer, a replaced texture) waits for the frame after, which may use
    // it, as may the other frame still in flight.
    Array<Garbage>  garbage;
    u32             garbage_owned = 0;
    Array<u32>      dirty_slots;                // texture slots to rewrite in this frame's set
    VkDeviceSize    ring_end   = 0;             // the ring head when this frame was submitted
    VkDeviceSize    ring_bytes = 0;             // what the frame took, released at its fence
    Buffer          layers;                     // the layers' offsets, previous and current: float4 each, host-visible
    bool            submitted  = false;
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
struct PostPass {
    Name                  shader;              // the game's post shader; null: the pass is off
    i32                   layer = 0;           // the first layer drawn over it
    bool                  failed = false;      // the shader would not load or link: off, logged
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
    f32                   params[KV_POST_PARAMS * 4] = {};
    u32                   param_count = 0;
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

// The GPU tables and the sort scratch, all sized for `capacity` sprite rows.
struct SpriteTables {
    u32    capacity = 0; // rows; a multiple of kBlockRows
    Buffer sprites;      // GpuSprite[capacity]
    Buffer bucket_of;    // u32[capacity]: each row's bucket, or ~0 when culled or dead
    Buffer block_hist;   // u32[blocks * kBucketCount]: counts, then offsets, per block
    Buffer sorted;       // u32[capacity]: row indices in draw order
    u32    blocks() const { return capacity / kBlockRows; }
};

struct Renderer {
    Renderer**  owner_slot = nullptr; // the module state's pointer to this; nulled on destroy
    Allocator*  general    = nullptr; // the owning module's attributed general allocator
    void*       window     = nullptr; // HostWindow::native_handle()
    char        base_path[512] = {}; // the executable's directory, where shaders/ is
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
    bool                     multi_draw_indirect = false;
    bool                     debug_utils = false; // VK_EXT_debug_utils: names and labels for capture tools
    GpuProfiler              gpu_prof;

    Swapchain swapchain;
    bool      swapchain_dirty = true;
    u32       swapchain_generation = 0;
    u32       want_width = 0, want_height = 0;

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
    f64                   start_time = 0.0; // the render time's zero
    VkPipeline            cs_count = VK_NULL_HANDLE, cs_scan = VK_NULL_HANDLE,
                          cs_bucket_scan = VK_NULL_HANDLE, cs_scatter = VK_NULL_HANDLE, cs_key_sort = VK_NULL_HANDLE;

    Pool<Texture>        textures;  // slot = index; 0 is the default checkerboard
    Pool<MaterialRecord> materials; // row = index; 0 is the default material
    Handle               default_material;
    u16                  texture_kind = 0, material_kind = 0;
    Array<PendingUpload> uploads;   // staged, not yet recorded

    // The persistent render world: the outer engine's entity handle maps to a slot here,
    // and the slot is the GPU row.
    Pool<RenderSprite>   sprites;
    Map<Handle, u32>     sprite_of_entity;
    Map<u64, Handle>     texture_by_name;  // Name::hash -> the first live texture with the name
    Map<u64, Handle>     material_by_name;
    u32                  textures_made = 0;
    Array<u64>           sprite_dirty;   // bit per slot: row to upload
    Array<u64>           material_dirty; // bit per row
    u32                  bucket_counts[kBucketCount] = {};
    u32                  keyed_counts[kBucketCount] = {};  // rows whose sort is not Row: their buckets get the key sort
    f32*                 check_keys = nullptr;             // --check-sort: each row's key as the checked frame computed it
    u32                  tick = 0;       // Tick deltas seen; the stamp of the newest rows
    u64                  deltas_applied = 0;
    u64                  rows_uploaded  = 0;

    SpriteTables tables;
    Buffer       material_table; // GpuMaterial[kMaxMaterials]
    Buffer       sort_meta;      // u32 totals[kBucketCount], starts[kBucketCount], visible
    Buffer       indirect;       // VkDrawIndirectCommand[kBucketCount]
    FrameRing    ring;

    FrameInFlight frames[kFramesInFlight];
    // The layers' scroll offsets (world units) at the last tick and the one before, by layer
    // + 128: copied into the frame's buffer when it is recorded, interpolated by alpha.
    f32           layer_prev[kLayerCount][2] = {};
    f32           layer_cur[kLayerCount][2]  = {};
    u32           frame_index = 0;
    u64           frame_count = 0;

    // A requested screenshot: the next presented frame is copied to a host buffer after
    // its draws and written as a PPM once its fence has passed. A dev facility; it stalls.
    char   shot_path[512] = {};
    bool   shot_pending   = false;
    bool   shot_recorded  = false;
    Buffer shot_buffer;
    u32    shot_width = 0, shot_height = 0;
    // A requested sort check: the sort's outputs are read back after the frame and
    // verified against the rows' buckets. Dev only; stalls like the screenshot.
    bool   check_pending  = false;
    bool   check_recorded = false;
    Buffer check_buffer;
    u32    check_rows = 0;
    u32    checks_passed = 0, checks_failed = 0;

    f64 last_wait = 0.0; // seconds blocked in the last renderer_render

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
void rb_run_garbage(Renderer* r, FrameInFlight& f); // what the slot's last frame owned; its fence has passed

// The frame ring. Allocation is for the frame being recorded; release happens at that
// frame's fence. A span is valid until the frame is submitted; its buffer is kept alive
// through the garbage queue if the ring grows meanwhile.
bool     rb_ring_init(Renderer* r, VkDeviceSize bytes);
RingSpan rb_ring_alloc(Renderer* r, VkDeviceSize bytes, VkDeviceSize align);
void     rb_ring_submitted(Renderer* r, FrameInFlight& f); // close the frame's span
void     rb_ring_release(Renderer* r, FrameInFlight& f);   // the frame's fence has passed

// Textures.
bool rb_create_texture_image(Renderer* r, Texture& t);
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels); // queues the upload
void rb_defer_destroy_texture(Renderer* r, Texture& t);
void rb_destroy_texture_now(Renderer* r, Texture& t);
bool rb_create_default_texture(Renderer* r);
void rb_mark_slot_dirty(Renderer* r, u32 slot);
void rb_write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot);
VkFormat rb_vk_format(TextureFormat f);

// Materials: the pool record becomes a GPU row, uploaded with the next frame.
void rb_material_row(Renderer* r, u32 row, GpuMaterial& out);
void rb_mark_material_dirty(Renderer* r, u32 row);
// Every sprite using material row `row` is rebucketed (its blend may have changed) and
// marked for upload. For material edits and destruction; never on the frame path.
void rb_rebind_material(Renderer* r, u32 row);

// Pools, handle kinds, tables, descriptors.
bool rb_create_resource_pools(Renderer* r);
bool rb_create_tables(Renderer* r, u32 rows); // sprite tables for `rows`; the old ones retire
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
// Records one frame: uploads, table copies, the sort, and the draws when there is an image.
bool rb_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view);
bool rb_load_shader(Renderer* r, const char* name, VkShaderModule* out); // <exe>/shaders/<name>
u32  rb_record_draws(Renderer* r, VkCommandBuffer cmd, u32 first_bucket, u32 end_bucket);

// ---- vk_post_pass.cc --------------------------------------------------------------------
bool rb_post_set(Renderer* r, i32 layer, Name shader);
bool rb_post_active(const Renderer* r);      // a post shader set and loaded, and a target to draw into
bool rb_post_prepare(Renderer* r);           // the target for the swapchain's size and format; false: draw straight
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
bool rb_write_screenshot(Renderer* r);
// Verifies the sort outputs the frame read back; the frame's fence has passed.
bool rb_verify_sort(Renderer* r);

// ---- renderer.cc ------------------------------------------------------------------------
// Row index for a material handle: its row, or 0 (the default) when null or stale.
u32  rb_material_row_of(const Renderer* r, Handle material);
u32  rb_bucket_of(i32 layer, u32 slot);
// The pipeline slot for a material: its blend's, or a game shader's (assigned here, its
// module loaded and its pipeline made once the swapchain's format is known).
u32  rb_material_slot(Renderer* r, Name shader, BlendMode blend);
bool rb_create_game_pipeline(Renderer* r, GamePipe& gp);

} // namespace kv
