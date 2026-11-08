#include "korvid/asset/material.h"
#include "korvid/core/arena.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_internal.h"
#include <stdio.h>
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
            if (m->len >= cap) { *error = "texture path too long"; *error_at = 0; return false; }
            memcpy(texture_path, m->str, m->len + 1);
        } else if (strcmp(m->key, "blend") == 0) {
            if (!m->is_string() || !blend_mode_parse(m->str, &desc.blend)) { *error = "blend must be opaque, alpha or additive"; *error_at = 0; return false; }
        } else if (strcmp(m->key, "tint") == 0) {
            f32 t[4] = {desc.tint.r, desc.tint.g, desc.tint.b, desc.tint.a};
            u32 n = json_numbers(doc, m, t, 4);
            if (!m->is_array() || n < 3 || n > 4) { *error = "tint must be [r, g, b] or [r, g, b, a]"; *error_at = 0; return false; }
            desc.tint = color_rgba(t[0], t[1], t[2], n == 4 ? t[3] : 1.0f);
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
    return snprintf(out, cap, "{ \"texture\": \"%s\", \"blend\": \"%s\", \"tint\": [%g, %g, %g, %g], \"params\": [%g, %g, %g, %g, %g, %g, %g, %g] }",
                    texture_path ? texture_path : "", blend_mode_name(desc.blend),
                    desc.tint.r, desc.tint.g, desc.tint.b, desc.tint.a,
                    desc.params[0], desc.params[1], desc.params[2], desc.params[3],
                    desc.params[4], desc.params[5], desc.params[6], desc.params[7]);
}

// ---- the records -----------------------------------------------------------------------------

namespace {

void* material_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->materials.get(Handle{h}); }
int material_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    MaterialAsset* m = ((AssetState_*)ctx)->materials.get(Handle{h});
    if (!m) return snprintf(out, cap, "stale");
    const char* st = m->state == AssetState::Ready ? "ready" : m->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %s texture '%s'", m->path, st, blend_mode_name(m->desc.blend), m->texture_path);
}

void fail_material(Engine& e, AssetState_& s, Handle h, MaterialAsset& m, AssetError err, const char* detail) {
    m.state = AssetState::Failed;
    m.error = err;
    m.io    = Handle{};
    ++s.materials_failed;
    KV_WARN("asset", "material '%s': %s%s%s", m.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Points the record's texture at the asset for `path` (loading it) and the row at that
// asset's renderer texture. Empty path: the default texture.
void bind_texture(Engine& e, MaterialAsset& m, const char* path) {
    strncpy(m.texture_path, path ? path : "", sizeof m.texture_path - 1);
    m.texture_asset = Handle{};
    m.desc.texture  = Handle{};
    if (m.texture_path[0]) {
        m.texture_asset = asset_load_texture(e, m.texture_path);
        if (const TextureAsset* t = asset_texture(e, m.texture_asset)) m.desc.texture = t->texture;
    }
    renderer_material_set(renderer_get(e), m.material, m.desc);
}

} // namespace

bool asset_materials_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.material_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.material"; desc.type_key = type_key<MaterialAsset>(); desc.ctx = &s;
    desc.resolve = material_resolve; desc.describe = material_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.material_kind = handle_kind_register(desc);
    if (!s.material_kind) return false;
    s.materials.use_kind(s.material_kind);
    return s.materials.init(4096, self.general.tag);
}

void asset_materials_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.materials.len; ++i)
        if (s.materials.is_alive(i) && io && !s.materials.items[i].io.is_null()) io->cancel(s.materials.items[i].io);
    KV_INFO("asset", "%u materials loaded, %u failed", s.materials_loaded, s.materials_failed);
    kv_handle_kind_unregister(s.material_kind);
    s.material_by_path.free();
    s.materials.free();
}

