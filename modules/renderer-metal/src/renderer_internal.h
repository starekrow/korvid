// Korvid renderer-metal: what the renderer's source files share. The Renderer struct, the
// GPU-side layouts the shaders mirror, and the private functions each file provides:
//   mt_resources.mm    buffers, the frame ring, deferred release, textures, materials, the
//                      handle kinds and pools, the tables, the argument buffers
//   mt_sprite_pass.mm  shaders, pipelines, and the frame's commands: uploads, the compute
//                      sort, the indirect draws, the sort check
//   renderer.mm        the public API behind korvid/render/renderer.h, and the frame
//   sdl3/mt_platform_sdl3.mm  the CAMetalLayer, from SDL's window
// This header and the sources next to it are the only place a Metal header is included.
//
// The CPU side is renderer-vulkan's, ported as it is (slice 7 extracts what the two share
// once both run; until then the constants and layouts below must match renderer-vulkan's
// renderer_internal.h and render/shaders/sprite_common.slangh). What differs is the API:
// the frame is a command buffer waited on instead of a fence, the drawable comes from the
// layer instead of a swapchain, an argument buffer per frame holds the bindless textures
// instead of a descriptor set, and Metal tracks hazards between encoders, so the barriers
// are gone.
//
// Ownership: Metal objects are held as Vulkan handles are, by pointer in plain records,
// made with a +1 reference (the new... methods, or retain) and released explicitly, at once
// or after the frame that last used them. The engine's pools and arrays copy records as
// bytes, which automatic reference counting cannot follow, so this module builds without it
// (clang's default for .mm). Autoreleased objects (a command buffer, a drawable, a
// descriptor) live inside an @autoreleasepool around each entry point that makes them.
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"

