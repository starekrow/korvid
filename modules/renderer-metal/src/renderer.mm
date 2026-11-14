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

u32 rb_material_row_of(const Renderer* r, Handle material) {
    return r->materials.valid(material) ? material.index() : 0;
}

u32 rb_bucket_of(i32 layer, u32 slot) {
    if (layer < kLayerMin) layer = kLayerMin;
    if (layer > kLayerMax) layer = kLayerMax;
    u32 b = slot < kSlotCount ? slot : (u32)BlendMode::Alpha;
    return (u32)(layer - kLayerMin) * kSlotCount + b;
}

const char* blend_mode_name(BlendMode mode) {
    switch (mode) {
    case BlendMode::Opaque:   return "opaque";
    case BlendMode::Alpha:    return "alpha";
    case BlendMode::Additive: return "additive";
    default:                  return "?";
    }
}

bool blend_mode_parse(const char* s, BlendMode* out) {
    if (!s) return false;
    if (strcmp(s, "opaque") == 0)   { *out = BlendMode::Opaque;   return true; }
    if (strcmp(s, "alpha") == 0)    { *out = BlendMode::Alpha;    return true; }
    if (strcmp(s, "additive") == 0) { *out = BlendMode::Additive; return true; }
    return false;
}

namespace {

void mark_sprite_dirty(Renderer* r, u32 slot) {
    u32 w = slot >> 6;
    if (w >= r->sprite_dirty.len) r->sprite_dirty.resize_fill(w + 1, 0);
    r->sprite_dirty[w] |= (u64)1 << (slot & 63);
}

void write_placement(f32 out[4], f32* scale_y, const Placement2& p) {
    out[0] = p.position.x; out[1] = p.position.y; out[2] = p.rotation; out[3] = p.scale.x;
    *scale_y = p.scale.y;
}

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
struct Shot {
    char          path[512] = {};
    bool          pending = false, recorded = false;
    id<MTLBuffer> buffer = nil;
    u32           width = 0, height = 0;
};
Shot g_shot;

void record_screenshot(Renderer* r, id<MTLCommandBuffer> cb, id<MTLTexture> image) {
    g_shot.pending = false;
    if (r->layer.framebufferOnly) {
        if (!r->shot_warned) KV_WARN("metal", "screenshots need a dev or debug build (the layer's drawables are framebuffer-only)");
        r->shot_warned = true;
        return;
    }
    u32 w = (u32)image.width, h = (u32)image.height;
    rb_defer_release(r, g_shot.buffer);
    g_shot.buffer = rb_buffer(r, (usize)w * h * 4, MTLResourceStorageModeShared, "screenshot");
    if (!g_shot.buffer) return;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    blit.label = @"screenshot";
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:g_shot.buffer destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [blit endEncoding];
    g_shot.width = w;
    g_shot.height = h;
    g_shot.recorded = true;
}

// Binary PPM, RGB, from the drawable's BGRA (sRGB-encoded bytes).
void write_screenshot(Renderer* r) {
    g_shot.recorded = false;
    kv::IoFileWriter f;
    if (!f.open(g_shot.path)) { KV_ERROR("metal", "cannot write screenshot '%s'", g_shot.path); return; }
    f.print("P6\n%u %u\n255\n", g_shot.width, g_shot.height);
    const u8* px = (const u8*)g_shot.buffer.contents;
    MemScope scope;
    u8* row = arena_push_n<u8>(*scope.arena, g_shot.width * 3);
    for (u32 y = 0; y < g_shot.height; ++y) {
        const u8* src = px + (usize)y * g_shot.width * 4;
        for (u32 x = 0; x < g_shot.width; ++x, src += 4) { row[x * 3 + 0] = src[2]; row[x * 3 + 1] = src[1]; row[x * 3 + 2] = src[0]; }
        f.write(row, g_shot.width * 3);
    }
    if (!f.close()) KV_ERROR("metal", "screenshot '%s': write failed (errno %d)", g_shot.path, f.error);
    else KV_INFO("metal", "screenshot '%s' %ux%u", g_shot.path, g_shot.width, g_shot.height);
    rb_defer_release(r, g_shot.buffer);
    g_shot.buffer = nil;
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
        ok = ok && rb_create_resource_pools(r);
        ok = ok && rb_create_tables(r, desc.sprite_rows > kInitialSpriteRows ? desc.sprite_rows : kInitialSpriteRows);
        ok = ok && rb_create_static_buffers(r);
        usize ring = (usize)desc.sprite_rows * sizeof(GpuSprite) * kFramesInFlight;
        ok = ok && rb_ring_init(r, ring > kInitialRingBytes ? ring : kInitialRingBytes);
        ok = ok && rb_create_bindless(r);
        ok = ok && rb_load_shaders(r);
        ok = ok && rb_create_default_texture(r);
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
    [g_shot.buffer release];
    g_shot = Shot{};
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
            (unsigned long long)r->frame_count, (unsigned long long)r->deltas_applied, (unsigned long long)r->rows_uploaded,
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

RenderCaps renderer_caps(const Renderer*) {
    RenderCaps c;
    c.backend          = "metal";
    c.max_textures     = kMaxTextures;
    c.max_materials    = kMaxMaterials;
    c.frames_in_flight = kFramesInFlight;
    c.srgb_output      = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_SRGB]  = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_UNORM] = true;
    return c;
}

