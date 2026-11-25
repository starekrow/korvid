// Korvid asset_render: materials (korvid/asset_render/material.h). The file format, the
// records as renderer rows, and the kind registered with the asset module.
#include "korvid/asset_render/material.h"
#include "korvid/asset/reload.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/core/name.h"
#include "korvid/core/utf8.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_render_internal.h"
#include <string.h>

namespace kv {

// ---- the file format ------------------------------------------------------------------------

bool material_parse(const char* text, usize len, MaterialDesc& desc, char* texture_path, usize cap,
                    const char** error, u32* error_at) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, len, mem_scratch(), doc)) { *error = doc.error; *error_at = doc.error_at; return false; }
    const JsonValue& root = doc.root();
    if (!root.is_object()) { *error = "a material file is one object"; *error_at = 0; return false; }
    for (const JsonValue* m = json_first(doc, root); m; m = json_next(doc, *m)) {
        if (strcmp(m->key, "texture") == 0) {
            if (!m->is_string()) { *error = "texture must be a path string"; *error_at = 0; return false; }
            if (m->len() >= cap) { *error = "texture path too long"; *error_at = 0; return false; }
            memcpy(texture_path, m->str(), m->len() + 1);
        } else if (strcmp(m->key, "blend") == 0) {
            if (!m->is_string() || !blend_mode_parse(m->str(), &desc.blend)) { *error = "blend must be opaque, alpha or additive"; *error_at = 0; return false; }
        } else if (strcmp(m->key, "tint") == 0) {
            f32 t[4] = {desc.tint.r, desc.tint.g, desc.tint.b, desc.tint.a};
            u32 n = json_numbers(doc, m, t, 4);
            if (!m->is_array() || n < 3 || n > 4) { *error = "tint must be [r, g, b] or [r, g, b, a]"; *error_at = 0; return false; }
            desc.tint = color_rgba(t[0], t[1], t[2], n == 4 ? t[3] : 1.0f);
        } else if (strcmp(m->key, "filter") == 0) {
            const char* f = json_string(m, "");
            if (!strcmp(f, "nearest")) desc.filter = TextureFilter::Nearest;
            else if (!strcmp(f, "linear")) desc.filter = TextureFilter::Linear;
            else { *error = "filter must be nearest or linear"; *error_at = 0; return false; }
        } else if (strcmp(m->key, "shader") == 0) {
            if (!m->is_string() || !m->len()) { *error = "shader must be a game shader's name"; *error_at = 0; return false; }
            desc.shader = name_str(name_intern(m->str())); // interned: it outlives the document
        } else if (strcmp(m->key, "params") == 0) {
            u32 n = json_numbers(doc, m, desc.params, KV_MATERIAL_PARAMS);
            if (!m->is_array() || n > KV_MATERIAL_PARAMS) { *error = "params must be an array of up to 8 numbers"; *error_at = 0; return false; }
        } else {
            *error = "unknown field"; *error_at = 0; return false;
        }
    }
    return true;
}

int material_format(const MaterialDesc& desc, const char* texture_path, char* out, usize cap) {
    return format(out, cap, "{ \"texture\": \"%s\", \"blend\": \"%s\", \"filter\": \"%s\", \"tint\": [%g, %g, %g, %g], \"params\": [%g, %g, %g, %g, %g, %g, %g, %g] }",
                    texture_path ? texture_path : "", blend_mode_name(desc.blend), desc.filter == TextureFilter::Linear ? "linear" : "nearest",
                    desc.tint.r, desc.tint.g, desc.tint.b, desc.tint.a,
                    desc.params[0], desc.params[1], desc.params[2], desc.params[3],
                    desc.params[4], desc.params[5], desc.params[6], desc.params[7]);
}

// ---- the records -----------------------------------------------------------------------------

