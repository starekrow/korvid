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
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"
#include "vk_funcs.h"
#include "vk_platform.h"

namespace kv {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxTextures    = 1024; // bindless array size; slot 0 is the default texture
constexpr u32 kMaxMaterials   = 4096; // GPU material rows; row 0 is the default material
constexpr u32 kMaxSprites     = 1u << 20;
constexpr u32 kDefaultTexSize = 16;   // checkerboard texels per side
constexpr u32 kDefaultTexCell = 2;    // texels per checker cell
constexpr u32 kBlendCount     = (u32)BlendMode::Count;

// Sort buckets: layers clamped to [kLayerMin, kLayerMax], eight pipeline slots per layer:
// the three blends with the engine's fragment shader, then five game shaders' (shader,
// blend) pairs as materials name them. Mirrored by sprite_common.slangh.
constexpr i32 kLayerMin     = -128;
constexpr i32 kLayerMax     = 127;
constexpr u32 kLayerCount   = 256;
constexpr u32 kSlotCount    = 8;
constexpr u32 kGameSlots    = kSlotCount - kBlendCount; // 5
constexpr u32 kBucketCount  = kLayerCount * kSlotCount; // 2048
constexpr u32 kBlockRows    = 1024;                      // sprite rows per compute block
constexpr u32 kComputeGroup = 256;                       // threads per compute workgroup

constexpr u32 kInitialSpriteRows = 4096;
constexpr u32 kInitialRingBytes  = 8u << 20;

// ---- GPU-side layouts: std430, mirrored by sprite_common.slangh --------------------------

// One sprite row. Slot-addressed: the row index is the render world's pool slot.
struct GpuSprite {
    f32 prev[4];    // position x, y, rotation, scale x, at the start of the stamped tick
    f32 cur[4];     // the same, now
    f32 scale_y[2]; // prev scale y, cur scale y
    f32 size[2];    // quad size in world units before the placement's scale
    f32 uv[4];      // x, y, w, h
    f32 tint[4];
    u32 material;   // material row
    u32 stamp;      // the tick of the last Set; rows stamped with the frame's tick interpolate
    i32 layer;
    u32 flags;      // bit 0: alive; bit 1: screen space; bits 2-3: SpriteSort
    f32 order;      // the sort key or the feet's offset
    u32 pad[3];
};
static_assert(sizeof(GpuSprite) == 112, "GpuSprite must match sprite_common.slangh");

constexpr u32 kSpriteAlive  = 1;
constexpr u32 kSpriteScreen = 2; // placed in window pixels through the screen view, not the camera
constexpr u32 kSpriteSortShift = 2;   // SpriteSort in bits 2-3
constexpr u32 kSpriteAnchorShift = 4; // a screen row's anchor: bits 4-5 x, 6-7 y; 0 center, 1 low (left, bottom), 2 high
constexpr u32 kKeySortMax = 2048;     // visible rows of one keyed bucket the key sort orders (the rest keep row order)

// One material row.
struct GpuMaterial {
    f32 tint[4];
    f32 params[8];
    u32 texture; // bindless slot, resolved through the texture pool when the row is written
    u32 blend;
    u32 filter;  // TextureFilter: which sampler
    u32 slot;    // its pipeline slot: the blend's, or a game shader's
};
static_assert(sizeof(GpuMaterial) == 64, "GpuMaterial must match sprite_common.slangh");

// Shared by every stage: the vertex shader uses the views, the compute passes the cull
// rects. World rows go through `view`, screen rows (kSpriteScreen) through `screen`.
struct PushData {
    f32 view[4];         // world to clip: affine2 a, b, c, d
    f32 view_trans[2];   // tx, ty
    f32 alpha;           // interpolation point for rows stamped with `tick`
    u32 tick;
    f32 cull[4];         // world rows: the view's bounding rect in world units, x, y, w, h
    f32 screen[4];       // screen pixels (center origin, +y up) to clip
    f32 screen_trans[2];
    u32 row_count;       // sprite rows the compute passes look at
    u32 key_bucket;      // cs_key_sort: the bucket this dispatch sorts
    f32 screen_cull[4];  // screen rows: the window rect in pixels, x, y, w, h
    f32 time;            // the render time in seconds, for game shaders
    u32 pad_t[3];
};
static_assert(sizeof(PushData) == 112, "PushData must match sprite_common.slangh");

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

// A material: what the API was told, kept so a row can be rewritten when the texture it
// names comes and goes. Its pool index is its GPU row.
struct MaterialRecord {
    Handle    texture;
    BlendMode blend;
    TextureFilter filter;
    color     tint;
    f32       params[KV_MATERIAL_PARAMS];
    Name      name;   // MaterialDesc::name, or derived from the content at creation
    Name      shader; // a game fragment shader's name, or none
    u32       slot;   // its pipeline slot: the blend, or kBlendCount + its game pipe
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

// A sprite in the render world. The GPU row, plus what the GPU does not need.
struct RenderSprite {
    GpuSprite gpu;
    Handle    entity;
    u32       bucket; // the CPU's copy of the sort bucket, for the per-bucket counts
    bool      keyed;  // its sort is not Row: counted in keyed_counts[bucket]
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
    Array<Garbage>  garbage;                    // destroyed after this frame's fence
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

// The frame's timestamps: its start, after the uploads, after the sort, after the drawing,
// its end. Spans: the frame, the uploads, the sort, the draw.
enum GpuMark : u32 { kGpuStart, kGpuUploaded, kGpuSorted, kGpuDrawn, kGpuEnd, kGpuMarks };

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
bool rv_create_instance(Renderer* r);
bool rv_create_device(Renderer* r);

// ---- vk_resources.cc --------------------------------------------------------------------
u32  rv_find_memory_type(Renderer* r, u32 type_bits, VkMemoryPropertyFlags flags);
bool rv_create_host_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
bool rv_create_device_buffer(Renderer* r, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out);
void rv_destroy_buffer(Renderer* r, Buffer& b);
// Deferred destruction: queued on the frame being recorded, run after its fence passes.
void rv_defer_destroy(Renderer* r, Garbage::Kind kind, u64 handle);
void rv_defer_destroy_buffer(Renderer* r, Buffer& b);
void rv_run_garbage(Renderer* r, FrameInFlight& f);

// The frame ring. Allocation is for the frame being recorded; release happens at that
// frame's fence. A span is valid until the frame is submitted; its buffer is kept alive
// through the garbage queue if the ring grows meanwhile.
bool     rv_ring_init(Renderer* r, VkDeviceSize bytes);
RingSpan rv_ring_alloc(Renderer* r, VkDeviceSize bytes, VkDeviceSize align);
void     rv_ring_submitted(Renderer* r, FrameInFlight& f); // close the frame's span
void     rv_ring_release(Renderer* r, FrameInFlight& f);   // the frame's fence has passed

// Textures.
bool rv_create_texture_image(Renderer* r, Texture& t);
bool rv_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels); // queues the upload
void rv_defer_destroy_texture(Renderer* r, Texture& t);
void rv_destroy_texture_now(Renderer* r, Texture& t);
bool rv_create_default_texture(Renderer* r);
void rv_mark_slot_dirty(Renderer* r, u32 slot);
void rv_write_texture_slot(Renderer* r, VkDescriptorSet set, u32 slot);
VkFormat rv_vk_format(TextureFormat f);

// Materials: the pool record becomes a GPU row, uploaded with the next frame.
void rv_material_row(Renderer* r, u32 row, GpuMaterial& out);
void rv_mark_material_dirty(Renderer* r, u32 row);
// Every sprite using material row `row` is rebucketed (its blend may have changed) and
// marked for upload. For material edits and destruction; never on the frame path.
void rv_rebind_material(Renderer* r, u32 row);

// Pools, handle kinds, tables, descriptors.
bool rv_create_resource_pools(Renderer* r);
bool rv_create_tables(Renderer* r, u32 rows); // sprite tables for `rows`; the old ones retire
bool rv_create_static_buffers(Renderer* r);   // material table, sort meta, indirect
bool rv_create_descriptors(Renderer* r);
void rv_write_tables_set(Renderer* r, FrameInFlight& f);
void rv_destroy_resources(Renderer* r); // everything above, device idle

// ---- vk_swapchain.cc --------------------------------------------------------------------
bool rv_create_frames(Renderer* r);
void rv_destroy_frames(Renderer* r);
bool rv_recreate_swapchain(Renderer* r); // fenced: the old chain retires with the frame
void rv_destroy_swapchain_now(Renderer* r);

// ---- vk_sprite_pass.cc ------------------------------------------------------------------
bool rv_load_shaders(Renderer* r);
bool rv_create_pipelines(Renderer* r);  // graphics: needs the swapchain format
void rv_defer_destroy_pipelines(Renderer* r);
void rv_destroy_pipelines_now(Renderer* r);
bool rv_create_compute_pipelines(Renderer* r);
// Records one frame: uploads, table copies, the sort, and the draws when there is an image.
bool rv_record_frame(Renderer* r, FrameInFlight& f, bool have_image, u32 image_index, const RenderView& view);
bool rv_load_shader(Renderer* r, const char* name, VkShaderModule* out); // <exe>/shaders/<name>
u32  rv_record_draws(Renderer* r, VkCommandBuffer cmd, u32 first_bucket, u32 end_bucket);

// ---- vk_post_pass.cc --------------------------------------------------------------------
bool rv_post_set(Renderer* r, i32 layer, Name shader);
bool rv_post_active(const Renderer* r);      // a post shader set and loaded, and a target to draw into
bool rv_post_prepare(Renderer* r);           // the target for the swapchain's size and format; false: draw straight
void rv_post_begin_world(Renderer* r, VkCommandBuffer cmd, const RenderView& view); // rendering into the target
void rv_post_end_world(Renderer* r, VkCommandBuffer cmd); // ends it; the target becomes readable
void rv_post_draw(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, const RenderView& view); // the target to the window, inside its rendering
bool rv_post_create_pipeline(Renderer* r);   // needs the swapchain format
void rv_post_defer_destroy_pipeline(Renderer* r);
void rv_post_destroy_now(Renderer* r);

// vk_gpu_profile.cc. Names and labels (VK_EXT_debug_utils, when the instance has it: a
// no-op unless a capture tool or layer listens). Timing: query pools made after the
// frames, a frame's queries reset and its marks and statistics written while it records
// (while the profiler records), read once its fence has passed (never waited for).
void rv_name(Renderer* r, VkObjectType type, u64 handle, const char* name);
void rv_label_begin(Renderer* r, VkCommandBuffer cmd, const char* name);
void rv_label_end(Renderer* r, VkCommandBuffer cmd);
bool rv_gpu_profile_create(Renderer* r);
void rv_gpu_profile_destroy(Renderer* r);
void rv_gpu_frame_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd); // resets, then the start mark
void rv_gpu_mark(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd, GpuMark mark);
void rv_gpu_stats_begin(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd);
void rv_gpu_stats_end(Renderer* r, FrameInFlight& f, VkCommandBuffer cmd);
void rv_gpu_collect(Renderer* r, FrameInFlight& f); // after its fence
// Writes the screenshot buffer the frame filled; the frame's fence has passed.
bool rv_write_screenshot(Renderer* r);
// Verifies the sort outputs the frame read back; the frame's fence has passed.
bool rv_verify_sort(Renderer* r);

// ---- renderer.cc ------------------------------------------------------------------------
// Row index for a material handle: its row, or 0 (the default) when null or stale.
u32  rv_material_row_of(const Renderer* r, Handle material);
u32  rv_bucket_of(i32 layer, u32 slot);
// The pipeline slot for a material: its blend's, or a game shader's (assigned here, its
// module loaded and its pipeline made once the swapchain's format is known).
u32  rv_material_slot(Renderer* r, Name shader, BlendMode blend);
bool rv_create_game_pipeline(Renderer* r, GamePipe& gp);

} // namespace kv