namespace {

template <class T>
void rename_after_destroy(Pool<T>& pool, Map<u64, Handle>& by_name, Name name, Handle gone) {
    Handle* at = by_name.find(name.hash);
    if (!at || *at != gone) return;
    for (u32 i = 0; i < pool.len; ++i)
        if (pool.is_alive(i) && pool.items[i].name == name) { *at = pool.handle_at(i); return; }
    by_name.remove(name.hash);
}

} // namespace

// ---- textures -------------------------------------------------------------------------------

Handle renderer_texture_create(Renderer* r, const TextureDesc& desc) {
    if (!r || (u32)desc.format >= (u32)TextureFormat::Count) return Handle{};
    Texture t{};
    t.format = desc.format;
    ++r->textures_made;
    if (desc.name && desc.name[0]) t.name = name_intern(desc.name);
    else {
        char derived[32];
        kv::format(derived, sizeof derived, "texture:%u", r->textures_made);
        t.name = name_intern(derived);
    }
    Handle h = r->textures.create(t);
    if (h.is_null()) { KV_ERROR("metal", "out of texture slots (%u)", kMaxTextures); return h; }
    r->texture_by_name.get_or_insert(t.name.hash, h);
    rb_mark_slot_dirty(r, h.index());
    return h;
}

bool renderer_texture_upload(Renderer* r, Handle texture, u32 width, u32 height, const void* pixels, usize bytes) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0 || !pixels) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || bytes != (usize)width * height * 4) {
        KV_ERROR("metal", "texture '%s': %ux%u with %llu bytes is not a tightly packed RGBA8 image",
                 name_str(t->name), width, height, (unsigned long long)bytes);
        return false;
    }
    t->width  = width;
    t->height = height;
    if (!rb_create_texture(r, *t)) return false;
    if (!rb_stage_texture(r, texture.index(), *t, (const u8*)pixels)) return false;
    rb_mark_slot_dirty(r, texture.index());
    return true;
}

void renderer_texture_destroy(Renderer* r, Handle texture) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0) return;
    rb_defer_destroy_texture(r, *t);
    u32 slot = texture.index();
    Name name = t->name;
    r->textures.destroy(texture);
    rename_after_destroy(r->textures, r->texture_by_name, name, texture);
    rb_mark_slot_dirty(r, slot);
}

u32 renderer_texture_slot(const Renderer* r, Handle texture) {
    return r && r->textures.valid(texture) ? texture.index() : 0;
}

// ---- materials ------------------------------------------------------------------------------