namespace kv {

constexpr u32 kFramesInFlight = 2;
constexpr u32 kMaxTextures    = 1024; // bindless array size; slot 0 is the default texture (sprite_fragment.slangh's BindlessSet)
constexpr u32 kMaxMaterials   = 4096; // GPU material rows; row 0 is the default material
constexpr u32 kMaxSprites     = 1u << 20;
constexpr u32 kDefaultTexSize = 16;   // checkerboard texels per side
constexpr u32 kDefaultTexCell = 2;    // texels per checker cell
constexpr u32 kBlendCount     = (u32)BlendMode::Count;

// Sort buckets, as renderer-vulkan's: layers clamped to [kLayerMin, kLayerMax], eight
// pipeline slots per layer (the three blends, then five game shaders).
constexpr i32 kLayerMin     = -128;
constexpr i32 kLayerMax     = 127;
constexpr u32 kLayerCount   = 256;
constexpr u32 kSlotCount    = 8;
constexpr u32 kGameSlots    = kSlotCount - kBlendCount; // 5
constexpr u32 kBucketCount  = kLayerCount * kSlotCount; // 2048
constexpr u32 kBlockRows    = 1024;                      // sprite rows per compute block
constexpr u32 kComputeGroup = 256;                       // threads per threadgroup: MSL has no numthreads, every dispatch says it

constexpr u32   kInitialSpriteRows = 4096;
constexpr usize kInitialRingBytes  = 8u << 20;

// ---- GPU-side layouts: mirrored by sprite_common.slangh ------------------------------------

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
constexpr u32 kSpriteScreen = 2;
constexpr u32 kSpriteSortShift = 2;
constexpr u32 kSpriteAnchorShift = 4;
constexpr u32 kKeySortMax = 2048;

struct GpuMaterial {
    f32 tint[4];
    f32 params[8];
    u32 texture; // bindless slot
    u32 blend;
    u32 filter;  // TextureFilter: which sampler
    u32 slot;    // its pipeline slot
};
static_assert(sizeof(GpuMaterial) == 64, "GpuMaterial must match sprite_common.slangh");

struct PushData {
    f32 view[4];
    f32 view_trans[2];
    f32 alpha;
    u32 tick;
    f32 cull[4];
    f32 screen[4];
    f32 screen_trans[2];
    u32 row_count;
    u32 key_bucket;
    f32 screen_cull[4];
    f32 time;
    u32 pad_t[3];
};
static_assert(sizeof(PushData) == 112, "PushData must match sprite_common.slangh");

// Buffer indices, as Slang numbers the globals in declaration order through the include
// chain (docs/design/renderer-metal.md). A dev build checks these against the reflection
// the build writes beside each library (<name>.metallib.json).
enum SpriteBinding : u32 { kBindPush = 0, kBindLayers = 1, kBindMaterials = 2, kBindBindless = 3, kBindSprites = 4, kBindSorted = 5 };
enum SortBinding : u32 {
    kSortPush = 0, kSortLayers = 1, kSortSprites = 2, kSortMaterials = 3, kSortSorted = 4,
    kSortBucketOf = 5, kSortBlockHist = 6, kSortMeta = 7, kSortIndirect = 8,
};
// The bindless set (an argument buffer, Metal 3 tier 2): one 64-bit resource id an entry,
// the two samplers, then the textures.
constexpr usize kBindlessSamplers = 0;
constexpr usize kBindlessTextures = 2 * sizeof(MTLResourceID);
constexpr usize kBindlessBytes    = kBindlessTextures + kMaxTextures * sizeof(MTLResourceID);

// ---- CPU-side records ------------------------------------------------------------------------

struct Texture {
    id<MTLTexture> tex = nil; // private storage; null until the first upload: the slot shows the default
    u32            width = 0, height = 0;
    TextureFormat  format = TextureFormat::RGBA8_SRGB;
    bool           resident = false;
    Name           name;
};

struct MaterialRecord {
    Handle        texture;
    BlendMode     blend;
    TextureFilter filter;
    color         tint;
    f32           params[KV_MATERIAL_PARAMS];
    Name          name;
    Name          shader;
    u32           slot;
};

struct GamePipe {
    Name                       shader;
    BlendMode                  blend = BlendMode::Alpha;
    id<MTLLibrary>             library = nil;
    id<MTLFunction>            fragment = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    bool                       used = false, failed = false;
};

struct RenderSprite {
    GpuSprite gpu;
    Handle    entity;
    u32       bucket;
    bool      keyed;
};

struct PendingUpload {
    u32            slot;
    id<MTLTexture> tex;    // skipped if the slot's texture is no longer this one
    id<MTLBuffer>  src;    // the ring, or a one-off staging buffer already queued for release
    usize          offset;
    u32            width, height;
};

struct RingSpan {
    id<MTLBuffer> buffer = nil;
    usize         offset = 0;
    void*         ptr    = nullptr;
};

struct FrameRing {
    id<MTLBuffer> buf = nil; // shared storage: the CPU writes, a blit reads
    usize         size = 0;
    usize         head = 0, tail = 0, used = 0, frame_bytes = 0, peak = 0;
    u32           grows = 0;
};

struct FrameInFlight {
    id<MTLCommandBuffer> cb = nil;       // retained until renderer_render waits for it, as for a fence
    id<MTLBuffer>        bindless = nil; // the argument buffer: samplers and texture ids, shared storage
    id<MTLBuffer>        layers = nil;   // the layers' offsets, previous and current: float4 each, shared
    Array<id>            garbage;        // released once this frame's command buffer has completed
    Array<u32>           dirty_slots;    // texture slots to rewrite in this frame's argument buffer
    usize                ring_end = 0, ring_bytes = 0;
    bool                 submitted = false;
};

struct SpriteTables {
    u32           capacity = 0; // rows; a multiple of kBlockRows
    id<MTLBuffer> sprites = nil, bucket_of = nil, block_hist = nil, sorted = nil; // private storage
    u32           blocks() const { return capacity / kBlockRows; }
};

struct Renderer {
    Renderer**  owner_slot = nullptr;
    Allocator*  general    = nullptr;
    void*       window     = nullptr; // HostWindow::native_handle(): an SDL_Window*
    void*       metal_view = nullptr; // SDL's view that carries the layer; destroyed with the renderer
    CAMetalLayer* layer    = nil;     // the view's, not retained here
    char        base_path[512] = {};
    bool        validation = false;
    bool        vsync      = true;

    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> queue  = nil;
    u32  want_width = 0, want_height = 0;
    bool size_dirty = true;

    id<MTLSamplerState>        sampler = nil, sampler_linear = nil; // nearest, linear; in the argument buffers
    id<MTLLibrary>             sprite_lib = nil, sort_lib = nil;
    id<MTLFunction>            vs_main = nil, fs_main = nil;
    id<MTLRenderPipelineState> pipelines[kBlendCount] = {};
    GamePipe                   game_pipes[kGameSlots];
    id<MTLComputePipelineState> cs_count = nil, cs_scan = nil, cs_bucket_scan = nil, cs_scatter = nil, cs_key_sort = nil;
    f64 start_time = 0.0;

    Pool<Texture>        textures;
    Pool<MaterialRecord> materials;
    Handle               default_material;
    u16                  texture_kind = 0, material_kind = 0;
    Array<PendingUpload> uploads;

