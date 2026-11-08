#include "korvid/asset/blob.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

void* blob_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->blobs.get(Handle{h}); }
int blob_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    BlobAsset* b = ((AssetState_*)ctx)->blobs.get(Handle{h});
    if (!b) return snprintf(out, cap, "stale");
    const char* st = b->state == AssetState::Ready ? "ready" : b->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %llu bytes", b->path, st, (unsigned long long)b->size);
}

void fail_blob(Engine& e, AssetState_& s, Handle h, BlobAsset& b, AssetError err) {
    b.state = AssetState::Failed;
    b.error = err;
    b.io    = Handle{};
    ++s.blobs_failed;
    KV_WARN("asset", "'%s': %s", b.path, asset_error_name(err));
    e.events.emit(AssetFailed{h, err});
}

} // namespace

bool asset_blobs_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.blob_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.blob"; desc.type_key = type_key<BlobAsset>(); desc.ctx = &s;
    desc.resolve = blob_resolve; desc.describe = blob_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.blob_kind = handle_kind_register(desc);
    if (!s.blob_kind) return false;
    s.blobs.use_kind(s.blob_kind);
    return s.blobs.init(4096, self.general.tag);
}

void asset_blobs_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.blobs.len; ++i) {
        if (!s.blobs.is_alive(i)) continue;
        BlobAsset& b = s.blobs.items[i];
        if (io && !b.io.is_null()) io->cancel(b.io);
        free_in(s.general, b.data, b.size + 1);
    }
    KV_INFO("asset", "%u blobs loaded, %u failed", s.blobs_loaded, s.blobs_failed);
    kv_handle_kind_unregister(s.blob_kind);
    s.blob_by_path.free();
    s.blobs.free();
}

// Phase PreSim, main thread: files that arrived become blobs, with a NUL after them.
void asset_blobs_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_reap_orphans(e, s, s.blob_kind);
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.blob_kind)) { ++i; continue; }
        Handle h{c.user};
        BlobAsset* b = s.blobs.get(h);
        if (!b || b->io != c.request) { ++i; continue; }
        io->reap(c.request);
        b->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_blob(e, s, h, *b, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read);
            continue;
        }
        // The read's buffer is exactly the file; the blob wants one byte more for the NUL.
        u8* data = (u8*)alloc_in(s.general, c.size + 1);
        if (data) { memcpy(data, c.data, c.size); data[c.size] = 0; }
        free_in(s.general, c.data, c.size ? c.size : 1);
        if (!data) { fail_blob(e, s, h, *b, AssetError::Read); continue; }
        b->data  = data;
        b->size  = c.size;
        b->state = AssetState::Ready;
        ++s.blobs_loaded;
        KV_INFO("asset", "'%s' ready: %llu bytes", b->path, (unsigned long long)b->size);
        e.events.emit(AssetReady{h});
    }
}

Handle blob_load(Engine& e, AssetState_& s, const char* path) {
    IoQueue* io = platform_io(e);
    if (!io || !path) { KV_ERROR("asset", "cannot load '%s': asset and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s.blob_by_path.find(name)) return *existing;
    BlobAsset b{};
    strncpy(b.path, path, sizeof b.path - 1);
    Handle h = s.blobs.create(b);
    if (h.is_null()) return Handle{};
    s.blob_by_path.set(name, h);
    IoRequest req;
    req.path  = path;
    req.alloc = s.general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    BlobAsset* rec = s.blobs.get(h);
    if (iq.is_null()) { fail_blob(e, s, h, *rec, AssetError::Read); return h; }
    rec->io = iq;
    return h;
}

void blob_retire(Engine& e, AssetState_& s, Handle h) {
    BlobAsset* b = s.blobs.get(h);
    if (!b) return;
    IoQueue* io = platform_io(e);
    if (io && !b->io.is_null()) io->cancel(b->io);
    free_in(s.general, b->data, b->size + 1);
    s.blob_by_path.remove(name_intern(b->path));
    KV_INFO("asset", "'%s' retired", b->path);
    s.blobs.destroy(h);
}

Handle asset_load_blob(Engine& e, const char* path, Handle scope) {
    AssetState_* s = asset_state(e);
    if (!s) { KV_ERROR("asset", "cannot load '%s': the asset module is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(*s, scope, blob_load(e, *s, path));
}

Handle asset_load_blob(Engine& e, const char* path) { return asset_load_blob(e, path, asset_scope_global(e)); }

const BlobAsset* asset_blob(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->blobs.get(asset) : nullptr;
}

} // namespace kv
