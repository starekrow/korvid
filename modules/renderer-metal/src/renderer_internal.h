// Korvid renderer-metal: what the renderer's source files share. This backend's half of the
// renderer: the device and what is made from it. The other half is render's core
// (korvid/render/core/core.h): the render world, the public API, the frame's order of work.
// The core is built into this module by render_core.mm, against the types below; this
// header gives it RbBuffer, RbGarbage and five values, completes its records (Texture,
// GamePipe, PostPass, FrameInFlight, Renderer) and the sources define the rb_ functions it
// calls. Each file provides:
//   mt_resources.mm    buffers, deferred release, textures, the tables, the argument
//                      buffers; the core's buffer and ring hooks
//   mt_sprite_pass.mm  shaders, pipelines, and the frame's recording steps: the upload blit,
//                      the compute sort, the passes, the indirect draws
//   mt_post_pass.mm    the full-frame pass: the world's target, the game's post shader
//   mt_gpu_profile.mm  GPU timing: timestamps around the frame's encoders
//   renderer.mm        create and destroy, the layer, the screenshot's blit, and the frame's
//                      steps that wait, take a drawable, commit and present
//   render_core.mm     the render core, compiled here
//   sdl3/mt_platform_sdl3.mm  the CAMetalLayer, from SDL's window (macOS)
//   ios/mt_platform_ios.mm    the CAMetalLayer, the layer of platform_hid's UIView (iOS)
// This header and the sources next to it are the only place a Metal header is included.
//
// What differs from the other backends is the API: the frame is a command buffer waited on
// instead of a fence, the drawable comes from the layer instead of a swapchain, an argument
// buffer per frame holds the bindless textures instead of a descriptor set, each pass is an
// encoder of its own, and Metal tracks hazards between encoders, so there are no barriers.
//
// Ownership: Metal objects are held as Vulkan handles are, by pointer in plain records,
// made with a +1 reference (the new... methods, or retain) and released explicitly, at once
// or after the frame that last used them. The engine's pools and arrays copy records as
// bytes, which automatic reference counting cannot follow, so this module builds without it
// (clang's default for .mm). Autoreleased objects (a command buffer, a drawable, a
// descriptor) live inside an @autoreleasepool: the core's frame runs in one
// (KV_RB_FRAME_SCOPE), as does each other entry point that makes them.
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "korvid/render/renderer.h"
#include "korvid/render/material.h"
#include "korvid/render/core/layout.h"

// What this backend tells the render core (korvid/render/core/core.h).
#define KV_RB_NAME       "metal"
#define KV_RB_CLIP_Y_UP  1
#define KV_RB_SHADER_EXT ".metallib"
#define KV_RB_SURFACE    RendererSurface::Metal
#define KV_RB_UI         1
#define KV_RB_FRAME_SCOPE @autoreleasepool
#define KV_RB_NS          rb_mt
#define KV_RB_ID          KV_RENDER_ID_METAL
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/profile.h"

namespace kv { namespace KV_RB_NS {

// What the render core holds of this backend's (korvid/render/core/core.h): a buffer is the
// object itself, and so is a piece of garbage (released, whatever it is).
using RbBuffer  = id<MTLBuffer>;
using RbGarbage = id;

} } // namespace kv::KV_RB_NS

#include "korvid/render/core/core.h"

