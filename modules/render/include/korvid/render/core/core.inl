// Korvid render core: the sources of the compiled-in half. A backend's src/render_core.cc
// includes this after its renderer_internal.h (core.h says how), so everything here is
// built against that backend's Renderer, Texture, GamePipe and FrameInFlight, whole. Named
// .inl so that no build rule compiles it on its own.
//
// What is here is what the three backends had word for word: the functions of
// korvid/render/renderer.h that are about the world, the textures and the materials; the
// pools and their handle kinds; the game shaders' slots; the frame ring; the garbage queue;
// the CPU's half of --check-sort and --screenshot. What draws is the backends'.
#include "korvid/render/core/core.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform_hid/host_window.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

RendererSurface renderer_surface() { return KV_RB_SURFACE; }

u8 renderer_window_surface() {
    switch (KV_RB_SURFACE) {
    case RendererSurface::Metal: return (u8)WindowSurface::Metal;
    case RendererSurface::D3D12: return (u8)WindowSurface::D3D12;
    case RendererSurface::Software: return (u8)WindowSurface::Software;
    default:                     return (u8)WindowSurface::Vulkan;
    }
}

RenderCaps renderer_caps([[maybe_unused]] const Renderer* r) {
    RenderCaps c;
    c.backend          = KV_RB_NAME;
    c.max_textures     = kMaxTextures;
    c.max_materials    = kMaxMaterials;
    c.frames_in_flight = kFramesInFlight;
    c.srgb_output      = true;
    c.ui               = KV_RB_UI != 0;
    c.paced            = KV_RB_PACED != 0;
    c.texture_formats[(u32)TextureFormat::RGBA8_SRGB]  = true;
    c.texture_formats[(u32)TextureFormat::RGBA8_UNORM] = true;
    return c;
}

Handle renderer_default_material(const Renderer* r) { return r ? r->default_material : Handle{}; }
f64    renderer_last_wait(const Renderer* r) { return r ? r->last_wait : 0.0; }
RendererGpuStats renderer_gpu_stats(const Renderer* r) { return r ? r->gpu_prof.last : RendererGpuStats{}; }

// ---- the render world -----------------------------------------------------------------------

void renderer_apply(Renderer* r, Span<const SpriteDelta> deltas) {
    if (!r) return;
    KV_ZONE("apply");
    rw_apply(r->world, deltas);
}

u32 renderer_sprite_count(const Renderer* r) { return r ? r->world.sprites.live : 0; }

// ---- handle kinds and pools -----------------------------------------------------------------

namespace rc {

void* texture_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->textures.get(Handle{h}); }
int texture_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    Texture* t = r->textures.get(Handle{h});
    if (!t) return kv::format(out, cap, "stale");
    return kv::format(out, cap, "'%s' %ux%u slot %u %s", name_str(t->name), t->width, t->height, Handle{h}.index(),
                      t->resident ? "resident" : "placeholder");
}
void* material_resolve(kv_handle h, void* ctx) { return ((Renderer*)ctx)->world.materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Renderer* r = (Renderer*)ctx;
    MaterialRecord* m = r->world.materials.get(Handle{h});
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
    MaterialRecord* m = ((Renderer*)ctx)->world.materials.get(Handle{h});
    return m ? name_str(m->name) : nullptr;
}
kv_handle material_find_name(const char* name, void* ctx) {
    Renderer* r = (Renderer*)ctx;
    Handle* h = r->world.material_by_name.find(name_lookup(name).hash);
    return h && r->world.materials.valid(*h) ? h->bits : 0;
}

} // namespace rc

bool rc_create_pools(Renderer* r) {
    kv_handle_kind_desc td{};
    td.name = "render.texture"; td.type_key = type_key<Texture>(); td.ctx = r;
    td.resolve = rc::texture_resolve; td.describe = rc::texture_describe;
    td.name_of = rc::texture_name_of; td.find_name = rc::texture_find_name; td.flags = KV_HANDLE_GENERATIONAL;
    r->texture_kind = handle_kind_register(td);
    kv_handle_kind_desc md{};
    md.name = "render.material"; md.type_key = type_key<MaterialRecord>(); md.ctx = r;
    md.resolve = rc::material_resolve; md.describe = rc::material_describe;
    md.name_of = rc::material_name_of; md.find_name = rc::material_find_name; md.flags = KV_HANDLE_GENERATIONAL;
    r->material_kind = handle_kind_register(md);
    if (!r->texture_kind || !r->material_kind) return false;
    r->textures.use_kind(r->texture_kind);
    r->world.materials.use_kind(r->material_kind);
    r->texture_by_name.init(r->general);
    return r->textures.init(kMaxTextures, r->general->tag) && rw_init(r->world, r->general);
}

