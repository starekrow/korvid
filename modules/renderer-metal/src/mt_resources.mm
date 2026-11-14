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
    if (obj) r->frames[r->frame_index].garbage.push(obj);
}

// The slot's last frame has completed: what it owned goes; what was queued since stays for
// the frame being opened.
void rb_run_garbage(Renderer*, FrameInFlight& f) {
    u32 n = f.garbage_owned <= f.garbage.len ? f.garbage_owned : f.garbage.len;
    for (u32 i = 0; i < n; ++i) [f.garbage[i] release];
    if (n) {
        u32 rest = f.garbage.len - n;
        if (rest) memmove(f.garbage.data, f.garbage.data + n, sizeof(id) * rest);
        f.garbage.len = rest;
    }
    f.garbage_owned = 0;
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

bool rb_ring_init(Renderer* r, usize bytes) {
    FrameRing& g = r->ring;
    g.buf = rb_buffer(r, bytes, MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined, "upload ring");
    if (!g.buf) return false;
    g.size = bytes;
    g.head = g.tail = g.used = g.frame_bytes = 0;
    return true;
}

namespace {

bool ring_grow(Renderer* r, usize need) {
    FrameRing& g = r->ring;
    usize cap = g.size * 2;
    while (cap < need * 2) cap *= 2;
    // Spans handed out so far point at the old buffer, which lives until this frame
    // completes; the accounting restarts in the new one.
    rb_defer_release(r, g.buf);
    g.buf = rb_buffer(r, cap, MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined, "upload ring");
    if (!g.buf) return false;
    g.size = cap;
    g.head = g.tail = g.used = 0;
    g.frame_bytes = 0;
    for (FrameInFlight& f : r->frames) f.ring_bytes = 0;
    ++g.grows;
    KV_INFO("metal", "frame ring grown to %llu KB", (unsigned long long)(cap / 1024));
    return true;
}

} // namespace

RingSpan rb_ring_alloc(Renderer* r, usize bytes, usize align) {
    FrameRing& g = r->ring;
    RingSpan out;
    if (bytes == 0) return out;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        usize cap  = g.size;
        usize head = (g.head + align - 1) / align * align;
        usize gap  = head - g.head;
        bool ok = false;
        if (g.used == 0) {
            g.head = g.tail = 0; head = 0; gap = 0;
            ok = bytes <= cap;
        } else if (g.head >= g.tail) {
            if (head + bytes <= cap) ok = true;
            else if (bytes < g.tail) { gap = cap - g.head; head = 0; ok = true; }
        } else {
            ok = head + bytes < g.tail;
        }
        if (ok) {
            out.buffer = g.buf;
            out.offset = head;
            out.ptr    = (u8*)g.buf.contents + head;
            g.head     = head + bytes;
            if (g.head == cap) g.head = 0;
            g.used        += gap + bytes;
            g.frame_bytes += gap + bytes;
            if (g.used > g.peak) g.peak = g.used;
            return out;
        }
        if (!ring_grow(r, bytes + align)) return RingSpan{};
    }
    return out;
}

void rb_ring_submitted(Renderer* r, FrameInFlight& f) {
    f.ring_end   = r->ring.head;
    f.ring_bytes = r->ring.frame_bytes;
    r->ring.frame_bytes = 0;
}

void rb_ring_release(Renderer* r, FrameInFlight& f) {
    FrameRing& g = r->ring;
    if (!f.submitted || f.ring_bytes == 0) return;
    g.used -= f.ring_bytes;
    g.tail  = f.ring_end;
    f.ring_bytes = 0;
    if (g.used == 0) g.head = g.tail = 0;
}

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

// A slot's contents changed. Each frame's argument buffer is rewritten once that frame's
// command buffer has completed, so no frame in flight sees an entry change under it.
void rb_mark_slot_dirty(Renderer* r, u32 slot) {
    for (FrameInFlight& f : r->frames) f.dirty_slots.push(slot);
}

void rb_write_texture_slot(Renderer* r, FrameInFlight& f, u32 slot) {
    MTLResourceID* ids = (MTLResourceID*)((u8*)f.bindless.contents + kBindlessTextures);
    ids[slot] = slot_texture(r, slot).gpuResourceID;
}

