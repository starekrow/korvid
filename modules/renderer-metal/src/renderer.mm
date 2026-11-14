// Korvid renderer-metal: the Metal sprite renderer behind korvid/render/renderer.h, on the
// Apple family. renderer-vulkan's design and CPU side, ported as they are: a persistent GPU
// sprite table slot-addressed by the render world's pool, dirty rows copied in each frame,
// compute sorting and culling into buckets of (layer, pipeline slot) and writing one
// indirect draw per bucket, bindless textures, materials as rows of a GPU table. See
// renderer_internal.h for how the source is split and what differs.
#include "renderer_internal.h"
#include "korvid/core/arena.h"
#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/hash.h"
#include "korvid/renderer-metal/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include <string.h>

namespace kv {

// ---- shared helpers ----------------------------------------------------------------------

namespace {

// The layer: the device, the format the pipelines are made for, vsync, and drawables a
// screenshot can read in the builds that take them.
void configure_layer(Renderer* r) {
    CAMetalLayer* l = r->layer;
    l.device             = r->device;
    l.pixelFormat        = MTLPixelFormatBGRA8Unorm_sRGB; // the hardware encodes linear to sRGB, as the Vulkan swapchain does
    l.maximumDrawableCount = 3;
#if TARGET_OS_OSX
    l.displaySyncEnabled = r->vsync ? YES : NO;
#endif // iOS presents at the display's refresh, always
#if KV_DEV || KV_DEBUG
    l.framebufferOnly    = NO;  // --screenshot blits from the drawable
#else
    l.framebufferOnly    = YES;
#endif
}

// A requested screenshot of the drawable just drawn: a blit into a shared buffer, written as
// a PPM once the frame completes (the dev stall renderer-vulkan's has too).
void record_screenshot(Renderer* r, id<MTLCommandBuffer> cb, id<MTLTexture> image) {
    r->shot_pending = false;
    if (r->layer.framebufferOnly) {
        if (!r->shot_warned) KV_WARN("metal", "screenshots need a dev or debug build (the layer's drawables are framebuffer-only)");
        r->shot_warned = true;
        return;
    }
    u32 w = (u32)image.width, h = (u32)image.height;
    rb_defer_release(r, r->shot_buffer);
    r->shot_buffer = rb_buffer(r, (usize)w * h * 4, MTLResourceStorageModeShared, "screenshot");
    if (!r->shot_buffer) return;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    blit.label = @"screenshot";
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:r->shot_buffer destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [blit endEncoding];
    r->shot_width = w;
    r->shot_height = h;
    r->shot_recorded = true;
}

} // namespace

// ---- lifetime -------------------------------------------------------------------------------


Renderer* renderer_create(Engine& engine, const RendererDesc& desc) {
    RendererMetalState* owner = engine_module_state_as<RendererMetalState>(engine, "renderer-metal");
    if (!owner) { KV_ERROR("metal", "renderer-metal module is not started"); return nullptr; }
    if (owner->renderer) { KV_ERROR("metal", "a renderer already exists"); return nullptr; }
    Allocator* ga = owner->general;
    Renderer* r = (Renderer*)ga->alloc(ga, sizeof(Renderer), alignof(Renderer));
    *r = Renderer{}; // plain data: Metal objects are plain pointers here (no ARC)
    r->general      = ga;
    r->owner_slot   = &owner->renderer;
    owner->renderer = r;
    for (FrameInFlight& f : r->frames) { f.garbage.alloc = ga; f.dirty_slots.alloc = ga; }
    r->uploads.alloc = ga;
    r->window      = desc.native_window;
    if (const PlatformState* ps = engine_module_state_as<PlatformState>(engine, "platform"))
        kv::format(r->base_path, sizeof r->base_path, "%s", ps->base_path);
    r->start_time  = time_now();
    r->validation  = desc.validation;
    r->vsync       = desc.vsync;
    r->want_width  = desc.width;
    r->want_height = desc.height;

    bool ok = true;
    @autoreleasepool {
        r->device = MTLCreateSystemDefaultDevice();
        if (!r->device) { KV_ERROR("metal", "no Metal device"); ok = false; }
        if (ok) r->queue = [r->device newCommandQueue];
        if (ok && !r->queue) { KV_ERROR("metal", "cannot make a command queue"); ok = false; }
        if (ok) r->queue.label = @"korvid";
        if (ok) r->layer = mt_platform_layer(r->window, &r->metal_view);
        if (ok && !r->layer) ok = false;
        if (ok) configure_layer(r);
        ok = ok && rb_gpu_profile_create(r);
        ok = ok && rc_create_pools(r);
        ok = ok && rb_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
        ok = ok && rb_create_static_buffers(r);
        usize ring = (usize)desc.sprite_rows * sizeof(GpuSprite) * kFramesInFlight;
        ok = ok && rc_ring_init(r, ring > kInitialRingBytes ? ring : kInitialRingBytes);
        ok = ok && rb_create_bindless(r);
        ok = ok && rb_load_shaders(r);
        ok = ok && rc_create_defaults(r);
        ok = ok && rb_create_compute_pipelines(r);
        ok = ok && rb_create_pipelines(r);
    }
    if (!ok) {
        renderer_destroy(r);
        return nullptr;
    }
    // Validation is the environment's on Metal (MTL_DEBUG_LAYER, MTL_SHADER_VALIDATION, read
    // when the device is made); say whether it is on, so a debug run knows.
    const char* layer_env = getenv("MTL_DEBUG_LAYER");
    KV_INFO("metal", "renderer up on '%s': %u frames in flight, %u texture slots, %u material rows, %u buckets; API validation %s",
            r->device.name.UTF8String, kFramesInFlight, kMaxTextures, kMaxMaterials, kBucketCount,
            layer_env && layer_env[0] == '1' ? "on" : "off (MTL_DEBUG_LAYER=1 turns it on)");
    return r;
}

void renderer_destroy(Renderer* r) {
    if (!r) return;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = nullptr;
    // Every frame in flight completes, then everything goes.
    for (FrameInFlight& f : r->frames) {
        if (f.cb) { [f.cb waitUntilCompleted]; [f.cb release]; f.cb = nil; }
    }
    for (FrameInFlight& f : r->frames) { rb_release_all(r, f); f.garbage.free(); f.dirty_slots.free(); }
    [r->shot_buffer release];
    r->shot_buffer = nil;
    rb_post_destroy(r);
    rb_gpu_profile_destroy(r);
    rb_destroy_pipelines(r);
    rb_destroy_resources(r);
    mt_platform_release(r->metal_view);
    r->metal_view = nullptr;
    r->layer = nil;
    [r->queue release];  r->queue = nil;
    [r->device release]; r->device = nil;
    KV_INFO("metal", "renderer down after %llu frames, %llu deltas, %llu rows uploaded, ring peak %llu KB (%u grows)",
            (unsigned long long)r->frame_count, (unsigned long long)r->world.deltas_applied, (unsigned long long)r->rows_uploaded,
            (unsigned long long)(r->ring.peak / 1024), r->ring.grows);
    if (r->checks_passed || r->checks_failed) KV_INFO("metal", "sort checks: %u passed, %u failed", r->checks_passed, r->checks_failed);
    if (r->owner_slot) *r->owner_slot = nullptr;
    Allocator* ga = r->general;
    ga->free(ga, r, sizeof(Renderer));
}

Renderer* renderer_get(Engine& engine) {
    RendererMetalState* s = engine_module_state_as<RendererMetalState>(engine, "renderer-metal");
    return s ? s->renderer : nullptr;
}

// ---- textures -------------------------------------------------------------------------------

// ---- materials ------------------------------------------------------------------------------

// ---- the render world ---------------------------------------------------------------------

// The layer lives as long as its view on every Apple platform: nothing to let go of.
bool renderer_set_surface(Renderer*, void*) { return true; }

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width  = width;
    r->want_height = height;
    r->size_dirty  = true;
}

