// Korvid asset_render: textures (korvid/asset_render/texture.h). A PNG read through the I/O
// queue, decoded and uploaded in the pump, the kind registered with the asset module.
#include "korvid/asset_render/texture.h"
#include "korvid/asset/reload.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/image/png.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_render_internal.h"
#include <string.h>

namespace kv {

namespace {

void* asset_resolve(kv_handle h, void* ctx) { return ((AssetRenderState*)ctx)->textures.get(Handle{h}); }
const char* asset_name_of(kv_handle h, void* ctx) { auto* a = ((AssetRenderState*)ctx)->textures.get(Handle{h}); return a ? a->path : nullptr; }
kv_handle asset_find_name(const char* name, void* ctx) {
    const Handle* h = ((AssetRenderState*)ctx)->by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int asset_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    TextureAsset* t = ((AssetRenderState*)ctx)->textures.get(Handle{h});
    if (!t) return format(out, cap, "stale");
    const char* st = t->state == AssetState::Ready ? "ready" : t->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s %ux%u", t->path, st, t->width, t->height);
}

void fail_asset(Engine& e, AssetRenderState& s, Handle h, TextureAsset& t, AssetError err, const char* detail) {
    t.state = AssetState::Failed;
    t.error = err;
    t.io    = Handle{};
    ++s.failed;
    KV_WARN("asset_render", "'%s': %s%s%s", t.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// A read of the file has landed (reaped): the first load's, or a reload's. A reload that
// cannot be used keeps the texture as it was; one whose bytes are the same does nothing.
void land(Engine& e, AssetRenderState& s, Renderer* r, Handle h, TextureAsset& t, const IoCompletion& c) {
    bool re = t.reread;
    t.reread = false;
    if (c.status != IoStatus::Done) {
        AssetError err = c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read;
        if (re) asset_reload_failed(e, h, t.path, err, nullptr);
        else fail_asset(e, s, h, t, err, nullptr);
        return;
    }
    u64 hash = hash_bytes(c.data, c.size);
    if (re && hash == t.hash) { free_in(s.general, c.data, c.size); asset_reload_unchanged(t.path); return; }
    PngImage img;
    const char* why = nullptr;
    bool ok = png_decode(c.data, c.size, s.general, img, &why);
    free_in(s.general, c.data, c.size);
    if (!ok) {
        if (re) asset_reload_failed(e, h, t.path, AssetError::Decode, why);
        else fail_asset(e, s, h, t, AssetError::Decode, why);
        return;
    }
    // The same renderer texture, new pixels: a new size makes a new image behind the handle,
    // the old one destroyed once the frames in flight are done with it.
    bool uploaded = r && renderer_texture_upload(r, t.texture, img.width, img.height, img.rgba, (usize)img.width * img.height * 4);
    free_in(s.general, img.rgba, (usize)img.width * img.height * 4);
    if (!uploaded) {
        if (re) asset_reload_failed(e, h, t.path, AssetError::Upload, nullptr);
        else fail_asset(e, s, h, t, AssetError::Upload, nullptr);
        return;
    }
    t.width  = img.width;
    t.height = img.height;
    t.hash   = hash;
    if (re) { asset_reloaded(e, h, t.path, t.generation); return; }
    t.state = AssetState::Ready;
    ++s.loaded;
    KV_INFO("asset_render", "'%s' ready: %ux%u in slot %u", t.path, t.width, t.height, renderer_texture_slot(r, t.texture));
    e.events.emit(AssetReady{h});
}

} // namespace

// Phase PreSim: completions reaped by the last platform pump become textures and events.
void asset_textures_pump(TickContext& ctx) {
    AssetRenderState& s = ctx.state_as<AssetRenderState>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    Renderer* r = renderer_get(e);
    if (!io) return;
    asset_render_reap_orphans(e, s, s.kind);
    // Scan what has finished, take what is ours. Reaping removes the entry, so the index
    // only advances past entries that stay.
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (Handle{c.user}.kind() != s.kind) { ++i; continue; }
        Handle h{c.user};
        TextureAsset* t = s.textures.get(h);
        if (!t || t->io != c.request) { ++i; continue; } // retired (reaped below), or no longer awaited
        io->reap(c.request);
        t->io = Handle{};
        land(e, s, r, h, *t, c);
        if (TextureAsset* again = s.textures.get(h)) asset_reread_after(e, s.general, h, asset_reread_of(*again));
    }
}

namespace {

u32* texture_refs(void* user, Handle h) { TextureAsset* t = ((AssetRenderState*)user)->textures.get(h); return t ? &t->refs : nullptr; }
AssetState texture_state(void* user, Handle h) { TextureAsset* t = ((AssetRenderState*)user)->textures.get(h); return t ? t->state : AssetState::Failed; }
u64 texture_bytes(void* user, Handle h) { TextureAsset* t = ((AssetRenderState*)user)->textures.get(h); return t ? (u64)t->width * t->height * 4 : 0; }
const char* texture_path(void* user, Handle h) { TextureAsset* t = ((AssetRenderState*)user)->textures.get(h); return t ? t->path : nullptr; }
void texture_retire(Engine& e, void* user, Handle h);
Handle texture_load(Engine& e, void*, const char* path, Handle scope) { return asset_load_texture(e, path, scope); }

u32 texture_reload(Engine& e, void* user, const char* path) {
    AssetRenderState& s = *(AssetRenderState*)user;
    if (!path) {
        u32 n = 0;
        for (u32 i = 0; i < s.textures.len; ++i)
            if (s.textures.is_alive(i) && asset_reread(e, s.general, s.textures.handle_at(i), asset_reread_of(s.textures.items[i]))) ++n;
        return n;
    }
    Name name = name_lookup(path);
    const Handle* h = name.hash ? s.by_path.find(name) : nullptr;
    TextureAsset* t = h ? s.textures.get(*h) : nullptr;
    return t && asset_reread(e, s.general, *h, asset_reread_of(*t)) ? 1 : 0;
}

} // namespace

bool asset_textures_init(Engine& e, AssetRenderState& s, ModuleInstance& self) {
    s.by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset_render.texture"; desc.type_key = type_key<TextureAsset>(); desc.ctx = &s;
    desc.resolve = asset_resolve; desc.name_of = asset_name_of; desc.find_name = asset_find_name; desc.describe = asset_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.kind = handle_kind_register(desc);
    if (!s.kind) return false;
    s.textures.use_kind(s.kind);
    if (!s.textures.init(4096, self.general.tag)) return false;
    AssetKindDesc k;
    k.kind = s.kind; k.refs = texture_refs; k.state = texture_state; k.bytes = texture_bytes; k.path = texture_path; k.retire = texture_retire;
    k.reload = texture_reload; k.suffix = ".png"; k.load = texture_load; k.user = &s;
    if (!asset_kind_register(e, k)) { KV_ERROR("asset_render", "could not register textures with the asset module: is it up?"); return false; }
    return true;
}

void asset_textures_shutdown(Engine& e, AssetRenderState& s) {
    asset_kind_unregister(e, s.kind);
    // Renderer textures belong to the renderer and go with it; cancel reads still in flight.
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.textures.len; ++i)
        if (s.textures.is_alive(i) && io && !s.textures.items[i].io.is_null()) io->cancel(s.textures.items[i].io);
    KV_INFO("asset_render", "%u textures loaded, %u failed", s.loaded, s.failed);
    kv_handle_kind_unregister(s.kind);
    s.by_path.free();
    s.textures.free();
}

Handle asset_load_texture(Engine& e, const char* path, Handle scope) {
    AssetRenderState* s = asset_render_state(e);
    if (!s) { KV_ERROR("asset_render", "cannot load '%s': asset_render is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(e, scope, texture_load(e, *s, path));
}

Handle asset_load_texture(Engine& e, const char* path) { return asset_load_texture(e, path, asset_scope_global(e)); }

namespace {

void texture_retire(Engine& e, void* user, Handle h) {
    AssetRenderState& s = *(AssetRenderState*)user;
    TextureAsset* t = s.textures.get(h);
    if (!t) return;
    IoQueue* io = platform_io(e);
    if (io && !t->io.is_null()) io->cancel(t->io); // its completion is reaped as an orphan
    if (Renderer* r = renderer_get(e)) renderer_texture_destroy(r, t->texture);
    s.by_path.remove(name_intern(t->path));
    KV_INFO("asset_render", "'%s' retired", t->path);
    s.textures.destroy(h);
}

} // namespace

Handle texture_load(Engine& e, AssetRenderState& st, const char* path) {
    AssetRenderState* s = &st;
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    if (!r || !io || !path) { KV_ERROR("asset_render", "cannot load '%s': asset_render, the renderer and the platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= sizeof(TextureAsset::path)) { KV_ERROR("asset_render", "path too long: '%s'", path); return Handle{}; }

    // The same path is the same asset.
    Name name = name_intern(path);
    if (const Handle* existing = s->by_path.find(name)) return *existing;

    TextureAsset t{};
    strncpy(t.path, path, sizeof t.path - 1);
    t.state = AssetState::Pending;
    // The slot exists from now on and shows the placeholder; the upload brings the image.
    TextureDesc td;
    td.name = path;
    t.texture = renderer_texture_create(r, td);
    if (t.texture.is_null()) return Handle{};
    Handle h = s->textures.create(t);
    if (h.is_null()) { renderer_texture_destroy(r, t.texture); return Handle{}; }
    s->by_path.set(name, h);

    IoRequest req;
    req.path  = path;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    if (iq.is_null()) {
        TextureAsset* rec = s->textures.get(h);
        fail_asset(e, *s, h, *rec, AssetError::Read, "could not submit");
        return h;
    }
    s->textures.get(h)->io = iq;
    return h;
}

const TextureAsset* asset_texture(Engine& e, Handle asset) {
    AssetRenderState* s = asset_render_state(e);
    return s ? s->textures.get(asset) : nullptr;
}

} // namespace kv
