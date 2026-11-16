// renderer-cpu's types: the render core's contract met with memory (docs/PRDs/
// CPU-Renderer-PRD.md, "The CPU backend"). A buffer is an allocation, the ring is ordinary
// memory, a copy is memcpy, and a frame runs to its end inside renderer_render, so nothing is
// ever in flight when the next one begins; the two frame slots exist because the core's
// ring and garbage queue are written for them.
#pragma once
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/render/core/layout.h"

#define KV_RB_NAME       "cpu"
#define KV_RB_CLIP_Y_UP  0 // the projections as pushed (Vulkan's clip space, y down): the rasterizer maps them
#define KV_RB_SHADER_EXT ".cpu"
#define KV_RB_SURFACE    RendererSurface::Software
#define KV_RB_FRAME_SCOPE
#define KV_RB_NS         rb_cpu
#define KV_RB_ID         KV_RENDER_ID_CPU
#define KV_RB_UI         1
#define KV_RB_PACED      0 // a software surface has no vsync: frameloop spaces the frames
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/pool.h"
#include "raster.h"

namespace kv {

struct HostWindow;

namespace KV_RB_NS {

// A buffer: memory of the renderer's general allocator, its size for the free.
struct Buffer {
    u8* data = nullptr;
    u64 size = 0;
};
using RbBuffer  = Buffer;
using RbGarbage = Buffer; // freed when the frame slot comes round

} } // namespace kv::KV_RB_NS

#include "korvid/render/core/core.h"

namespace kv { namespace KV_RB_NS {

// A texture's pixels as uploaded: RGBA8, sRGB or not as its format says.
struct Texture : TextureCore {
    Buffer pixels;
    u32    version = 0; // the renderer's count when its pixels were last written (tile hashes)
};

// A game shader on the CPU: milestone 4's (none yet: the material draws in its blend's
// engine slot).
struct GamePipe : GamePipeCore {};

// The post pass on the CPU: milestone 4's (off until then).
struct PostPass : PostCore {};

struct FrameInFlight : FrameCore {
    Buffer layers; // the layers' offsets for the frame (float4 each), as the core writes them
};

// What the frame loop reads as the GPU's numbers: the CPU's, in time (milestone 5's).
struct GpuProfiler {
    RendererGpuStats last;
};

struct Renderer : RenderCore {
    Pool<Texture> textures;
    GamePipe      game_pipes[kGameSlots];
    PostPass      post;
    FrameInFlight frames[kFramesInFlight];
    GpuProfiler   gpu_prof;

    HostWindow* window = nullptr; // null: headless, drawing into `headless`
    Buffer      headless;         // the target with no window, B, G, R, A
    // The target the frame draws into, from rb_frame_acquire to rb_frame_present: the
    // window's pixels (locked until present), or the headless buffer.
    u8*  target = nullptr;
    u32  target_width = 0, target_height = 0, target_pitch = 0;
    bool target_rgba = false;  // R, G, B in memory; else B, G, R
    bool presenting  = false;  // the window's frame is held: present_software() is owed

    // The frame being drawn (frame.cc): its push block and layer offsets (rb_sort,
    // rb_bind_sprites); the sort's totals and starts are in sort_meta, as the GPU's are.
    PushData   push{};
    const f32* layers = nullptr;
    // A pass: its clear, and its draws in order (a quad's index, or a UI item's with
    // kCpuDrawUi); binned into tiles at its end.
    RcPass         pass = RcPass::Window;
    u16            clear[3] = {};
    Array<CpuQuad> quads;
    Array<CpuUi>   ui;
    Array<u32>     draws;
    Array<u32>     bin_counts, bin_items;
    bool           quantize8 = false; // KV_CPU_TARGET8: round each draw through 8-bit sRGB, as the GPU's target does
    u32            texture_version = 0;

    // The window pass's tiles as last drawn: a tile whose draws hash as they did, into the
    // same target, is left as it is (KV_CPU_TILE_CACHE=0 draws every tile).
    Array<u64> tile_hash;
    const u8*  hashed_target = nullptr;
    u32        hashed_width = 0, hashed_height = 0, hashed_pitch = 0;
    bool       hashed_rgba = false;
    bool       tile_cache = true;
    u32        tiles_drawn = 0, tiles_skipped = 0; // the last window pass's

    // KV_CPU_STATS: a line every 300 window passes, their means: the UI lists' decoding, the
    // pass's end (binning, hashing, the tiles), the tiles drawn and skipped.
    bool stats = false;
    u32  stat_passes = 0;
    f64  stat_ui = 0.0, stat_end = 0.0, stat_end_worst = 0.0;
    u64  stat_drawn = 0, stat_skipped = 0, stat_items = 0;
};

// A buffer of `bytes`, zeroed; false when there is no memory.
bool cpu_alloc(Renderer* r, u64 bytes, Buffer& out);

} } // namespace kv::KV_RB_NS