namespace {

Name derived_material_name(const Renderer* r, const MaterialRecord& m) {
    const Texture* t = r->textures.get(m.texture);
    u64 h = hash_mix(0x6d6174657269616cull ^ (t ? t->name.hash : 0));
    h = hash_mix(h ^ (u64)m.blend ^ ((u64)m.filter << 8));
    h = hash_mix(h ^ hash_bytes(&m.tint, sizeof m.tint));
    h = hash_mix(h ^ hash_bytes(m.params, sizeof m.params));
    h = hash_mix(h ^ m.shader.hash);
    char text[32];
    kv::format(text, sizeof text, "material:%016llx", (unsigned long long)h);
    return name_intern(text);
}

void fill_material(Renderer* r, MaterialRecord& m, const MaterialDesc& desc) {
    m.texture = desc.texture;
    m.blend   = (u32)desc.blend < (u32)BlendMode::Count ? desc.blend : BlendMode::Alpha;
    m.shader  = desc.shader && desc.shader[0] ? name_intern(desc.shader) : Name{};
    m.slot    = rb_material_slot(r, m.shader, m.blend);
    m.filter  = (u32)desc.filter < (u32)TextureFilter::Count ? desc.filter : TextureFilter::Nearest;
    m.tint    = desc.tint;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) m.params[i] = desc.params[i];
}

} // namespace

Handle renderer_material_create(Renderer* r, const MaterialDesc& desc) {
    if (!r) return Handle{};
    MaterialRecord m{};
    fill_material(r, m, desc);
    if (desc.name && desc.name[0]) m.name = name_intern(desc.name);
    else {
        m.name = derived_material_name(r, m);
#if KV_DEV
        bool inserted = false;
        r->material_by_name.get_or_insert(m.name.hash, Handle{}, &inserted);
        if (inserted) { r->material_by_name.remove(m.name.hash); KV_INFO("metal", "material named %s by its content", name_str(m.name)); }
#endif
    }
    Handle h = r->materials.create(m);
    if (h.is_null()) { KV_ERROR("metal", "out of materials (%u)", kMaxMaterials); return h; }
    r->material_by_name.get_or_insert(m.name.hash, h);
    rb_mark_material_dirty(r, h.index());
    return h;
}

bool renderer_material_set(Renderer* r, Handle material, const MaterialDesc& desc) {
    MaterialRecord* m = r ? r->materials.get(material) : nullptr;
    if (!m) return false;
    u32 before = m->slot;
    fill_material(r, *m, desc);
    if (m->slot != before) rb_rebind_material(r, material.index());
    rb_mark_material_dirty(r, material.index());
    return true;
}

bool renderer_material_get(const Renderer* r, Handle material, MaterialDesc* out) {
    const MaterialRecord* m = r ? r->materials.get(material) : nullptr;
    if (!m || !out) return false;
    out->texture = m->texture;
    out->blend   = m->blend;
    out->filter  = m->filter;
    out->tint    = m->tint;
    out->name    = name_str(m->name);
    out->shader  = m->shader.is_null() ? nullptr : name_str(m->shader);
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out->params[i] = m->params[i];
    return true;
}

void renderer_material_destroy(Renderer* r, Handle material) {
    if (!r || material == r->default_material || !r->materials.valid(material)) return;
    u32 row = material.index();
    Name name = r->materials.get(material)->name;
    r->materials.destroy(material);
    rename_after_destroy(r->materials, r->material_by_name, name, material);
    rb_rebind_material(r, row);
    rb_mark_material_dirty(r, row);
}

bool renderer_post_layer(Renderer* r, i32 layer, const char* shader) {
    if (!r) return false;
    return rb_post_set(r, layer, shader && *shader ? name_intern(shader) : Name{});
}

void renderer_post_params(Renderer* r, const f32* params, u32 count) {
    if (!r) return;
    if (count > KV_POST_PARAMS) count = KV_POST_PARAMS;
    if (count) memcpy(r->post.params, params, sizeof(f32) * 4 * count);
    r->post.param_count = count;
}

Handle renderer_default_material(const Renderer* r) { return r ? r->default_material : Handle{}; }

f64 renderer_last_wait(const Renderer* r) { return r ? r->last_wait : 0.0; }

bool renderer_check_sort(Renderer* r) {
    if (!r) return false;
    r->check_pending = true;
    return true;
}

bool renderer_screenshot(Renderer* r, const char* path) {
    if (!r || !path || strlen(path) >= sizeof g_shot.path) return false;
    kv::format(g_shot.path, sizeof g_shot.path, "%s", path);
    g_shot.pending = true;
    return true;
}

// ---- the render world ---------------------------------------------------------------------

// The layer lives as long as its view on every Apple platform: nothing to let go of.
bool renderer_set_surface(Renderer*, void*) { return true; }

