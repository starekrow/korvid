// Korvid render core: the numbers and the bytes every backend agrees on. The constants, the
// GPU-side layouts the shaders mirror (render/shaders/sprite_common.slangh: change one and
// change the other) and the CPU-side records that hold no GPU object. One definition, where
// each backend's renderer_internal.h used to carry a copy (docs/PRDs/Engine-Slice-9-PRD.md).
// Nothing here names a graphics API.
#pragma once
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/core/name.h"

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
    u32 ui_base;         // a UI draw: its layer's region in the frame's UI buffer, in bytes (pad_t0 in the shaders)
    u32 ui_count;        // and its items (pad_t1)
    u32 pad_t0;
};
static_assert(sizeof(PushData) == 112, "PushData must match sprite_common.slangh");

// ---- CPU-side records with no GPU object in them ---------------------------------------------

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

// A sprite in the render world. The GPU row, plus what the GPU does not need.
struct RenderSprite {
    GpuSprite gpu;
    Handle    entity;
    u32       bucket; // the CPU's copy of the sort bucket, for the per-bucket counts
    bool      keyed;  // its sort is not Row: counted in keyed_counts[bucket]
};

// The frame's timestamps: its start, after the uploads, after the sort, after the drawing,
// its end. Spans: the frame, the uploads, the sort, the draw.
enum GpuMark : u32 { kGpuStart, kGpuUploaded, kGpuSorted, kGpuDrawn, kGpuEnd, kGpuMarks };

} // namespace kv