namespace kv { namespace KV_RB_NS {

// The constants and the GPU-side layouts are render's (korvid/render/core/layout.h).
// kMaxTextures is also the length of sprite_fragment.slangh's BindlessSet; every dispatch
// names kComputeGroup, since MSL has no numthreads.

// Buffer indices, as Slang numbers the globals in declaration order through the include
// chain (docs/design/renderer-metal.md). A dev build checks these against the reflection
// the build writes beside each library (<name>.metallib.json).
enum SpriteBinding : u32 { kBindPush = 0, kBindLayers = 1, kBindMaterials = 2, kBindBindless = 3, kBindSprites = 4, kBindSorted = 5 };
// The UI primitive's shader (ui.slang): the sprite shader's first four, then the item lists,
// at the index the sprite range gives the sprite table (rb_ui_draw puts the table back).
enum UiBinding : u32 { kUiPush = 0, kUiLayers = 1, kUiMaterials = 2, kUiBindless = 3, kUiList = 4 };
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

struct Texture : TextureCore {
    id<MTLTexture> tex = nil; // private storage; null until the first upload: the slot shows the default
};

struct GamePipe : GamePipeCore {
    id<MTLLibrary>             library = nil;
    id<MTLFunction>            fragment = nil;
    id<MTLRenderPipelineState> pipeline = nil;
};

struct PendingUpload {
    u32            slot;
    id<MTLTexture> tex;    // skipped if the slot's texture is no longer this one
    id<MTLBuffer>  src;    // the ring, or a one-off staging buffer already queued for release
    usize          offset;
    u32            width, height;
    u32            x = 0, y = 0; // where in the texture: a glyph page's rectangle, or the whole at 0, 0
};

struct FrameInFlight : FrameCore {
    id<MTLCommandBuffer> cb = nil;       // retained until renderer_render waits for it, as for a fence
    // While the frame is recorded (core/frame.inl's steps): its command buffer and drawable,
    // autoreleased and alive for the frame's scope, and the encoder that is open.
    id<MTLCommandBuffer>        rec = nil;
    id<CAMetalDrawable>         drawable = nil;
    id<MTLBlitCommandEncoder>   blit = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    bool                        world_pass = false; // the world's pass ran: the window's is not the frame's first
    id<MTLBuffer>        bindless = nil; // the argument buffer: samplers and texture ids, shared storage
    id<MTLBuffer>        layers = nil;   // the layers' offsets, previous and current: float4 each, shared
    bool                 gpu_stamped = false; // it carries timestamps (mt_gpu_profile.mm)
};

// The full-frame pass (renderer_post_layer), as renderer-vulkan's: the layers below the post
// layer draw into the world's target (the drawable's size and format, so the sprite
// pipelines draw into it unchanged); the window's pass begins with one triangle through the
// game's post shader, and the layers above draw over it. Metal tracks the target's hazards:
// the post pass's read waits for the world's writes, and the next frame's writes for the
// read.
enum PostBinding : u32 { kPostPush = 0, kPostParams = 1 }; // the world at texture 0, its sampler at sampler 0
struct PostPass : PostCore {
    id<MTLLibrary>             vertex_lib = nil; // post.metallib, on first use
    id<MTLFunction>            vertex = nil;     // vs_post
    id<MTLLibrary>             library = nil;    // the game's
    id<MTLFunction>            fragment = nil;   // its fs_main
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSamplerState>        sampler = nil;    // linear, clamped at the edges
    id<MTLTexture>             target = nil;     // private; render target and sampled
    u32                        width = 0, height = 0;
    id<MTLBuffer>              param_buffers[kFramesInFlight] = {}; // shared; the shader's gPostParams is writable, so not bytes
};

// GPU timing (mt_gpu_profile.mm), a profiling build's (KV_PROFILE) while the profiler
// records: timestamps at the start and end of the frame's encoders (Apple GPUs sample at
// stage boundaries only), read once the frame has completed. The marks are renderer-vulkan's.
enum GpuSample : u32 { kGpuUploadStart, kGpuUploadEnd, kGpuSortStart, kGpuSortEnd, kGpuDrawStart, kGpuDrawEnd, kGpuSamples };
struct GpuProfiler {
    id<MTLCounterSampleBuffer> samples = nil; // kGpuSamples per frame in flight
    u32              track = 0;       // the profiler's "gpu" track
    i64              gpu_to_os = 0;   // os_ticks minus the GPU's clock (both nanoseconds on Apple GPUs), sampled
    u32              cal_age = 0;     // frames since that was sampled
    bool             calibrated = false;
    RendererGpuStats last;
};

struct Renderer : RenderCore {
    void*       window     = nullptr; // HostWindow::native_handle(): an SDL_Window*
    void*       metal_view = nullptr; // SDL's view that carries the layer; destroyed with the renderer
    CAMetalLayer* layer    = nil;     // the view's, not retained here

    id<MTLDevice>       device = nil;
    id<MTLCommandQueue> queue  = nil;
    bool size_dirty = true;