// Phase PreSim, after the texture pump: files that arrived become rows and texture
// requests; materials whose texture became resident become Ready.
void asset_materials_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.material_kind)) { ++i; continue; }
        Handle h{c.user};
        MaterialAsset* m = s.materials.get(h);
        if (!m || m->io != c.request) { ++i; continue; }
        io->reap(c.request);
        m->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_material(e, s, h, *m, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        MaterialDesc desc;
        char texture_path[KV_ASSET_PATH_MAX] = {};
        const char* why = nullptr;
        u32 at = 0;
        bool ok = material_parse((const char*)c.data, c.size, desc, texture_path, sizeof texture_path, &why, &at);
        free_in(s.general, c.data, c.size);
        if (!ok) {
            char detail[160];
            snprintf(detail, sizeof detail, "%s (byte %u)", why, at);
            fail_material(e, s, h, *m, AssetError::Parse, detail);
            continue;
        }
        m->desc = desc;
        bind_texture(e, *m, texture_path); // sets the row; Ready follows the texture below
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
        KV_INFO("asset", "material '%s' ready: %s, texture '%s'", m.path, blend_mode_name(m.desc.blend), m.texture_path);
        e.events.emit(AssetReady{h});
    }
}

Handle asset_load_material(Engine& e, const char* path) {
    AssetState_* s = asset_state(e);
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    if (!s || !r || !io || !path) { KV_ERROR("asset", "cannot load material '%s': asset, renderer and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }

    Name name = name_intern(path);
    if (const Handle* existing = s->material_by_path.find(name)) return *existing;

    MaterialAsset m{};
    strncpy(m.path, path, sizeof m.path - 1);
    m.state    = AssetState::Pending;
    m.material = renderer_material_create(r, m.desc); // the placeholder look, until the file says otherwise
    if (m.material.is_null()) return Handle{};
    Handle h = s->materials.create(m);
    if (h.is_null()) { renderer_material_destroy(r, m.material); return Handle{}; }
    s->material_by_path.set(name, h);

    char full[512];
    snprintf(full, sizeof full, "%sassets/%s", platform_base_path(e), path);
    IoRequest req;
    req.path  = full;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_file(req);
    MaterialAsset* rec = s->materials.get(h);
    if (iq.is_null()) { fail_material(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
    return h;
}

const MaterialAsset* asset_material(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->materials.get(asset) : nullptr;
}

Handle asset_material_find(Engine& e, const char* path) {
    AssetState_* s = asset_state(e);
    if (!s || !path) return Handle{};
    Name name = name_lookup(path);
    if (name.is_null()) return Handle{};
    const Handle* h = s->material_by_path.find(name);
    return h ? *h : Handle{};
}

u32 asset_material_count(Engine& e) {
    AssetState_* s = asset_state(e);
    return s ? s->materials.len : 0;
}

const MaterialAsset* asset_material_at(Engine& e, u32 slot) {
    AssetState_* s = asset_state(e);
    return s && s->materials.is_alive(slot) ? &s->materials.items[slot] : nullptr;
}

Handle asset_material_handle_at(Engine& e, u32 slot) {
    AssetState_* s = asset_state(e);
    return s && s->materials.is_alive(slot) ? s->materials.handle_at(slot) : Handle{};
}

bool asset_material_patch(Engine& e, Handle asset, const MaterialDesc& desc) {
    AssetState_* s = asset_state(e);
    MaterialAsset* m = s ? s->materials.get(asset) : nullptr;
    if (!m) return false;
    Handle texture = m->desc.texture; // the texture follows its path, not the caller's handle
    m->desc = desc;
    m->desc.texture = texture;
    return renderer_material_set(renderer_get(e), m->material, m->desc);
}

bool asset_material_set_texture(Engine& e, Handle asset, const char* texture_path) {
    AssetState_* s = asset_state(e);
    MaterialAsset* m = s ? s->materials.get(asset) : nullptr;
    if (!m || (texture_path && strlen(texture_path) >= KV_ASSET_PATH_MAX)) return false;
    bind_texture(e, *m, texture_path);
    return true;
}

} // namespace kv