void rc_free_pools(Renderer* r) {
    if (r->texture_kind)  kv_handle_kind_unregister(r->texture_kind);
    if (r->material_kind) kv_handle_kind_unregister(r->material_kind);
    r->texture_kind = r->material_kind = 0;
    r->textures.free();
    r->texture_by_name.free();
    rw_free(r->world);
}

void rc_mark_slot_dirty(Renderer* r, u32 slot) {
    for (FrameInFlight& f : r->frames) f.dirty_slots.push(slot);
}

// The hardcoded defaults. Slot 0: a red and white checkerboard; what a stale or null texture
// reference shows, and what a pending texture shows until its upload. Its upload rides the
// first frame like any other; until then every slot of every frame's set points at it.
// Row 0: the material a sprite with none, or with one that has gone, draws with.
bool rc_create_defaults(Renderer* r) {
    Texture t{};
    t.width = t.height = kDefaultTexSize;
    t.format = TextureFormat::RGBA8_SRGB;
    t.name = name_intern("default");
    Handle h = r->textures.create(t);
    if (h.index() != 0) { KV_ERROR(KV_RB_NAME, "default texture is not slot 0"); return false; }
    Texture& slot0 = r->textures.items[0];
    if (!rb_texture_create(r, slot0)) return false;

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
        for (u32 slot = 0; slot < kMaxTextures; ++slot) rb_frame_slot(r, f, slot);

    MaterialRecord m{};
    m.texture = h;
    m.blend   = BlendMode::Alpha;
    m.slot    = (u32)BlendMode::Alpha;
    m.tint    = color_white();
    m.name    = name_intern("default");
    r->default_material = r->world.materials.create(m);
    r->texture_by_name.set(t.name.hash, h);
    r->world.material_by_name.set(m.name.hash, r->default_material);
    if (r->default_material.index() != 0) { KV_ERROR(KV_RB_NAME, "default material is not row 0"); return false; }
    rw_mark_material_dirty(r->world, 0);
    return true;
}

// ---- textures and materials: the API --------------------------------------------------------

namespace rc {

// A name whose holder died goes to the next live record with the same name, if any.
template <class T>
void rename_after_destroy(Pool<T>& pool, Map<u64, Handle>& by_name, Name name, Handle gone) {
    Handle* at = by_name.find(name.hash);
    if (!at || *at != gone) return;
    for (u32 i = 0; i < pool.len; ++i)
        if (pool.is_alive(i) && pool.items[i].name == name) { *at = pool.handle_at(i); return; }
    by_name.remove(name.hash);
}


} // namespace rc


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
    if (h.is_null()) { KV_ERROR(KV_RB_NAME, "out of texture slots (%u)", kMaxTextures); return h; }
    r->texture_by_name.get_or_insert(t.name.hash, h); // the first live one keeps the name
    rc_mark_slot_dirty(r, h.index()); // shows the default until the upload lands
    return h;
}

bool renderer_texture_upload(Renderer* r, Handle texture, u32 width, u32 height, const void* pixels, usize bytes) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0 || !pixels) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || bytes != (usize)width * height * 4) {
        KV_ERROR(KV_RB_NAME, "texture '%s': %ux%u with %llu bytes is not a tightly packed RGBA8 image",
                 name_str(t->name), width, height, (unsigned long long)bytes);
        return false;
    }
    t->width  = width;
    t->height = height;
    if (!rb_texture_create(r, *t)) return false;
    if (!rb_stage_texture(r, texture.index(), *t, (const u8*)pixels)) return false;
    rc_mark_slot_dirty(r, texture.index());
    return true;
}