// The texture for a record's size and format, private to the GPU; a previous one is
// released after the frames in flight.
bool rb_create_texture(Renderer* r, Texture& t) {
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
        RingSpan span = rb_ring_alloc(r, bytes, 16);
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

// The red and white checkerboard at slot 0, and the default material at row 0, as
// renderer-vulkan's. Its upload rides the first frame like any other.
bool rb_create_default_texture(Renderer* r) {
    Texture t{};
    t.width = t.height = kDefaultTexSize;
    t.format = TextureFormat::RGBA8_SRGB;
    t.name = name_intern("default");
    Handle h = r->textures.create(t);
    if (h.index() != 0) { KV_ERROR("metal", "default texture is not slot 0"); return false; }
    Texture& slot0 = r->textures.items[0];
    if (!rb_create_texture(r, slot0)) return false;

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
    if (!rb_stage_texture(r, 0, slot0, pixels)) return false;
    for (FrameInFlight& f : r->frames)
        for (u32 slot = 0; slot < kMaxTextures; ++slot) rb_write_texture_slot(r, f, slot);

    MaterialRecord m{};
    m.texture = h;
    m.blend   = BlendMode::Alpha;
    m.slot    = (u32)BlendMode::Alpha;
    m.tint    = color_white();
    m.name    = name_intern("default");
    r->default_material = r->materials.create(m);
    r->texture_by_name.set(t.name.hash, h);
    r->material_by_name.set(m.name.hash, r->default_material);
    if (r->default_material.index() != 0) { KV_ERROR("metal", "default material is not row 0"); return false; }
    rb_mark_material_dirty(r, 0);
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

// ---- materials ------------------------------------------------------------------------------

void rb_material_row(Renderer* r, u32 row, GpuMaterial& out) {
    out = GpuMaterial{};
    const MaterialRecord* m = r->materials.is_alive(row) ? &r->materials.items[row] : nullptr;
    if (!m) m = &r->materials.items[0];
    out.tint[0] = m->tint.r; out.tint[1] = m->tint.g; out.tint[2] = m->tint.b; out.tint[3] = m->tint.a;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out.params[i] = m->params[i];
    out.texture = renderer_texture_slot(r, m->texture);
    out.blend   = (u32)m->blend;
    out.filter  = (u32)m->filter;
    out.slot    = m->slot;
}

void rb_mark_material_dirty(Renderer* r, u32 row) {
    r->material_dirty[row >> 6] |= (u64)1 << (row & 63);
}

void rb_rebind_material(Renderer* r, u32 row) {
    u32 slot = r->materials.is_alive(row) ? r->materials.items[row].slot : r->materials.items[0].slot;
    for (u32 i = 0; i < r->sprites.len; ++i) {
        if (!r->sprites.is_alive(i)) continue;
        RenderSprite& s = r->sprites.items[i];
        if (s.gpu.material != row) continue;
        if (!r->materials.is_alive(row)) s.gpu.material = 0;
        u32 bucket = rb_bucket_of(s.gpu.layer, slot);
        if (bucket != s.bucket) {
            --r->bucket_counts[s.bucket];
            ++r->bucket_counts[bucket];
            if (s.keyed) { --r->keyed_counts[s.bucket]; ++r->keyed_counts[bucket]; }
            s.bucket = bucket;
        }
        r->sprite_dirty[i >> 6] |= (u64)1 << (i & 63);
    }
}

// ---- handle kinds and pools -----------------------------------------------------------------

namespace {

void* texture_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->textures.get(Handle{h}); }
int texture_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    Texture* t = r->textures.get(Handle{h});
    if (!t) return kv::format(out, cap, "stale");
    return kv::format(out, cap, "'%s' %ux%u slot %u %s", name_str(t->name), t->width, t->height, Handle{h}.index(),
                    t->resident ? "resident" : "placeholder");
}
void* material_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    MaterialRecord* m = r->materials.get(Handle{h});
    if (!m) return kv::format(out, cap, "stale");
    return kv::format(out, cap, "'%s' %s texture slot %u row %u", name_str(m->name), blend_mode_name(m->blend), renderer_texture_slot(r, m->texture), Handle{h}.index());
}
const char* texture_name_of(kv_handle h, void* ctx) {
    Texture* t = ((Renderer*)ctx)->textures.get(Handle{h});
    return t ? name_str(t->name) : nullptr;
}
kv_handle texture_find_name(const char* name, void* ctx) {
    Renderer* r = (Renderer*)ctx;
    Handle* h = r->texture_by_name.find(name_lookup(name).hash);
    return h && r->textures.valid(*h) ? h->bits : 0;
}
const char* material_name_of(kv_handle h, void* ctx) {
    MaterialRecord* m = ((Renderer*)ctx)->materials.get(Handle{h});
    return m ? name_str(m->name) : nullptr;
}
kv_handle material_find_name(const char* name, void* ctx) {
    Renderer* r = (Renderer*)ctx;
    Handle* h = r->material_by_name.find(name_lookup(name).hash);
    return h && r->materials.valid(*h) ? h->bits : 0;
}

} // namespace

bool rb_create_resource_pools(Renderer* r) {
    kv_handle_kind_desc td{};
    td.name = "render.texture"; td.type_key = type_key<Texture>(); td.ctx = r;
    td.resolve = texture_resolve; td.describe = texture_describe;
    td.name_of = texture_name_of; td.find_name = texture_find_name; td.flags = KV_HANDLE_GENERATIONAL;
    r->texture_kind = handle_kind_register(td);
    kv_handle_kind_desc md{};
    md.name = "render.material"; md.type_key = type_key<MaterialRecord>(); md.ctx = r;
    md.resolve = material_resolve; md.describe = material_describe;
    md.name_of = material_name_of; md.find_name = material_find_name; md.flags = KV_HANDLE_GENERATIONAL;
    r->material_kind = handle_kind_register(md);
    if (!r->texture_kind || !r->material_kind) return false;
    r->textures.use_kind(r->texture_kind);
    r->materials.use_kind(r->material_kind);
    r->sprite_of_entity.init(r->general);
    r->texture_by_name.init(r->general);
    r->material_by_name.init(r->general);
    r->sprite_dirty.alloc = r->material_dirty.alloc = r->uploads.alloc = r->general;
    r->material_dirty.resize_fill((kMaxMaterials + 63) / 64, 0);
    return r->textures.init(kMaxTextures, r->general->tag) && r->materials.init(kMaxMaterials, r->general->tag) &&
           r->sprites.init(kMaxSprites, r->general->tag);
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
    r->sprite_dirty.resize_fill(cap / 64, 0);
    for (u32 w = 0; w < (r->sprites.len + 63) / 64; ++w) r->sprite_dirty[w] = ~(u64)0; // the new table knows nothing
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
    if (r->texture_kind)  kv_handle_kind_unregister(r->texture_kind);
    if (r->material_kind) kv_handle_kind_unregister(r->material_kind);
    r->texture_kind = r->material_kind = 0;
    r->textures.free();
    r->materials.free();
    r->texture_by_name.free();
    r->material_by_name.free();
    r->sprites.free();
    r->sprite_of_entity.free();
    r->sprite_dirty.free();
    r->material_dirty.free();
    r->uploads.free();
}

} // namespace kv