void renderer_resize(Renderer* r, u32 width, u32 height) {
    if (!r) return;
    r->want_width  = width;
    r->want_height = height;
    r->size_dirty  = true;
}

void renderer_apply(Renderer* r, Span<const SpriteDelta> deltas) {
    if (!r) return;
    KV_ZONE("apply");
    for (const SpriteDelta& d : deltas) {
        ++r->deltas_applied;
        switch (d.kind) {
        case SpriteDeltaKind::Tick:
            ++r->tick;
            memcpy(r->layer_prev, r->layer_cur, sizeof r->layer_prev);
            break;
        case SpriteDeltaKind::Layer: {
            i32 l = d.state.layer < -128 ? -128 : d.state.layer > 127 ? 127 : d.state.layer;
            f32* cur = r->layer_cur[l + 128];
            cur[0] = d.state.placement.position.x;
            cur[1] = d.state.placement.position.y;
            if (d.state.snap) memcpy(r->layer_prev[l + 128], cur, sizeof(f32) * 2);
            break;
        }
        case SpriteDeltaKind::Set: {
            const RenderSpriteState& st = d.state;
            u32 material = rb_material_row_of(r, st.material);
            u32 bucket   = rb_bucket_of(st.layer, r->materials.items[material].slot);
            RenderSprite* s;
            u32 slot;
            if (u32* found = r->sprite_of_entity.find(d.entity)) {
                slot = *found;
                s = &r->sprites.items[slot];
                if (s->gpu.stamp != r->tick) {
                    memcpy(s->gpu.prev, s->gpu.cur, sizeof s->gpu.prev);
                    s->gpu.scale_y[0] = s->gpu.scale_y[1];
                    s->gpu.stamp = r->tick;
                }
                if (st.snap) write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement);
                if (bucket != s->bucket) {
                    --r->bucket_counts[s->bucket];
                    ++r->bucket_counts[bucket];
                    if (s->keyed) { --r->keyed_counts[s->bucket]; ++r->keyed_counts[bucket]; }
                    s->bucket = bucket;
                }
            } else {
                RenderSprite fresh{};
                fresh.entity = d.entity;
                fresh.bucket = bucket;
                Handle h = r->sprites.create(fresh);
                if (h.is_null()) { KV_ERROR("metal", "render world is full (%u sprites)", kMaxSprites); break; }
                slot = h.index();
                s = &r->sprites.items[slot];
                r->sprite_of_entity.set(d.entity, slot);
                ++r->bucket_counts[bucket];
                write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement);
                s->gpu.stamp = r->tick;
            }
            write_placement(s->gpu.cur, &s->gpu.scale_y[1], st.placement);
            s->gpu.size[0] = st.size.x; s->gpu.size[1] = st.size.y;
            s->gpu.uv[0] = st.uv.x; s->gpu.uv[1] = st.uv.y; s->gpu.uv[2] = st.uv.w; s->gpu.uv[3] = st.uv.h;
            s->gpu.tint[0] = st.tint.r; s->gpu.tint[1] = st.tint.g; s->gpu.tint[2] = st.tint.b; s->gpu.tint[3] = st.tint.a;
            s->gpu.material = material;
            s->gpu.layer    = st.layer;
            i32 ax = 0, ay = 0;
            screen_anchor_dir(st.anchor, &ax, &ay);
            u32 anchor = (u32)(ax < 0 ? 1 : ax > 0 ? 2 : 0) | (u32)(ay < 0 ? 1 : ay > 0 ? 2 : 0) << 2;
            s->gpu.flags    = kSpriteAlive | (st.space == SpriteSpace::Screen ? kSpriteScreen : 0) | ((u32)st.sort << kSpriteSortShift) |
                              (anchor << kSpriteAnchorShift);
            s->gpu.order    = st.order;
            bool keyed = st.sort != SpriteSort::Row;
            if (keyed != s->keyed) { if (keyed) ++r->keyed_counts[bucket]; else --r->keyed_counts[bucket]; s->keyed = keyed; }
            mark_sprite_dirty(r, slot);
            break;
        }
        case SpriteDeltaKind::Remove: {
            if (u32* found = r->sprite_of_entity.find(d.entity)) {
                u32 slot = *found;
                --r->bucket_counts[r->sprites.items[slot].bucket];
                if (r->sprites.items[slot].keyed) --r->keyed_counts[r->sprites.items[slot].bucket];
                r->sprites.destroy(r->sprites.handle_at(slot));
                r->sprite_of_entity.remove(d.entity);
                mark_sprite_dirty(r, slot);
            }
            break;
        }
        }
    }
}

