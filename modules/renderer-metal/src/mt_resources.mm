// Korvid renderer-metal: buffers, the frame ring, deferred release, textures, materials, the
// handle kinds and pools, the tables and the argument buffers. renderer-vulkan's
// vk_resources.cc with Metal's calls: memory is unified, so a buffer is shared (the CPU
// writes it) or private (only the GPU does), and there are no descriptor sets to write.
#include "renderer_internal.h"
#include "korvid/core/handle.h"
#include "korvid/core/format.h"
#include <string.h>

namespace kv {

// ---- buffers and deferred release -----------------------------------------------------------

id<MTLBuffer> rb_buffer(Renderer* r, usize bytes, MTLResourceOptions storage, const char* label) {
    id<MTLBuffer> b = [r->device newBufferWithLength:bytes ? bytes : 16 options:storage];
    if (!b) { KV_ERROR("metal", "out of memory for a %llu byte buffer (%s)", (unsigned long long)bytes, label); return nil; }
    if (label) b.label = @(label); // the names a GPU capture shows, in every build: free without one
    return b;
}

// Queued on the slot of the next frame to be submitted (the one being recorded, or between
// frames the one about to be), and released once that frame has completed: after every
// earlier frame on the queue too. An object queued between frames may be what that frame
// uses (a staging buffer its upload reads), so it must not go at the wait that opens it.
void rb_defer_release(Renderer* r, id obj) {
    if (obj) rc_defer(r, obj);
}

void rb_release_all(Renderer*, FrameInFlight& f) {
    for (id obj : f.garbage) [obj release];
    f.garbage.clear();
    f.garbage_owned = 0;
}

// ---- the frame ring -------------------------------------------------------------------------
// As renderer-vulkan's: [tail, head) is owned by frames in flight, the frame being recorded
// included; `used` counts those bytes with wrap gaps so head == tail is unambiguous. A
// frame's allocations are contiguous in ring order, so releasing a frame moves the tail to
// where it ended. The CPU writes spans; a blit copies them where they go.

// ---- textures -------------------------------------------------------------------------------

namespace {

MTLPixelFormat pixel_format(TextureFormat f) {
    return f == TextureFormat::RGBA8_UNORM ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA8Unorm_sRGB;
}

// The texture a slot should show right now: its own, or the default's.
id<MTLTexture> slot_texture(Renderer* r, u32 slot) {
    const Texture* t = slot < r->textures.len && r->textures.is_alive(slot) ? &r->textures.items[slot] : nullptr;
    if (t && t->tex) return t->tex;
    return r->textures.items[0].tex;
}

} // namespace

void rb_defer_destroy_texture(Renderer* r, Texture& t) {
    rb_defer_release(r, t.tex);
    t.tex = nil;
    t.resident = false;
}

void rb_write_texture_slot(Renderer* r, FrameInFlight& f, u32 slot) {
    MTLResourceID* ids = (MTLResourceID*)((u8*)f.bindless.contents + kBindlessTextures);
    ids[slot] = slot_texture(r, slot).gpuResourceID;
}

// The texture for a record's size and format, private to the GPU; a previous one is
// released after the frames in flight.
bool rb_texture_create(Renderer* r, Texture& t) {
    if (t.tex) rb_defer_destroy_texture(r, t);
    @autoreleasepool {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format(t.format)
                                                                                     width:t.width height:t.height mipmapped:NO];
        d.usage       = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        t.tex = [r->device newTextureWithDescriptor:d];
    }
    if (!t.tex) { KV_ERROR("metal", "cannot make a %ux%u texture for '%s'", t.width, t.height, name_str(t.name)); return false; }
    t.tex.label = @(name_str(t.name));
    return true;
}

// A rectangle into the live texture: not yet here (Krome slice 3); the caller uploads the
// whole image.
bool rb_stage_texture_region(Renderer*, u32, Texture&, const u8*, u32, u32, u32, u32) { return false; }

// Copies tightly packed pixels into the frame ring (or a one-off staging buffer when they
// would not fit) and queues the copy for the next frame's blit, ahead of its draws.
bool rb_stage_texture(Renderer* r, u32 slot, Texture& t, const u8* pixels) {
    usize bytes = (usize)t.width * t.height * 4;
    PendingUpload up{};
    up.slot   = slot;
    up.tex    = t.tex;
    up.width  = t.width;
    up.height = t.height;
    if (bytes <= r->ring.size / 2) {
        RingSpan span = rc_ring_alloc(r, bytes, 16);
        if (!span.ptr) return false;
        memcpy(span.ptr, pixels, bytes);
        up.src    = span.buffer;
        up.offset = span.offset;
    } else {
        id<MTLBuffer> staging = rb_buffer(r, bytes, MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined, "texture staging");
        if (!staging) return false;
        memcpy(staging.contents, pixels, bytes);
        up.src    = staging;
        up.offset = 0;
        rb_defer_release(r, staging); // alive until this frame completes, after the copy
    }
    r->uploads.push(up);
    t.resident = true;
    return true;
}