bool renderer_texture_update(Renderer* r, Handle texture, const void* pixels, u32 width, u32 height, u32 x, u32 y, u32 w, u32 h) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0 || !pixels) return false;
    usize bytes = (usize)width * height * 4;
    // Not uploaded yet, or at another size, or a rectangle outside it: the whole image.
    if (t->width != width || t->height != height || x + w > width || y + h > height || !w || !h)
        return renderer_texture_upload(r, texture, width, height, pixels, bytes);
    if (rb_stage_texture_region(r, texture.index(), *t, (const u8*)pixels, x, y, w, h)) return true;
    return renderer_texture_upload(r, texture, width, height, pixels, bytes);
}

void renderer_texture_destroy(Renderer* r, Handle texture) {
    Texture* t = r ? r->textures.get(texture) : nullptr;
    if (!t || texture.index() == 0) return; // the default stays
    rb_defer_destroy_texture(r, *t);
    u32 slot = texture.index();
    Name name = t->name;
    r->textures.destroy(texture);
    rc::rename_after_destroy(r->textures, r->texture_by_name, name, texture);
    rc_mark_slot_dirty(r, slot); // back to the default, in each set once its frame has passed
}

u32 renderer_texture_slot(const Renderer* r, Handle texture) {
    return r && r->textures.valid(texture) ? texture.index() : 0;
}

namespace rc {


// What an unnamed material is: its texture's name and everything it draws with, hashed.
// The same content made in any order gets the same name.
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
    m.slot    = rc_material_slot(r, m.shader, m.blend);
    m.filter  = (u32)desc.filter < (u32)TextureFilter::Count ? desc.filter : TextureFilter::Nearest;
    m.tint    = desc.tint;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) m.params[i] = desc.params[i];
}


} // namespace rc

Handle renderer_material_create(Renderer* r, const MaterialDesc& desc) {
    if (!r) return Handle{};
    MaterialRecord m{};
    rc::fill_material(r, m, desc);
    if (desc.name && desc.name[0]) m.name = name_intern(desc.name);
    else {
        m.name = rc::derived_material_name(r, m);
#if KV_DEV
        bool inserted = false;
        r->world.material_by_name.get_or_insert(m.name.hash, Handle{}, &inserted);
        if (inserted) { r->world.material_by_name.remove(m.name.hash); KV_INFO(KV_RB_NAME, "material named %s by its content", name_str(m.name)); }
#endif
    }
    Handle h = r->world.materials.create(m);
    if (h.is_null()) { KV_ERROR(KV_RB_NAME, "out of materials (%u)", kMaxMaterials); return h; }
    r->world.material_by_name.get_or_insert(m.name.hash, h);
    rw_mark_material_dirty(r->world, h.index());
    return h;
}

bool renderer_material_set(Renderer* r, Handle material, const MaterialDesc& desc) {
    MaterialRecord* m = r ? r->world.materials.get(material) : nullptr;
    if (!m) return false;
    u32 before = m->slot;
    rc::fill_material(r, *m, desc);
    if (m->slot != before) rw_rebind_material(r->world, material.index()); // the sprites' buckets moved
    rw_mark_material_dirty(r->world, material.index());
    return true;
}

bool renderer_material_get(const Renderer* r, Handle material, MaterialDesc* out) {
    const MaterialRecord* m = r ? r->world.materials.get(material) : nullptr;
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
    if (!r || material == r->default_material || !r->world.materials.valid(material)) return;
    u32 row = material.index();
    Name name = r->world.materials.get(material)->name;
    r->world.materials.destroy(material);
    rc::rename_after_destroy(r->world.materials, r->world.material_by_name, name, material);
    rw_rebind_material(r->world, row);      // its sprites fall back to the default
    rw_mark_material_dirty(r->world, row);  // the row is rewritten as a copy of the default
}


// ---- game shaders ---------------------------------------------------------------------------

void rc_material_row(Renderer* r, u32 row, GpuMaterial& out) {
    const MaterialRecord& m = rw_material_at(r->world, row);
    rw_material_row(m, renderer_texture_slot(r, m.texture), out);
}