u32 renderer_sprite_count(const Renderer* r) { return r ? r->sprites.live : 0; }

// ---- the frame ------------------------------------------------------------------------------

bool renderer_render(Renderer* r, const RenderView& view) {
    if (!r) return false;
    FrameInFlight& f = r->frames[r->frame_index];
    f64 waited = 0.0;
    bool presented = false;
    @autoreleasepool {
        // This slot's previous frame: once it completes, what it queued for release can go,
        // its span of the ring is free, and its argument buffer may be rewritten. Vulkan's
        // fence wait.
        if (f.cb) {
            KV_ZONE_WAIT("frame completion");
            f64 t = time_now();
            [f.cb waitUntilCompleted];
            waited += time_now() - t;
            if (f.cb.status == MTLCommandBufferStatusError)
                KV_ERROR("metal", "frame failed on the GPU: %s", f.cb.error ? f.cb.error.localizedDescription.UTF8String : "?");
            rb_gpu_collect(r, f); // its timestamps, now that it has completed
            [f.cb release];
            f.cb = nil;
        }
        r->last_wait = 0.0;
        rb_ring_release(r, f);
        rb_run_garbage(r, f);
        if (r->sprites.len > r->tables.capacity) {
            KV_INFO("metal", "sprite tables grow at frame %llu: %u rows live, %u held", (unsigned long long)r->frame_count, r->sprites.len, r->tables.capacity);
            if (!rb_create_tables(r, r->sprites.len)) return false;
        }
        f32* lo = (f32*)f.layers.contents;
        for (u32 i = 0; i < kLayerCount; ++i) {
            lo[i * 4 + 0] = r->layer_prev[i][0]; lo[i * 4 + 1] = r->layer_prev[i][1];
            lo[i * 4 + 2] = r->layer_cur[i][0];  lo[i * 4 + 3] = r->layer_cur[i][1];
        }
        for (u32 slot : f.dirty_slots) rb_write_texture_slot(r, f, slot);
        f.dirty_slots.clear();

        // The drawable's size follows the window's, set before the next drawable is asked for.
        // A window with no area (minimized) has no drawable; the frame still sends its uploads.
        if (r->size_dirty) {
            if (r->want_width && r->want_height) r->layer.drawableSize = CGSizeMake(r->want_width, r->want_height);
            r->size_dirty = false;
        }
        id<CAMetalDrawable> drawable = nil;
        if (r->want_width && r->want_height) {
            KV_ZONE_WAIT("drawable");
            f64 t = time_now();
            drawable = [r->layer nextDrawable];
            waited += time_now() - t;
            if (!drawable) KV_WARN("metal", "no drawable this frame");
        }

        id<MTLCommandBuffer> cb = [r->queue commandBuffer];
        cb.label = @"frame";
        rb_gpu_frame_begin(r, f);
        if (!rb_record_frame(r, f, cb, drawable, view)) return false;
        if (drawable && g_shot.pending) record_screenshot(r, cb, drawable.texture);
        if (drawable) [cb presentDrawable:drawable];
        [cb commit];
        f.cb = [cb retain]; // waited for when this slot comes round again
        f.garbage_owned = f.garbage.len; // released when it has; anything queued later waits for the next
        f.submitted = true;
        rb_ring_submitted(r, f);
        presented = drawable != nil;

        if (g_shot.recorded || r->check_recorded) { // a dev stall: wait for this frame and read it back now
            [cb waitUntilCompleted];
            if (g_shot.recorded) write_screenshot(r);
            if (r->check_recorded) rb_verify_sort(r);
        }
    }
    r->last_wait = waited;
    r->frame_index = (r->frame_index + 1) % kFramesInFlight;
    ++r->frame_count;
    return presented;
}

} // namespace kv