    id<MTLSamplerState>        sampler = nil, sampler_linear = nil; // nearest, linear; in the argument buffers
    id<MTLLibrary>             sprite_lib = nil, sort_lib = nil;
    id<MTLFunction>            vs_main = nil, fs_main = nil;
    id<MTLRenderPipelineState> pipelines[kBlendCount] = {};
    GamePipe                   game_pipes[kGameSlots];
    id<MTLComputePipelineState> cs_count = nil, cs_scan = nil, cs_bucket_scan = nil, cs_scatter = nil, cs_key_sort = nil;
    id<MTLLibrary>             ui_lib = nil;      // ui.metallib: without it, no UI (renderer_caps still says the backend can)
    id<MTLFunction>            vs_ui = nil, fs_ui = nil;
    id<MTLRenderPipelineState> ui_pipeline = nil; // premultiplied: one, one minus source alpha

    Pool<Texture>        textures;
    Array<PendingUpload> uploads;



    FrameInFlight frames[kFramesInFlight];

    PostPass    post;
    GpuProfiler gpu_prof;
    bool        shot_warned = false;


};

// ---- mt_resources.mm ------------------------------------------------------------------------
id<MTLBuffer> rb_buffer(Renderer* r, usize bytes, MTLResourceOptions storage, const char* label); // +1
void rb_defer_release(Renderer* r, id obj); // after the next frame to be submitted completes; nil is fine
void rb_release_all(Renderer* r, FrameInFlight& f);  // everything; nothing is in flight


void rb_write_texture_slot(Renderer* r, FrameInFlight& f, u32 slot);
bool rb_create_bindless(Renderer* r); // the samplers and every frame's argument buffer
// The textures a draw may sample, made resident for a render encoder.
void rb_use_textures(Renderer* r, id<MTLRenderCommandEncoder> enc);


bool rb_create_static_buffers(Renderer* r);
void rb_destroy_resources(Renderer* r); // everything above; nothing in flight

// ---- mt_sprite_pass.mm ----------------------------------------------------------------------
bool rb_load_library(Renderer* r, const char* name, id<MTLLibrary>* out); // <exe>/shaders/<name>.metallib
bool rb_load_shaders(Renderer* r);
bool rb_create_pipelines(Renderer* r);
bool rb_create_compute_pipelines(Renderer* r);
void rb_destroy_pipelines(Renderer* r);
#if KV_DEV || KV_DEBUG
// A library's buffer, texture and sampler indices by name, against the reflection the build
// writes beside it (<lib>.metallib.json). False, logged by name, on any difference.
struct MtExpected { const char* name; u32 index; };
bool rb_check_bindings(Renderer* r, const char* lib, const MtExpected* want, u32 count);
#endif

// ---- mt_post_pass.mm ------------------------------------------------------------------------
bool rb_post_active(const Renderer* r);
void rb_post_draw(Renderer* r, FrameInFlight& f, id<MTLRenderCommandEncoder> enc, const RenderView& view); // inside the window's pass
void rb_post_destroy(Renderer* r); // nothing in flight

// ---- mt_gpu_profile.mm ----------------------------------------------------------------------
bool rb_gpu_profile_create(Renderer* r);  // nothing to do (false never) where it cannot sample
void rb_gpu_profile_destroy(Renderer* r);
void rb_gpu_frame_begin(Renderer* r, FrameInFlight& f); // whether this frame is timed
// Timestamps on an encoder about to be made, when the frame is timed. Render: `first` takes
// the draw's start, `last` its end (with the post pass, the world's encoder is first).
void rb_gpu_attach(Renderer* r, FrameInFlight& f, MTLBlitPassDescriptor* d);
void rb_gpu_attach(Renderer* r, FrameInFlight& f, MTLComputePassDescriptor* d);
void rb_gpu_attach(Renderer* r, FrameInFlight& f, MTLRenderPassDescriptor* d, bool first, bool last);
void rb_gpu_collect(Renderer* r, FrameInFlight& f); // it has completed

// ---- sdl3/mt_platform_sdl3.mm, ios/mt_platform_ios.mm -------------------------------------
// The layer to present through, from the window's native handle; null (logged) on failure.
// *view_out is a view to destroy with mt_platform_release (SDL's), or null.
CAMetalLayer* mt_platform_layer(void* native_window, void** view_out);
void          mt_platform_release(void* view);

// ---- renderer.mm ----------------------------------------------------------------------------

} } // namespace kv::KV_RB_NS