namespace {

void* material_resolve(kv_handle h, void* ctx) { return ((AssetRenderState*)ctx)->materials.get(Handle{h}); }
const char* material_name_of(kv_handle h, void* ctx) { auto* a = ((AssetRenderState*)ctx)->materials.get(Handle{h}); return a ? a->path : nullptr; }
kv_handle material_find_name(const char* name, void* ctx) {
    const Handle* h = ((AssetRenderState*)ctx)->material_by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    MaterialAsset* m = ((AssetRenderState*)ctx)->materials.get(Handle{h});
    if (!m) return format(out, cap, "stale");
    const char* st = m->state == AssetState::Ready ? "ready" : m->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s %s texture '%s'", m->path, st, blend_mode_name(m->desc.blend), m->texture_path);
}

Handle material_load(Engine& e, AssetRenderState& st, const char* path); // below: no scope, refs untouched

void fail_material(Engine& e, AssetRenderState& s, Handle h, MaterialAsset& m, AssetError err, const char* detail) {
    m.state = AssetState::Failed;
    m.error = err;
    m.io    = Handle{};
    ++s.materials_failed;
    KV_WARN("asset_render", "material '%s': %s%s%s", m.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Points the record's texture at the asset for `path` (loading it, and holding a
// reference on it in place of the one on the texture before) and the row at that asset's
// renderer texture. Empty path: the default texture.
void bind_texture(Engine& e, AssetRenderState& s, MaterialAsset& m, const char* path) {
    Handle old = m.texture_asset;
    utf8_copy(m.texture_path, sizeof m.texture_path, path ? path : "");
    m.texture_asset = Handle{};
    m.desc.texture  = Handle{};
    if (m.texture_path[0]) {
        m.texture_asset = texture_load(e, s, m.texture_path);
        asset_ref(e, m.texture_asset);
        if (const TextureAsset* t = asset_texture(e, m.texture_asset)) m.desc.texture = t->texture;
    }
    asset_unref(e, old); // after the new one is held: the same path keeps its texture
    renderer_material_set(renderer_get(e), m.material, m.desc);
}

u32* material_kind_refs(void* user, Handle h) { MaterialAsset* m = ((AssetRenderState*)user)->materials.get(h); return m ? &m->refs : nullptr; }
AssetState material_kind_state(void* user, Handle h) { MaterialAsset* m = ((AssetRenderState*)user)->materials.get(h); return m ? m->state : AssetState::Failed; }
const char* material_kind_path(void* user, Handle h) { MaterialAsset* m = ((AssetRenderState*)user)->materials.get(h); return m ? m->path : nullptr; }
Handle material_kind_load(Engine& e, void*, const char* path, Handle scope) { return asset_load_material(e, path, scope); }

void material_kind_retire(Engine& e, void* user, Handle h) {
    AssetRenderState& s = *(AssetRenderState*)user;
    MaterialAsset* m = s.materials.get(h);
    if (!m) return;
    IoQueue* io = platform_io(e);
    if (io && !m->io.is_null()) io->cancel(m->io);
    if (Renderer* r = renderer_get(e)) renderer_material_destroy(r, m->material);
    asset_unref(e, m->texture_asset);
    s.material_by_path.remove(name_intern(m->path));
    KV_INFO("asset_render", "material '%s' retired", m->path);
    s.materials.destroy(h);
}

// A material made for a texture (asset_material_for_texture) has no file of its own: only
// those read from one are read again.
u32 material_kind_reload(Engine& e, void* user, const char* path) {
    AssetRenderState& s = *(AssetRenderState*)user;
    if (!path) {
        u32 n = 0;
        for (u32 i = 0; i < s.materials.len; ++i)
            if (s.materials.is_alive(i) && asset_reread(e, s.general, s.materials.handle_at(i), asset_reread_of(s.materials.items[i]))) ++n;
        return n;
    }
    Name name = name_lookup(path);
    const Handle* h = name.hash ? s.material_by_path.find(name) : nullptr;
    MaterialAsset* m = h ? s.materials.get(*h) : nullptr;
    return m && asset_reread(e, s.general, *h, asset_reread_of(*m)) ? 1 : 0;
}

} // namespace

bool asset_materials_init(Engine& e, AssetRenderState& s, ModuleInstance& self) {
    s.material_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset_render.material"; desc.type_key = type_key<MaterialAsset>(); desc.ctx = &s;
    desc.resolve = material_resolve; desc.name_of = material_name_of; desc.find_name = material_find_name; desc.describe = material_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.material_kind = handle_kind_register(desc);
    if (!s.material_kind) return false;
    s.materials.use_kind(s.material_kind);
    if (!s.materials.init(4096, self.general.tag)) return false;
    AssetKindDesc k;
    k.kind = s.material_kind; k.refs = material_kind_refs; k.state = material_kind_state; k.path = material_kind_path; k.retire = material_kind_retire;
    k.reload = material_kind_reload; k.suffix = ".material.json"; k.load = material_kind_load; k.user = &s;
    if (!asset_kind_register(e, k)) { KV_ERROR("asset_render", "could not register materials with the asset module: is it up?"); return false; }
    return true;
}

void asset_materials_shutdown(Engine& e, AssetRenderState& s) {
    asset_kind_unregister(e, s.material_kind);
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.materials.len; ++i)
        if (s.materials.is_alive(i) && io && !s.materials.items[i].io.is_null()) io->cancel(s.materials.items[i].io);
    KV_INFO("asset_render", "%u materials loaded, %u failed", s.materials_loaded, s.materials_failed);
    kv_handle_kind_unregister(s.material_kind);
    s.material_by_path.free();
    s.materials.free();
}

// Phase PreSim, after the texture pump: files that arrived become rows and texture
// requests; materials whose texture became resident become Ready.
void asset_materials_pump(TickContext& ctx) {
    AssetRenderState& s = ctx.state_as<AssetRenderState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_render_reap_orphans(e, s, s.material_kind);
    for (u32 i = 0; i < io->completions(asset_io_owner(s.material_kind)).len;) {
        IoCompletion c = io->completions(asset_io_owner(s.material_kind))[i];
        if (Handle{c.user}.kind() != s.material_kind) { ++i; continue; }
        Handle h{c.user};
        MaterialAsset* m = s.materials.get(h);
        if (!m || m->io != c.request) { ++i; continue; }
        io->reap(c.request);
        m->io = Handle{};
        bool re = m->reread;
        m->reread = false;
        if (c.status != IoStatus::Done) {
            AssetError err = c.not_found() ? AssetError::NotFound : AssetError::Read;
            if (re) asset_reload_failed(e, h, m->path, err, nullptr);
            else fail_material(e, s, h, *m, err, nullptr);
            asset_reread_after(e, s.general, h, asset_reread_of(*m));
            continue;
        }
        u64 hash = hash_bytes(c.bytes.data, c.bytes.size);
        if (re && hash == m->hash) {
            c.bytes.free();
            asset_reload_unchanged(m->path);
            asset_reread_after(e, s.general, h, asset_reread_of(*m));
            continue;
        }
        MaterialDesc desc;
        char texture_path[KV_ASSET_PATH_MAX] = {};
        const char* why = nullptr;
        u32 at = 0;
        bool ok = material_parse((const char*)c.bytes.data, c.bytes.size, desc, texture_path, sizeof texture_path, &why, &at);
        c.bytes.free();
        if (!ok) {
            char detail[160];
            format(detail, sizeof detail, "%s (byte %u)", why, at);
            if (re) asset_reload_failed(e, h, m->path, AssetError::Parse, detail);
            else fail_material(e, s, h, *m, AssetError::Parse, detail);
            asset_reread_after(e, s.general, h, asset_reread_of(*m));
            continue;
        }
        m->desc = desc;
        m->hash = hash;
        // Sets the row. A first load is Ready when its texture is (below); a reload's row is
        // the new look at once, its texture's placeholder until a new path's file lands.
        bind_texture(e, s, *m, texture_path);
        if (re) asset_reloaded(e, h, m->path, m->generation);
        asset_reread_after(e, s.general, h, asset_reread_of(*m));
    }
    // Parsed and waiting: Ready when the texture is, or at once without one.
    for (u32 i = 0; i < s.materials.len; ++i) {
        if (!s.materials.is_alive(i)) continue;
        MaterialAsset& m = s.materials.items[i];
        if (m.state != AssetState::Pending || !m.io.is_null()) continue;
        const TextureAsset* t = m.texture_asset.is_null() ? nullptr : asset_texture(e, m.texture_asset);
        if (t && t->state == AssetState::Pending) continue;
        Handle h = s.materials.handle_at(i);
        if (t && t->state == AssetState::Failed) { fail_material(e, s, h, m, t->error, m.texture_path); continue; }
        m.state = AssetState::Ready;
        ++s.materials_loaded;
        KV_INFO("asset_render", "material '%s' ready: %s, texture '%s'", m.path, blend_mode_name(m.desc.blend), m.texture_path);
        e.events.emit(AssetReady{h});
    }
}

Handle asset_load_material(Engine& e, const char* path, Handle scope) {
    AssetRenderState* s = asset_render_state(e);
    if (!s) { KV_ERROR("asset_render", "cannot load material '%s': asset_render is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(e, scope, material_load(e, *s, path));
}

Handle asset_load_material(Engine& e, const char* path) { return asset_load_material(e, path, asset_scope_global(e)); }

namespace {

Handle material_load(Engine& e, AssetRenderState& st, const char* path) {
    AssetRenderState* s = &st;
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    if (!r || !io || !path) { KV_ERROR("asset_render", "cannot load material '%s': asset_render, the renderer and the platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset_render", "path too long: '%s'", path); return Handle{}; }

    Name name = name_intern(path);
    if (const Handle* existing = s->material_by_path.find(name)) return *existing;

    MaterialAsset m{};
    strncpy(m.path, path, sizeof m.path - 1);
    m.state    = AssetState::Pending;
    MaterialDesc first = m.desc;
    first.name = path; // the renderer's row is called what the file is
    m.material = renderer_material_create(r, first); // the placeholder look, until the file says otherwise
    if (m.material.is_null()) return Handle{};
    Handle h = s->materials.create(m);
    if (h.is_null()) { renderer_material_destroy(r, m.material); return Handle{}; }
    s->material_by_path.set(name, h);

    IoRequest req;
    req.path  = path;
    req.alloc = s->general;
    req.user  = h.bits;
    req.owner = asset_io_owner(h.kind());
    Handle iq = io->read_asset(req);
    MaterialAsset* rec = s->materials.get(h);
    if (iq.is_null()) { fail_material(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

} // namespace

Handle asset_material_for_texture(Engine& e, Handle texture_asset, BlendMode blend, TextureFilter filter, Handle scope) {
    AssetRenderState* s = asset_render_state(e);
    Renderer* r = renderer_get(e);
    const TextureAsset* t = s ? asset_texture(e, texture_asset) : nullptr;
    if (!t || !r) { KV_ERROR("asset_render", "asset_material_for_texture: %s", !t ? "not a texture asset" : "no renderer"); return Handle{}; }
    char path[KV_ASSET_PATH_MAX];
    int n = format(path, sizeof path, "%s:%s%s", t->path, blend_mode_name(blend), filter == TextureFilter::Linear ? ":linear" : "");
    if (n < 0 || (usize)n >= sizeof path) { KV_ERROR("asset_render", "material name too long for '%s'", t->path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s->material_by_path.find(name)) return asset_hold(e, scope, *existing);
    MaterialAsset m{};
    utf8_copy(m.path, sizeof m.path, path);
    m.state = AssetState::Pending; // Ready with its texture, in the pump
    m.desc.blend  = blend;
    m.desc.filter = filter;
    MaterialDesc first = m.desc;
    first.name = path;
    m.material = renderer_material_create(r, first);
    if (m.material.is_null()) return Handle{};
    Handle h = s->materials.create(m);
    if (h.is_null()) { renderer_material_destroy(r, m.material); return Handle{}; }
    s->material_by_path.set(name, h);
    bind_texture(e, *s, *s->materials.get(h), t->path); // holds the texture, points the row at it
    return asset_hold(e, scope, h);
}

const MaterialAsset* asset_material(Engine& e, Handle asset) {
    AssetRenderState* s = asset_render_state(e);
    return s ? s->materials.get(asset) : nullptr;
}

Handle asset_material_find(Engine& e, const char* path) {
    AssetRenderState* s = asset_render_state(e);
    if (!s || !path) return Handle{};
    Name name = name_lookup(path);
    if (name.is_null()) return Handle{};
    const Handle* h = s->material_by_path.find(name);
    return h ? *h : Handle{};
}

u32 asset_material_count(Engine& e) {
    AssetRenderState* s = asset_render_state(e);
    return s ? s->materials.len : 0;
}

const MaterialAsset* asset_material_at(Engine& e, u32 slot) {
    AssetRenderState* s = asset_render_state(e);
    return s && s->materials.is_alive(slot) ? &s->materials.items[slot] : nullptr;
}

Handle asset_material_handle_at(Engine& e, u32 slot) {
    AssetRenderState* s = asset_render_state(e);
    return s && s->materials.is_alive(slot) ? s->materials.handle_at(slot) : Handle{};
}

bool asset_material_patch(Engine& e, Handle asset, const MaterialDesc& desc) {
    AssetRenderState* s = asset_render_state(e);
    MaterialAsset* m = s ? s->materials.get(asset) : nullptr;
    if (!m) return false;
    Handle texture = m->desc.texture; // the texture follows its path, not the caller's handle
    m->desc = desc;
    m->desc.texture = texture;
    return renderer_material_set(renderer_get(e), m->material, m->desc);
}

bool asset_material_set_texture(Engine& e, Handle asset, const char* texture_path) {
    AssetRenderState* s = asset_render_state(e);
    MaterialAsset* m = s ? s->materials.get(asset) : nullptr;
    if (!m || (texture_path && strlen(texture_path) >= KV_ASSET_PATH_MAX)) return false;
    bind_texture(e, *s, *m, texture_path);
    return true;
}

} // namespace kv