// ---- the frame ------------------------------------------------------------------------------

// The steps of the core's frame (korvid/render/core/frame.inl) that wait, acquire, submit
// and present. renderer_render itself is the core's, inside an @autoreleasepool
// (KV_RB_FRAME_SCOPE): the command buffer and the drawable live until it closes.

// This slot's previous frame: a command buffer waited on, where Vulkan waits on a fence.
bool rb_frame_wait(Renderer* r, FrameInFlight& f, f64* waited) {
    if (!f.cb) return true;
    {
        KV_ZONE_WAIT("frame completion");
        f64 t = time_now();
        [f.cb waitUntilCompleted];
        *waited += time_now() - t;
    }
    if (f.cb.status == MTLCommandBufferStatusError)
        KV_ERROR("metal", "frame failed on the GPU: %s", f.cb.error ? f.cb.error.localizedDescription.UTF8String : "?");
    rb_gpu_collect(r, f); // its timestamps, now that it has completed
    [f.cb release];
    f.cb = nil;
    return true;
}

void rb_frame_slot(Renderer* r, FrameInFlight& f, u32 slot) { rb_write_texture_slot(r, f, slot); }

// The drawable's size follows the window's, set before the next drawable is asked for. A
// window with no area (minimized) has no drawable; the frame still sends its uploads.
bool rb_frame_acquire(Renderer* r, FrameInFlight& f, f64* waited) {
    if (r->size_dirty) {
        if (r->want_width && r->want_height) r->layer.drawableSize = CGSizeMake(r->want_width, r->want_height);
        r->size_dirty = false;
    }
    f.drawable = nil;
    if (r->want_width && r->want_height) {
        KV_ZONE_WAIT("drawable");
        f64 t = time_now();
        f.drawable = [r->layer nextDrawable];
        *waited += time_now() - t;
        if (!f.drawable) KV_WARN("metal", "no drawable this frame");
    }
    f.have_image = f.drawable != nil;
    f.width  = f.drawable ? (u32)f.drawable.texture.width : 0;
    f.height = f.drawable ? (u32)f.drawable.texture.height : 0;
    return true;
}

void rb_read_window(Renderer* r, FrameInFlight& f) { record_screenshot(r, f.rec, f.drawable.texture); }

// Presenting is part of the command buffer: scheduled before it is committed.
bool rb_frame_submit(Renderer*, FrameInFlight& f) {
    if (f.drawable) [f.rec presentDrawable:f.drawable];
    [f.rec commit];
    f.cb = [f.rec retain]; // waited for when this slot comes round again
    f.rec = nil;
    return true;
}

bool rb_frame_present(Renderer*, FrameInFlight& f, f64*) {
    f.drawable = nil;
    return true;
}

bool rb_frame_finish(Renderer*, FrameInFlight& f) {
    [f.cb waitUntilCompleted];
    return true;
}

} // namespace kv
