// Korvid asset: shapes (korvid/asset/shape.h): a .kshape read, as blob.cc reads bytes, then
// decoded into a patherizer shape on the main thread when it lands.
#include "korvid/asset/shape.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

namespace {

void* shape_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->shapes.get(Handle{h}); }
const char* shape_name_of(kv_handle h, void* ctx) { auto* a = ((AssetState_*)ctx)->shapes.get(Handle{h}); return a ? a->path : nullptr; }
kv_handle shape_find_name(const char* name, void* ctx) {
    const Handle* h = ((AssetState_*)ctx)->shape_by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int shape_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    ShapeAsset* b = ((AssetState_*)ctx)->shapes.get(Handle{h});
    if (!b) return format(out, cap, "stale");
    const char* st = b->state == AssetState::Ready ? "ready" : b->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s, %u nodes", b->path, st, b->shape.nodes.len);
}

void fail_shape(Engine& e, AssetState_& s, Handle h, ShapeAsset& b, AssetError err) {
    b.state = AssetState::Failed;
    b.error = err;
    b.io    = Handle{};
    ++s.shapes_failed;
    KV_WARN("asset", "'%s': %s", b.path, asset_error_name(err));
    e.events.emit(AssetFailed{h, err});
}

} // namespace

bool asset_shapes_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.shape_by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.shape"; desc.type_key = type_key<ShapeAsset>(); desc.ctx = &s;
    desc.resolve = shape_resolve; desc.name_of = shape_name_of; desc.find_name = shape_find_name; desc.describe = shape_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.shape_kind = handle_kind_register(desc);
    if (!s.shape_kind) return false;
    s.shapes.use_kind(s.shape_kind);
    return s.shapes.init(4096, self.general.tag);
}

void asset_shapes_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.shapes.len; ++i) {
        if (!s.shapes.is_alive(i)) continue;
        ShapeAsset& b = s.shapes.items[i];
        if (io && !b.io.is_null()) io->cancel(b.io);
        b.shape.free();
    }
    if (s.shapes_loaded || s.shapes_failed) KV_INFO("asset", "%u shapes loaded, %u failed", s.shapes_loaded, s.shapes_failed);
    kv_handle_kind_unregister(s.shape_kind);
    s.shape_by_path.free();
    s.shapes.free();
}

// Phase PreSim, main thread: files that arrived become shapes.
void asset_shapes_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_reap_orphans(e, s, s.shape_kind);
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.shape_kind)) { ++i; continue; }
        Handle h{c.user};
        ShapeAsset* b = s.shapes.get(h);
        if (!b || b->io != c.request) { ++i; continue; }
        io->reap(c.request);
        b->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_shape(e, s, h, *b, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read);
            continue;
        }
        // The file read into the shape; the read's buffer goes back at once.
        b->shape.init(s.general);
        const char* why = "";
        bool ok = patherizer_read_kshape(c.data, c.size, b->shape, &why);
        free_in(s.general, c.data, c.size ? c.size : 1);
        if (!ok) {
            KV_WARN("asset", "'%s': %s", b->path, why);
            b->shape.free();
            fail_shape(e, s, h, *b, AssetError::Decode);
            continue;
        }
        b->one_colour = b->shape.one_colour();
        b->state = AssetState::Ready;
        ++s.shapes_loaded;
        KV_INFO("asset", "'%s' ready: %u nodes%s", b->path, b->shape.nodes.len, b->one_colour ? ", one colour" : "");
        e.events.emit(AssetReady{h});
    }
}

Handle shape_load(Engine& e, AssetState_& s, const char* source) {
    IoQueue* io = platform_io(e);
    if (!io || !source) { KV_ERROR("asset", "cannot load '%s': asset and platform must be up", source ? source : "?"); return Handle{}; }
    // The source's cook: x.svg is x.kshape.
    char cooked[KV_ASSET_PATH_MAX + 8];
    usize sl = strlen(source);
    if (sl > 4 && !strcmp(source + sl - 4, ".svg")) format(cooked, sizeof cooked, "%.*s.kshape", (int)(sl - 4), source);
    else format(cooked, sizeof cooked, "%s", source);
    const char* path = cooked;
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    if (const Handle* existing = s.shape_by_path.find(name)) return *existing;
    ShapeAsset b{};
    strncpy(b.path, path, sizeof b.path - 1);
    Handle h = s.shapes.create(b);
    if (h.is_null()) return Handle{};
    s.shape_by_path.set(name, h);
    IoRequest req;
    req.path  = path;
    req.alloc = s.general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    ShapeAsset* rec = s.shapes.get(h);
    if (iq.is_null()) { fail_shape(e, s, h, *rec, AssetError::Read); return h; }
    rec->io = iq;
    return h;
}

void shape_retire(Engine& e, AssetState_& s, Handle h) {
    ShapeAsset* b = s.shapes.get(h);
    if (!b) return;
    IoQueue* io = platform_io(e);
    if (io && !b->io.is_null()) io->cancel(b->io);
    b->shape.free();
    s.shape_by_path.remove(name_intern(b->path));
    KV_INFO("asset", "'%s' retired", b->path);
    s.shapes.destroy(h);
}

Handle asset_load_shape(Engine& e, const char* path, Handle scope) {
    AssetState_* s = asset_state(e);
    if (!s) { KV_ERROR("asset", "cannot load '%s': the asset module is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(*s, scope, shape_load(e, *s, path));
}

Handle asset_load_shape(Engine& e, const char* path) { return asset_load_shape(e, path, asset_scope_global(e)); }

const ShapeAsset* asset_shape(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->shapes.get(asset) : nullptr;
}

} // namespace kv