u32 rc_material_slot(Renderer* r, Name shader, BlendMode blend) {
    u32 engine = (u32)blend < kBlendCount ? (u32)blend : (u32)BlendMode::Alpha;
    if (shader.is_null()) return engine;
    u32 free_slot = kGameSlots;
    for (u32 i = 0; i < kGameSlots; ++i) {
        GamePipe& gp = r->game_pipes[i];
        if (gp.used && gp.shader == shader && gp.blend == blend) return gp.failed ? engine : kBlendCount + i;
        if (!gp.used && free_slot == kGameSlots) free_slot = i;
    }
    if (free_slot == kGameSlots) {
        KV_ERROR(KV_RB_NAME, "game shader '%s': all %u game pipeline slots are taken; drawn with the engine's", name_str(shader), kGameSlots);
        return engine;
    }
    GamePipe& gp = r->game_pipes[free_slot];
    gp = GamePipe{};
    gp.used   = true;
    gp.shader = shader;
    gp.blend  = blend;
    if (!rb_game_pipe_load(r, gp)) {
        gp.failed = true;
        KV_ERROR(KV_RB_NAME, "game shader '%s' did not load: its sprites draw with the engine's", name_str(shader));
        return engine;
    }
    return gp.failed ? engine : kBlendCount + free_slot;
}

// ---- the full-frame pass: what the game set ---------------------------------------------------

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

// ---- the UI primitive's lists ------------------------------------------------------------------

void renderer_ui_set(Renderer* r, i32 layer, const UiList& list) { renderer_ui_set(r, 0u, layer, list); }
void renderer_ui_clear(Renderer* r, i32 layer) { renderer_ui_clear(r, 0u, layer); }

void renderer_ui_set(Renderer* r, u32 surface, i32 layer, const UiList& list) {
    if (r && surface < kSurfaces && (surface == 0 || r->surfaces[surface].live)) rw_ui_set(r->world, surface, layer, list);
}

void renderer_ui_clear(Renderer* r, u32 surface, i32 layer) {
    if (r) rw_ui_clear(r->world, surface, layer);
}

// ---- other windows ----------------------------------------------------------------------------

bool renderer_surface_add(Renderer* r, u32 surface, void* native_window, u32 width, u32 height, bool transparent) {
    if (!r) return false;
    if (surface == 0 || surface >= kSurfaces || r->surfaces[surface].live || !native_window) {
        KV_ERROR(KV_RB_NAME, "surface %u: %s", surface, surface == 0 || surface >= kSurfaces ? "not an id another window has (1 to 7)" : !native_window ? "no window" : "already added");
        return false;
    }
    Surface& s = r->surfaces[surface];
    s.native = native_window;
    s.transparent = transparent;
    s.want_width = width;
    s.want_height = height;
    s.resized = true;
    s.drawn_version = s.drawing_version = 0;
    if (!rb_surface_create(r, s)) {
        rb_surface_destroy(r, s);
        return false;
    }
    s.live = true;
    r->presents[surface] = 0;
    KV_INFO(KV_RB_NAME, "surface %u added: %ux%u%s", surface, width, height, transparent ? ", transparent" : "");
    return true;
}

void renderer_surface_remove(Renderer* r, u32 surface) {
    if (!r || surface == 0 || surface >= kSurfaces || !r->surfaces[surface].live) return;
    rb_surface_destroy(r, r->surfaces[surface]);
    r->surfaces[surface].live = false;
    rw_ui_clear_surface(r->world, surface);
    KV_INFO(KV_RB_NAME, "surface %u removed after %llu presents", surface, (unsigned long long)r->presents[surface]);
}

void renderer_surface_resize(Renderer* r, u32 surface, u32 width, u32 height) {
    if (!r || surface == 0 || surface >= kSurfaces || !r->surfaces[surface].live) return;
    Surface& s = r->surfaces[surface];
    if (s.want_width == width && s.want_height == height) return;
    s.want_width = width;
    s.want_height = height;
    s.resized = true;
}

void renderer_surface_visible(Renderer* r, u32 surface, bool visible) {
    if (!r || surface == 0 || surface >= kSurfaces || !r->surfaces[surface].live) return;
    Surface& s = r->surfaces[surface];
    if (s.hidden == !visible) return;
    s.hidden = !visible;
    if (visible) s.resized = true; // what it shows is drawn again
}