    Pool<RenderSprite>   sprites;
    Map<Handle, u32>     sprite_of_entity;
    Map<u64, Handle>     texture_by_name;
    Map<u64, Handle>     material_by_name;
    u32                  textures_made = 0;
    Array<u64>           sprite_dirty;
    Array<u64>           material_dirty;
    u32                  bucket_counts[kBucketCount] = {};
    u32                  keyed_counts[kBucketCount] = {};
    f32*                 check_keys = nullptr;
    u32                  tick = 0;
    u64                  deltas_applied = 0;
    u64                  rows_uploaded  = 0;

    SpriteTables  tables;
    id<MTLBuffer> material_table = nil; // GpuMaterial[kMaxMaterials], private
    id<MTLBuffer> sort_meta = nil;      // u32 totals[kBucketCount], starts[kBucketCount], visible
    id<MTLBuffer> indirect = nil;       // MTLDrawPrimitivesIndirectArguments[kBucketCount]: VkDrawIndirectCommand's layout
    FrameRing     ring;

    FrameInFlight frames[kFramesInFlight];
    f32           layer_prev[kLayerCount][2] = {};
    f32           layer_cur[kLayerCount][2]  = {};
    u32           frame_index = 0;
    u64           frame_count = 0;

    // The full-frame pass and screenshots are milestone 4's: until then they say so once.
    Name   post_shader;
    i32    post_layer = 0;
    f32    post_params[KV_POST_PARAMS * 4] = {};
    u32    post_param_count = 0;
    bool   post_warned = false, shot_warned = false;

    bool          check_pending  = false;
    bool          check_recorded = false;
    id<MTLBuffer> check_buffer = nil; // shared: sorted, bucket_of and the sort meta, after the sort
    u32           check_rows = 0;
    u32           checks_passed = 0, checks_failed = 0;

    f64 last_wait = 0.0;
};

// ---- mt_resources.mm ------------------------------------------------------------------------
id<MTLBuffer> mt_buffer(Renderer* r, usize bytes, MTLResourceOptions storage, const char* label); // +1
void mt_defer_release(Renderer* r, id obj); // after the frame being recorded completes; nil is fine
void mt_run_garbage(Renderer* r, FrameInFlight& f);

bool     mt_ring_init(Renderer* r, usize bytes);
RingSpan mt_ring_alloc(Renderer* r, usize bytes, usize align);
void     mt_ring_submitted(Renderer* r, FrameInFlight& f);
void     mt_ring_release(Renderer* r, FrameInFlight& f);

bool mt_create_texture(Renderer* r, Texture& t); // any previous texture is released after the frame
bool mt_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels);
void mt_defer_destroy_texture(Renderer* r, Texture& t);
bool mt_create_default_texture(Renderer* r);
void mt_mark_slot_dirty(Renderer* r, u32 slot);
void mt_write_texture_slot(Renderer* r, FrameInFlight& f, u32 slot);
bool mt_create_bindless(Renderer* r); // the samplers and every frame's argument buffer
// The textures a draw may sample, made resident for a render encoder.
void mt_use_textures(Renderer* r, id<MTLRenderCommandEncoder> enc);

void mt_material_row(Renderer* r, u32 row, GpuMaterial& out);
void mt_mark_material_dirty(Renderer* r, u32 row);
void mt_rebind_material(Renderer* r, u32 row);

bool mt_create_resource_pools(Renderer* r);
bool mt_create_tables(Renderer* r, u32 rows);
bool mt_create_static_buffers(Renderer* r);
void mt_destroy_resources(Renderer* r); // everything above; nothing in flight

// ---- mt_sprite_pass.mm ----------------------------------------------------------------------
bool mt_load_library(Renderer* r, const char* name, id<MTLLibrary>* out); // <exe>/shaders/<name>.metallib
bool mt_load_shaders(Renderer* r);
bool mt_create_pipelines(Renderer* r);
bool mt_create_compute_pipelines(Renderer* r);
void mt_destroy_pipelines(Renderer* r);
u32  mt_material_slot(Renderer* r, Name shader, BlendMode blend);
// Records one frame into cb: uploads, table copies, the sort, and the draws into drawable's
// texture when there is one (nil: a zero-size window; the uploads still go).
bool mt_record_frame(Renderer* r, FrameInFlight& f, id<MTLCommandBuffer> cb, id<CAMetalDrawable> drawable, const RenderView& view);
bool mt_verify_sort(Renderer* r);

// ---- sdl3/mt_platform_sdl3.mm ---------------------------------------------------------------
// The layer to present through, from the window's native handle; null (logged) on failure.
// *view_out is SDL's view, destroyed with mt_platform_release.
CAMetalLayer* mt_platform_layer(void* native_window, void** view_out);
void          mt_platform_release(void* view);

// ---- renderer.mm ----------------------------------------------------------------------------
u32 mt_material_row_of(const Renderer* r, Handle material);
u32 mt_bucket_of(i32 layer, u32 slot);

} // namespace kv
