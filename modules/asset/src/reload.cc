// Korvid asset: reload (korvid/asset/reload.h). A loaded file read again by path; each kind's
// pump decides, when the bytes land, whether they changed and what the record becomes.
#include "korvid/asset/reload.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

bool asset_reread(Engine& e, Allocator* alloc, Handle h, const AssetReread& r) {
    if (!r.io->is_null()) { *r.again = true; return true; } // once what is out lands
    IoQueue* io = platform_io(e);
    if (!io) return false;
    IoRequest req;
    req.path  = r.path;
    req.alloc = alloc;
    req.user  = h.bits;
    req.owner = asset_io_owner(h.kind());
    Handle q = io->read_asset(req);
    if (q.is_null()) { KV_WARN("asset", "'%s': could not read it again", r.path); return false; }
    *r.io = q;
    *r.reread = *r.state == AssetState::Ready;
    if (*r.state == AssetState::Failed) *r.state = AssetState::Pending; // a load from the start
    return true;
}

void asset_reread_after(Engine& e, Allocator* alloc, Handle h, const AssetReread& r) {
    if (!*r.again) return;
    *r.again = false;
    asset_reread(e, alloc, h, r);
}

void asset_reload_failed(Engine& e, Handle h, const char* path, AssetError err, const char* detail) {
    KV_WARN("asset", "'%s' read again: %s%s%s; keeping what it was", path, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err, true});
}

void asset_reloaded(Engine& e, Handle h, const char* path, u32& generation) {
    ++generation;
    KV_INFO("asset", "'%s' reloaded (generation %u)", path, generation);
    e.events.emit(AssetReloaded{h});
}

void asset_reload_unchanged(const char* path) { KV_DEBUG_LOG("asset", "'%s' read again: unchanged", path); }

namespace {

template <class R>
u32 reread_by_path(Engine& e, AssetState_& s, Pool<R>& pool, Map<Name, Handle>& by_path, const char* path) {
    Name n = name_lookup(path);
    if (n.hash == 0) return 0;
    const Handle* h = by_path.find(n);
    R* r = h ? pool.get(*h) : nullptr;
    return r && asset_reread(e, s.general, *h, asset_reread_of(*r)) ? 1 : 0;
}

template <class R>
u32 reread_every(Engine& e, AssetState_& s, Pool<R>& pool) {
    u32 n = 0;
    for (u32 i = 0; i < pool.len; ++i)
        if (pool.is_alive(i) && asset_reread(e, s.general, pool.handle_at(i), asset_reread_of(pool.items[i]))) ++n;
    return n;
}

} // namespace

u32 asset_reload(Engine& e, const char* path) {
    AssetState_* s = asset_state(e);
    if (!s || !path || !path[0]) return 0;
    u32 n = reread_by_path(e, *s, s->blobs, s->blob_by_path, path)
          + reread_by_path(e, *s, s->shapes, s->shape_by_path, path);
    // A shape is loaded by its source's name and kept by its cook's: x.svg is x.kshape.
    usize len = strlen(path);
    if (len > 4 && !strcmp(path + len - 4, ".svg")) {
        char cooked[KV_ASSET_PATH_MAX + 8];
        format(cooked, sizeof cooked, "%.*s.kshape", (int)(len - 4), path);
        n += reread_by_path(e, *s, s->shapes, s->shape_by_path, cooked);
    }
    for (u32 i = 0; i < s->external_count; ++i)
        if (s->external[i].reload) n += s->external[i].reload(e, s->external[i].user, path);
    return n;
}

u32 asset_reload_all(Engine& e) {
    AssetState_* s = asset_state(e);
    if (!s) return 0;
    u32 n = reread_every(e, *s, s->blobs) + reread_every(e, *s, s->shapes);
    for (u32 i = 0; i < s->external_count; ++i)
        if (s->external[i].reload) n += s->external[i].reload(e, s->external[i].user, nullptr);
    return n;
}

void asset_cmd_reload(CliContext& ctx, Span<const char* const> args) {
    Engine& e = *ctx.engine;
    if (!args.len) { ctx.print("%u assets read again", asset_reload_all(e)); return; }
    u32 total = 0;
    for (const char* path : args) {
        u32 n = asset_reload(e, path);
        if (!n) ctx.print("%s: nothing loaded from it", path);
        total += n;
    }
    ctx.print("%u assets read again", total);
}

} // namespace kv