bool renderer_surface_pending(const Renderer* r) {
    if (!r) return false;
    for (u32 i = 1; i < kSurfaces; ++i) {
        const Surface& s = r->surfaces[i];
        if (s.live && !s.hidden && (s.resized || s.drawn_version != r->world.ui_surface_version[i])) return true;
    }
    return false;
}

u64 renderer_surface_presents(const Renderer* r, u32 surface) { return r && surface < kSurfaces ? r->presents[surface] : 0; }

RendererUiStats renderer_ui_stats(const Renderer* r) {
    RendererUiStats s;
    if (!r) return s;
    s.sets = r->world.ui_sets;
    s.stages = r->world.ui_stages;
    s.layers = r->world.ui.len;
    s.bytes = r->world.ui_bytes;
    return s;
}

// ---- deferred destruction -------------------------------------------------------------------

void rc_defer(Renderer* r, RbGarbage g) { r->frames[r->frame_index].garbage.push(g); }

void rc_run_garbage(Renderer* r, FrameInFlight& f) {
    u32 owned = f.garbage_owned <= f.garbage.len ? f.garbage_owned : f.garbage.len;
    for (u32 i = 0; i < owned; ++i) rb_garbage_destroy(r, f.garbage[i]);
    // What was queued after the frame's submit stays, first, for the frame after.
    u32 rest = f.garbage.len - owned;
    if (owned && rest) memmove(f.garbage.data, f.garbage.data + owned, sizeof(RbGarbage) * rest);
    f.garbage.len   = rest;
    f.garbage_owned = 0;
}

// ---- the frame ring -------------------------------------------------------------------------
// [tail, head) is owned by frames in flight, the frame being recorded included; `used`
// counts those bytes with wrap gaps so head == tail is unambiguous. A frame's allocations
// are contiguous in ring order even when they begin before the previous frame is done, so
// releasing a frame is moving the tail to where that frame ended.

bool rc_ring_init(Renderer* r, u64 bytes) {
    FrameRing& g = r->ring;
    if (!rb_ring_buffer(r, bytes, g.buf)) return false;
    g.size = bytes;
    g.head = g.tail = g.used = g.frame_bytes = 0;
    return true;
}

namespace rc {

bool ring_grow(Renderer* r, u64 need) {
    FrameRing& g = r->ring;
    u64 cap = g.size * 2;
    while (cap < need * 2) cap *= 2;
    // Spans handed out so far point at the old buffer, which stays alive until this
    // frame is done; the accounting restarts in the new one.
    rb_buffer_retire(r, g.buf);
    if (!rb_ring_buffer(r, cap, g.buf)) return false;
    g.size = cap;
    g.head = g.tail = g.used = 0;
    // The old buffer's spans are the garbage queue's now, this frame's included: counted
    // against the new buffer they would be released from it, and `used` would wrap.
    g.frame_bytes = 0;
    for (FrameInFlight& f : r->frames) f.ring_bytes = 0;
    ++g.grows;
    KV_INFO(KV_RB_NAME, "frame ring grown to %llu KB", (unsigned long long)(cap / 1024));
    return true;
}

} // namespace rc

RingSpan rc_ring_alloc(Renderer* r, u64 bytes, u64 align) {
    FrameRing& g = r->ring;
    RingSpan out;
    if (bytes == 0) return out;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        u64 cap  = g.size;
        u64 head = (g.head + align - 1) / align * align;
        u64 gap  = head - g.head;
        bool ok = false;
        if (g.used == 0) {
            g.head = g.tail = 0; head = 0; gap = 0;
            ok = bytes <= cap;
        } else if (g.head >= g.tail) {         // free: [head, cap) then [0, tail)
            if (head + bytes <= cap) ok = true;
            else if (bytes < g.tail) { gap = cap - g.head; head = 0; ok = true; } // wrap; the rest of the buffer is a gap
        } else {                               // free: [head, tail)
            ok = head + bytes < g.tail;
        }
        if (ok) {
            out.buffer = g.buf;
            out.offset = head;
            out.ptr    = (u8*)rb_buffer_ptr(g.buf) + head;
            g.head     = head + bytes;
            if (g.head == cap) g.head = 0;
            g.used        += gap + bytes;
            g.frame_bytes += gap + bytes;
            if (g.used > g.peak) g.peak = g.used;
            return out;
        }
        if (!rc::ring_grow(r, bytes + align)) return RingSpan{};
    }
    return out;
}

