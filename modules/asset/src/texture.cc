#include "korvid/asset/texture.h"
#include "korvid/asset/module.h"
#include "korvid/asset/png.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_internal.h"
#include <stdio.h>
#include <string.h>

namespace kv {

AssetState_* asset_state(Engine& e) { return engine_module_state_as<AssetState_>(e, "asset"); }

namespace {

void* asset_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->textures.get(Handle{h}); }
int asset_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    TextureAsset* t = ((AssetState_*)ctx)->textures.get(Handle{h});
    if (!t) return snprintf(out, cap, "stale");
    const char* st = t->state == AssetState::Ready ? "ready" : t->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %ux%u", t->path, st, t->width, t->height);
}

const char* k_error_names[(u32)AssetError::Count] = {"none", "not found", "read error", "decode error", "upload error", "parse error"};

void fail_asset(Engine& e, AssetState_& s, Handle h, TextureAsset& t, AssetError err, const char* detail) {
    t.state = AssetState::Failed;
    t.error = err;
    t.io    = Handle{};
    ++s.failed;
    KV_WARN("asset", "'%s': %s%s%s", t.path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// Phase PreSim: completions reaped by the last platform pump become textures and events.
void asset_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    Renderer* r = renderer_get(e);
    if (!io) return;
    // Scan what has finished, take what is ours. Reaping removes the entry, so the index
    // only advances past entries that stay.
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.kind)) { ++i; continue; }
        Handle h{c.user};
        TextureAsset* t = s.textures.get(h);
        if (!t || t->io != c.request) { ++i; continue; } // not ours, or no longer awaited
        io->reap(c.request);
        if (c.status != IoStatus::Done) {
            fail_asset(e, s, h, *t, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        PngImage img;
        const char* why = nullptr;
        bool ok = png_decode(c.data, c.size, s.general, img, &why);
        free_in(s.general, c.data, c.size);
        if (!ok) { fail_asset(e, s, h, *t, AssetError::Decode, why); continue; }
        t->width  = img.width;
        t->height = img.height;
        bool uploaded = r && renderer_texture_upload(r, t->texture, img.width, img.height, img.rgba, (usize)img.width * img.height * 4);
        free_in(s.general, img.rgba, (usize)img.width * img.height * 4);
        if (!uploaded) { fail_asset(e, s, h, *t, AssetError::Upload, nullptr); continue; }
        t->state = AssetState::Ready;
        t->io    = Handle{};
        ++s.loaded;
        KV_INFO("asset", "'%s' ready: %ux%u in slot %u", t->path, t->width, t->height, renderer_texture_slot(r, t->texture));
        e.events.emit(AssetReady{h});
    }
}

bool asset_startup(Engine& e, ModuleInstance& self) {
    AssetState_* s = arena_push<AssetState_>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.texture"; desc.type_key = type_key<TextureAsset>(); desc.ctx = s;
    desc.resolve = asset_resolve; desc.describe = asset_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s->kind = handle_kind_register(desc);
    if (!s->kind) return false;
    s->textures.use_kind(s->kind);
    if (!s->textures.init(4096, self.general.tag)) return false;
    if (!asset_materials_init(e, *s, self)) return false;
    self.set_state(s);
    return true;
}

void asset_shutdown(Engine& e, ModuleInstance& self) {
    AssetState_* s = self.state_as<AssetState_>();
    if (!s) return;
    asset_materials_shutdown(e, *s);
    // Renderer textures belong to the renderer and go with it; cancel reads still in flight.
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s->textures.len; ++i)
        if (s->textures.is_alive(i) && io && !s->textures.items[i].io.is_null()) io->cancel(s->textures.items[i].io);
    KV_INFO("asset", "%u textures loaded, %u failed", s->loaded, s->failed);
    kv_handle_kind_unregister(s->kind);
    s->by_path.free();
    s->textures.free();
}

constinit const EventTypeDesc k_events[]    = {KV_EVENT(AssetReady), KV_EVENT(AssetFailed)};
constinit const ResourceDesc  k_resources[] = {{"asset.textures"}, {"asset.materials"}};
constinit const char* const   k_pump_writes[] = {"asset.textures"};
constinit const char* const   k_materials_reads[]  = {"asset.textures"};
constinit const char* const   k_materials_writes[] = {"asset.materials"};
constinit const char* const   k_materials_after[]  = {"asset.pump"};
constinit const SystemDesc    k_systems[]   = {
    {.name = "asset.pump", .phase = Phase::PreSim, .fn = asset_pump, .writes = k_pump_writes, .main_thread = true, .runs = RunWhen::Always},
    {.name = "asset.pump_materials", .phase = Phase::PreSim, .fn = asset_materials_pump, .reads = k_materials_reads,
     .writes = k_materials_writes, .after = k_materials_after, .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

extern const CliCommandDesc k_asset_commands[3]; // material.cc

KV_MODULE_DEFINE(asset_module) = {
    .name      = "asset",
    .events    = k_events,
    .resources = k_resources,
    .systems   = k_systems,
    .commands  = Span<const CliCommandDesc>(k_asset_commands, 3),
    .startup   = asset_startup,
    .shutdown  = asset_shutdown,
    .memory    = {.reserve = 64 * 1024},
};

Handle asset_load_texture(Engine& e, const char* path) {
    AssetState_* s = asset_state(e);
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    if (!s || !r || !io || !path) { KV_ERROR("asset", "cannot load '%s': asset, renderer and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= sizeof(TextureAsset::path)) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }

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

    char full[512];
    snprintf(full, sizeof full, "%sassets/%s", platform_base_path(e), path);
    IoRequest req;
    req.path  = full;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_file(req);
    if (iq.is_null()) {
        TextureAsset* rec = s->textures.get(h);
        fail_asset(e, *s, h, *rec, AssetError::Read, "could not submit");
        return h;
    }
    s->textures.get(h)->io = iq;
    return h;
}

const TextureAsset* asset_texture(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->textures.get(asset) : nullptr;
}

const char* asset_error_name(AssetError err) {
    return (u32)err < (u32)AssetError::Count ? k_error_names[(u32)err] : "?";
}

} // namespace kv