// The two samplers (nearest, linear; repeat, as renderer-vulkan's) and one argument buffer
// per frame in flight, the samplers written once.
bool rb_create_bindless(Renderer* r) {
    @autoreleasepool {
        MTLSamplerDescriptor* d = [[MTLSamplerDescriptor new] autorelease];
        d.minFilter = d.magFilter = MTLSamplerMinMagFilterNearest;
        d.mipFilter = MTLSamplerMipFilterNotMipmapped;
        d.sAddressMode = d.tAddressMode = d.rAddressMode = MTLSamplerAddressModeRepeat; // uv outside [0, 1] tiles
        d.supportArgumentBuffers = YES;
        d.label = @"nearest";
        r->sampler = [r->device newSamplerStateWithDescriptor:d];
        d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear;
        d.label = @"linear";
        r->sampler_linear = [r->device newSamplerStateWithDescriptor:d];
    }
    if (!r->sampler || !r->sampler_linear) { KV_ERROR("metal", "cannot make the samplers"); return false; }
    for (FrameInFlight& f : r->frames) {
        f.bindless = rb_buffer(r, kBindlessBytes, MTLResourceStorageModeShared, "bindless textures");
        if (!f.bindless) return false;
        memset(f.bindless.contents, 0, kBindlessBytes);
        MTLResourceID* samplers = (MTLResourceID*)((u8*)f.bindless.contents + kBindlessSamplers);
        samplers[0] = r->sampler.gpuResourceID;
        samplers[1] = r->sampler_linear.gpuResourceID;
    }
    return true;
}

// Argument-buffer entries are not resources the encoder can see: every texture a draw may
// sample is declared for the pass. At most kMaxTextures, a few microseconds.
void rb_use_textures(Renderer* r, id<MTLRenderCommandEncoder> enc) {
    id<MTLResource> list[kMaxTextures];
    NSUInteger n = 0;
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i) && r->textures.items[i].tex) list[n++] = r->textures.items[i].tex;
    if (n) [enc useResources:list count:n usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
}

// ---- the GPU tables ------------------------------------------------------------------------------

// Sprite tables for at least `rows` rows (rounded up to whole blocks). Previous tables are
// released after the frame; every row is marked for upload.
bool rb_create_tables(Renderer* r, u32 rows) {
    SpriteTables& t = r->tables;
    u32 cap = kBlockRows;
    while (cap < rows) cap *= 2;
    if (cap > kMaxSprites) cap = kMaxSprites;
    if (t.capacity) {
        rb_defer_release(r, t.sprites);
        rb_defer_release(r, t.bucket_of);
        rb_defer_release(r, t.block_hist);
        rb_defer_release(r, t.sorted);
    }
    t.capacity = cap;
    const MTLResourceOptions gpu = MTLResourceStorageModePrivate;
    t.sprites    = rb_buffer(r, sizeof(GpuSprite) * cap, gpu, "sprite rows");
    t.bucket_of  = rb_buffer(r, sizeof(u32) * cap, gpu, "sprite buckets");
    t.block_hist = rb_buffer(r, sizeof(u32) * (usize)t.blocks() * kBucketCount, gpu, "sort block counts");
    t.sorted     = rb_buffer(r, sizeof(u32) * cap, gpu, "sorted rows");
    if (!t.sprites || !t.bucket_of || !t.block_hist || !t.sorted) return false;
    r->world.sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->world.sprites.len + 63) / 64; ++w) r->world.sprite_dirty[w] = ~(u64)0; // the new table knows nothing
    KV_INFO("metal", "sprite tables: %u rows, %u blocks, %llu KB", cap, t.blocks(),
            (unsigned long long)((t.sprites.length + t.bucket_of.length + t.block_hist.length + t.sorted.length) / 1024));
    return true;
}

bool rb_create_static_buffers(Renderer* r) {
    for (FrameInFlight& f : r->frames) {
        f.layers = rb_buffer(r, sizeof(f32) * 4 * kLayerCount, MTLResourceStorageModeShared, "layer offsets");
        if (!f.layers) return false;
        memset(f.layers.contents, 0, sizeof(f32) * 4 * kLayerCount);
    }
    r->material_table = rb_buffer(r, sizeof(GpuMaterial) * kMaxMaterials, MTLResourceStorageModePrivate, "material rows");
    r->sort_meta      = rb_buffer(r, sizeof(u32) * (2 * kBucketCount + 4), MTLResourceStorageModePrivate, "sort totals");
    r->indirect       = rb_buffer(r, sizeof(MTLDrawPrimitivesIndirectArguments) * kBucketCount, MTLResourceStorageModePrivate, "indirect draws");
    static_assert(sizeof(MTLDrawPrimitivesIndirectArguments) == 16, "the sort writes a uint4 per bucket");
    return r->material_table && r->sort_meta && r->indirect;
}

// Nothing in flight: textures, tables, ring, samplers, argument buffers, pools.
void rb_destroy_resources(Renderer* r) {
    for (u32 i = 0; i < r->textures.len; ++i)
        if (r->textures.is_alive(i)) { [r->textures.items[i].tex release]; r->textures.items[i].tex = nil; }
    auto drop = [](id<MTLBuffer>& b) { [b release]; b = nil; };
    drop(r->tables.sprites); drop(r->tables.bucket_of); drop(r->tables.block_hist); drop(r->tables.sorted);
    drop(r->material_table); drop(r->sort_meta); drop(r->indirect); drop(r->ring.buf); drop(r->check_buffer);
    for (FrameInFlight& f : r->frames) {
        [f.bindless release]; f.bindless = nil;
        [f.layers release];   f.layers = nil;
    }
    [r->sampler release];        r->sampler = nil;
    [r->sampler_linear release]; r->sampler_linear = nil;
    rc_free_pools(r);
    r->uploads.free();
}

// ---- what the render core asks of a buffer and of the garbage queue (core.h) ----------------------

// A buffer in private storage has no contents; nor has no buffer.
void* rb_buffer_ptr(const RbBuffer& b) { return b.contents; }
void  rb_buffer_retire(Renderer* r, RbBuffer& b) { rb_defer_release(r, b); b = nil; }

bool rb_ring_buffer(Renderer* r, u64 bytes, RbBuffer& out) {
    out = rb_buffer(r, (usize)bytes, MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined, "upload ring");
    return out != nil;
}

void rb_garbage_destroy(Renderer*, const RbGarbage& g) { [g release]; }

} // namespace kv