void rc_ring_submitted(Renderer* r, FrameInFlight& f) {
    f.ring_end   = r->ring.head;
    f.ring_bytes = r->ring.frame_bytes;
    r->ring.frame_bytes = 0;
}

void rc_ring_release(Renderer* r, FrameInFlight& f) {
    FrameRing& g = r->ring;
    // A frame that took nothing (or whose spans went with a retired buffer) ends where
    // the frame before it did; its end mark says nothing about the current buffer.
    if (!f.submitted || f.ring_bytes == 0) return;
    g.used -= f.ring_bytes;
    g.tail  = f.ring_end;
    f.ring_bytes = 0;
    if (g.used == 0) g.head = g.tail = 0;
}

// ---- --check-sort and --screenshot ----------------------------------------------------------

bool renderer_check_sort(Renderer* r) {
    if (!r) return false;
    r->check_pending = true;
    return true;
}

bool renderer_screenshot(Renderer* r, const char* path) {
    if (!r || !path || strlen(path) >= sizeof r->shot_path) return false;
    kv::format(r->shot_path, sizeof r->shot_path, "%s", path);
    r->shot_pending = true;
    return true;
}

// Each row's key as this frame computes it, kept for the check: the rows may change before
// the frame's results are back.
void rc_snapshot_keys(Renderer* r, f32 alpha) {
    u32 rows = r->world.sprites.len;
    free_in(mem_general(), r->check_keys, sizeof(f32) * (r->check_rows ? r->check_rows : 1));
    r->check_keys = (f32*)alloc_in(mem_general(), sizeof(f32) * (rows ? rows : 1));
    rw_snapshot_keys(r->world, alpha, r->check_keys);
}

bool rc_verify_sort(Renderer* r) {
    r->check_recorded = false;
    const u32* readback = (const u32*)rb_buffer_ptr(r->check_buffer);
    if (!readback) return false;
    bool ok = rw_verify_sort(r->world, readback, r->check_rows, r->check_keys, KV_RB_NAME);
    if (ok) ++r->checks_passed; else ++r->checks_failed;
    rb_buffer_retire(r, r->check_buffer);
    return ok;
}

// Binary PPM, RGB, from the four bytes a pixel the backend copied: sRGB-encoded either way.
bool rc_write_screenshot(Renderer* r) {
    r->shot_recorded = false;
    const u8* px = (const u8*)rb_buffer_ptr(r->shot_buffer);
    if (!px) return false;
    IoFileWriter f;
    if (!f.open(r->shot_path)) { KV_ERROR(KV_RB_NAME, "cannot write screenshot '%s'", r->shot_path); rb_buffer_retire(r, r->shot_buffer); return false; }
    f.print("P6\n%u %u\n255\n", r->shot_width, r->shot_height);
    usize pitch = r->shot_pitch ? r->shot_pitch : (usize)r->shot_width * 4;
    bool bgra = !r->shot_rgba;
    MemScope scope;
    u8* row = arena_push_n<u8>(*scope.arena, r->shot_width * 3);
    for (u32 y = 0; y < r->shot_height; ++y) {
        const u8* src = px + (usize)y * pitch;
        for (u32 x = 0; x < r->shot_width; ++x, src += 4) {
            row[x * 3 + 0] = bgra ? src[2] : src[0];
            row[x * 3 + 1] = src[1];
            row[x * 3 + 2] = bgra ? src[0] : src[2];
        }
        f.write(row, r->shot_width * 3);
    }
    if (!f.close()) KV_ERROR(KV_RB_NAME, "screenshot '%s': write failed (errno %d)", r->shot_path, f.error);
    else KV_INFO(KV_RB_NAME, "screenshot '%s' %ux%u", r->shot_path, r->shot_width, r->shot_height);
    rb_buffer_retire(r, r->shot_buffer);
    return true;
}

} } // namespace kv::KV_RB_NS

#include "korvid/render/core/frame.inl"
