// The tilemap module: records in the asset module's scopes, the load pipeline (map blob,
// tileset blobs, textures, materials), and instances as rows of cosmetic sprites.
#include "korvid/tilemap/module.h"
#include "korvid/tilemap/tilemap.h"
#include "korvid/asset/blob.h"
#include "korvid/asset/scope.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/name.h"
#include "korvid/core/pool.h"
#include "korvid/core/utf8.h"
#include "korvid/render/renderer.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "tilemap_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

constexpr usize kMapArena = 64u << 20; // address space per map; pages commit as the parse uses them

struct Instance {
    Handle map;
    Handle owner;
    Handle* rows = nullptr; // in the module's allocator
    u32     row_count = 0;
};

struct TilemapState {
    Allocator*         general = nullptr;
    Pool<TilemapAsset> maps;
    Map<Name, Handle>  by_path;
    Pool<Instance>     instances;
    u16                map_kind = 0, instance_kind = 0;
    u32                tile_kind = 0;
    u32                loaded = 0, failed = 0;
};

TilemapState* state(Engine& e) { return engine_module_state_as<TilemapState>(e, "tilemap"); }

// The map's dependencies while loading: its blob, its external tilesets' blobs. Kept in
// the record's arena so the record stays plain data.
struct Deps { Handle tileset_blob[64]; };
Deps* deps_of(TilemapAsset& m) { return m.mem ? (Deps*)((Arena*)m.mem + 1) : nullptr; }

Arena* arena_of(TilemapAsset& m) { return (Arena*)m.mem; }

void fail_map(Engine& e, TilemapState& s, Handle h, TilemapAsset& m, AssetError err, const char* why) {
    m.state = AssetState::Failed;
    m.error = err;
    if (why) utf8_copy(m.error_text, sizeof m.error_text, why);
    ++s.failed;
    KV_WARN("tilemap", "'%s': %s%s%s", m.path, asset_error_name(err), why ? ": " : "", why ? why : "");
    e.events.emit(AssetFailed{h, err});
}

// What holds a map's memory and references, released when it retires or reloads.
void drop_contents(Engine& e, TilemapState& s, TilemapAsset& m) {
    Renderer* r = renderer_get(e);
    for (u32 i = 0; i < m.tileset_count; ++i) {
        MapTileset& t = m.tilesets[i];
        if (r && !t.material.is_null()) renderer_material_destroy(r, t.material);
        asset_unref(e, t.texture);
        t.material = t.texture = Handle{};
    }
    if (Deps* d = deps_of(m)) for (Handle& b : d->tileset_blob) { asset_unref(e, b); b = Handle{}; }
    asset_unref(e, m.blob);
    m.blob = Handle{};
    if (Arena* a = arena_of(m)) { arena_destroy(*a); free_in(s.general, a, sizeof(Arena) + sizeof(Deps)); }
    m.mem = nullptr;
}

// ---- the asset kind --------------------------------------------------------------------------------

u32*        kind_refs(void* user, Handle h) { TilemapAsset* m = ((TilemapState*)user)->maps.get(h); return m ? &m->refs : nullptr; }
AssetState  kind_state(void* user, Handle h) { TilemapAsset* m = ((TilemapState*)user)->maps.get(h); return m ? m->state : AssetState::Failed; }
const char* kind_path(void* user, Handle h) { TilemapAsset* m = ((TilemapState*)user)->maps.get(h); return m ? m->path : "?"; }
u64 kind_bytes(void* user, Handle h) {
    TilemapAsset* m = ((TilemapState*)user)->maps.get(h);
    Arena* a = m ? (Arena*)m->mem : nullptr;
    return a ? a->used : 0;
}
void kind_retire(Engine& e, void* user, Handle h) {
    TilemapState& s = *(TilemapState*)user;
    TilemapAsset* m = s.maps.get(h);
    if (!m) return;
    drop_contents(e, s, *m);
    s.by_path.remove(name_intern(m->path));
    KV_INFO("tilemap", "'%s' retired", m->path);
    s.maps.destroy(h);
}

void* map_resolve(kv_handle h, void* ctx) { return ((TilemapState*)ctx)->maps.get(Handle{h}); }
int map_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    TilemapAsset* m = ((TilemapState*)ctx)->maps.get(Handle{h});
    if (!m) return snprintf(out, cap, "stale");
    const char* st = m->state == AssetState::Ready ? "ready" : m->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %s %ux%u of %ux%u, %u layers", m->path, st, m->width, m->height, m->tile_w, m->tile_h, m->layer_count);
}
void* instance_resolve(kv_handle h, void* ctx) { return ((TilemapState*)ctx)->instances.get(Handle{h}); }
int instance_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    Instance* in = ((TilemapState*)ctx)->instances.get(Handle{h});
    return in ? snprintf(out, cap, "%u rows", in->row_count) : snprintf(out, cap, "stale");
}

// ---- loading -----------------------------------------------------------------------------------

// One step of each pending map: parse once the file is in, read the tilesets it names,
// load their images, make their materials, build the solids, turn Ready.
void advance(Engine& e, TilemapState& s, Handle h, TilemapAsset& m) {
    Arena* arena = arena_of(m);
    char why[160];
    if (m.stage == 0) { // the map file
        const BlobAsset* b = asset_blob(e, m.blob);
        if (!b || b->state == AssetState::Failed) { fail_map(e, s, h, m, b ? b->error : AssetError::Read, nullptr); return; }
        if (b->state != AssetState::Ready) return;
        if (!tilemap_parse_map((const char*)b->data, b->size, m.path, &arena->a, m, why, sizeof why)) { fail_map(e, s, h, m, AssetError::Parse, why); return; }
        Deps* d = deps_of(m);
        if (m.tileset_count > KV_COUNTOF(d->tileset_blob)) { fail_map(e, s, h, m, AssetError::Parse, "more than 64 tilesets"); return; }
        for (u32 i = 0; i < m.tileset_count; ++i)
            if (!m.tilesets[i].image) { d->tileset_blob[i] = asset_load_blob(e, m.tilesets[i].name, Handle{}); asset_ref(e, d->tileset_blob[i]); }
        m.stage = 1;
    }
    if (m.stage == 1) { // external tilesets
        Deps* d = deps_of(m);
        for (u32 i = 0; i < m.tileset_count; ++i) {
            if (m.tilesets[i].image) continue;
            const BlobAsset* b = asset_blob(e, d->tileset_blob[i]);
            if (!b || b->state == AssetState::Failed) { snprintf(why, sizeof why, "tileset '%s' did not load", m.tilesets[i].name); fail_map(e, s, h, m, AssetError::NotFound, why); return; }
            if (b->state != AssetState::Ready) return;
        }
        for (u32 i = 0; i < m.tileset_count; ++i) {
            if (m.tilesets[i].image) continue;
            const BlobAsset* b = asset_blob(e, d->tileset_blob[i]);
            char base[KV_ASSET_PATH_MAX];
            utf8_copy(base, sizeof base, m.tilesets[i].name);
            if (!tilemap_parse_tileset((const char*)b->data, b->size, base, &arena->a, m, i, why, sizeof why)) {
                char full[KV_ASSET_PATH_MAX + sizeof why + 4];
                snprintf(full, sizeof full, "%s: %s", base, why);
                fail_map(e, s, h, m, AssetError::Parse, full);
                return;
            }
        }
        for (u32 i = 0; i < m.tileset_count; ++i) {
            m.tilesets[i].texture = asset_load_texture(e, m.tilesets[i].image, Handle{});
            asset_ref(e, m.tilesets[i].texture);
        }
        m.stage = 2;
    }
    if (m.stage == 2) { // the images
        for (u32 i = 0; i < m.tileset_count; ++i) {
            const TextureAsset* t = asset_texture(e, m.tilesets[i].texture);
            if (t && t->state == AssetState::Pending) return;
        }
        Renderer* r = renderer_get(e);
        for (u32 i = 0; i < m.tileset_count; ++i) {
            const TextureAsset* t = asset_texture(e, m.tilesets[i].texture);
            if (!t || t->state == AssetState::Failed) KV_WARN("tilemap", "'%s': tileset image '%s' failed; its tiles show the placeholder", m.path, m.tilesets[i].image);
            if (r) {
                MaterialDesc md;
                md.texture = t ? t->texture : Handle{};
                md.blend   = BlendMode::Alpha;
                m.tilesets[i].material = renderer_material_create(r, md);
            }
        }
        tilemap_build_solids(m, &arena->a);
        // The files are parsed: their bytes can go.
        Deps* d = deps_of(m);
        for (Handle& b : d->tileset_blob) { asset_unref(e, b); b = Handle{}; }
        asset_unref(e, m.blob);
        m.blob  = Handle{};
        m.state = AssetState::Ready;
        m.stage = 3;
        ++s.loaded;
        KV_INFO("tilemap", "'%s' ready: %ux%u of %ux%u px, %u layers, %u objects, %u tilesets, %u solid shapes, %llu KB", m.path, m.width, m.height,
                m.tile_w, m.tile_h, m.layer_count, m.object_count, m.tileset_count, m.solid_rect_count, (unsigned long long)(arena->used / 1024));
        e.events.emit(AssetReady{h});
    }
}

// Phase PreSim, main thread, after the asset pumps: loads advance; instances whose owner
// died or whose map retired lose their rows.
void tilemap_pump(TickContext& ctx) {
    TilemapState& s = ctx.state_as<TilemapState>();
    Engine& e = *ctx.engine;
    for (u32 i = 0; i < s.maps.len; ++i) {
        if (!s.maps.is_alive(i)) continue;
        TilemapAsset& m = s.maps.items[i];
        if (m.state == AssetState::Pending) advance(e, s, s.maps.handle_at(i), m);
    }
    Table* tiles = ctx.write_kind(s.tile_kind);
    for (u32 i = 0; i < s.instances.len; ++i) {
        if (!s.instances.is_alive(i)) continue;
        Instance& in = s.instances.items[i];
        Table* ot = in.owner.is_null() ? nullptr : engine_table_of(e, in.owner);
        bool owner_gone = !in.owner.is_null() && (!ot || !ot->valid(in.owner));
        if (!owner_gone && s.maps.get(in.map)) continue;
        if (tiles) for (u32 k = 0; k < in.row_count; ++k) tiles->destroy(in.rows[k]);
        free_in(s.general, in.rows, sizeof(Handle) * (in.row_count ? in.row_count : 1));
        s.instances.destroy(s.instances.handle_at(i));
    }
}

bool tilemap_startup(Engine& e, ModuleInstance& self) {
    TilemapState* s = arena_push<TilemapState>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    s->by_path.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "tilemap.map"; desc.type_key = type_key<TilemapAsset>(); desc.ctx = s;
    desc.resolve = map_resolve; desc.describe = map_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s->map_kind = handle_kind_register(desc);
    desc.name = "tilemap.instance"; desc.type_key = type_key<Instance>();
    desc.resolve = instance_resolve; desc.describe = instance_describe;
    s->instance_kind = handle_kind_register(desc);
    if (!s->map_kind || !s->instance_kind) return false;
    s->maps.use_kind(s->map_kind);
    s->instances.use_kind(s->instance_kind);
    if (!s->maps.init(1024, self.general.tag) || !s->instances.init(1024, self.general.tag)) return false;
    s->tile_kind = engine_kind_id(e, "tilemap.tile");
    AssetKindDesc k;
    k.kind = s->map_kind; k.refs = kind_refs; k.state = kind_state; k.bytes = kind_bytes; k.path = kind_path; k.retire = kind_retire; k.user = s;
    if (!asset_kind_register(e, k)) return false;
    self.set_state(s);
    return true;
}

void tilemap_shutdown(Engine& e, ModuleInstance& self) {
    TilemapState* s = self.state_as<TilemapState>();
    if (!s) return;
    asset_kind_unregister(e, s->map_kind);
    for (u32 i = 0; i < s->instances.len; ++i)
        if (s->instances.is_alive(i)) free_in(s->general, s->instances.items[i].rows, sizeof(Handle) * (s->instances.items[i].row_count ? s->instances.items[i].row_count : 1));
    for (u32 i = 0; i < s->maps.len; ++i)
        if (s->maps.is_alive(i)) drop_contents(e, *s, s->maps.items[i]);
    KV_INFO("tilemap", "%u maps loaded, %u failed", s->loaded, s->failed);
    kv_handle_kind_unregister(s->map_kind);
    kv_handle_kind_unregister(s->instance_kind);
    s->instances.free();
    s->maps.free();
    s->by_path.free();
}

constinit const KindDesc k_kinds[] = {
    // Tiles are presentation: never hashed. What collides is the map's solids, not these.
    {.name = ".tile", .components = "Transform2, Sprite", .capacity = 1u << 18, .cosmetic = true},
};
constinit const SystemDesc k_systems[] = {
    {.name = ".pump", .phase = Phase::PreSim, .fn = tilemap_pump, .writes = ".tile",
     .after = "asset.pump_scopes, asset.pump, asset.pump_blobs", .main_thread = true, .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(tilemap_module) = {
    .name     = "tilemap",
    .kinds    = k_kinds,
    .systems  = k_systems,
    .startup  = tilemap_startup,
    .shutdown = tilemap_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

Handle tilemap_load(Engine& e, const char* path, Handle scope) {
    TilemapState* s = state(e);
    if (!s || !path) { KV_ERROR("tilemap", "cannot load '%s': the module is not up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("tilemap", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    Handle h;
    if (const Handle* existing = s->by_path.find(name)) h = *existing;
    else {
        TilemapAsset m{};
        utf8_copy(m.path, sizeof m.path, path);
        Arena* a = (Arena*)alloc_in(s->general, sizeof(Arena) + sizeof(Deps));
        if (!a) return Handle{};
        *a = Arena{};
        memset((void*)(a + 1), 0, sizeof(Deps));
        if (!arena_create(*a, kMapArena, "tilemap", s->general->tag)) { free_in(s->general, a, sizeof(Arena) + sizeof(Deps)); return Handle{}; }
        m.mem = a;
        h = s->maps.create(m);
        if (h.is_null()) { arena_destroy(*a); free_in(s->general, a, sizeof(Arena) + sizeof(Deps)); return Handle{}; }
        s->by_path.set(name, h);
        TilemapAsset* rec = s->maps.get(h);
        rec->blob = asset_load_blob(e, path, Handle{});
        asset_ref(e, rec->blob);
    }
    if (scope.is_null()) return h;
    return asset_scope_add(e, scope, h) ? h : Handle{};
}

const TilemapAsset* tilemap_get(Engine& e, Handle map) {
    TilemapState* s = state(e);
    return s ? s->maps.get(map) : nullptr;
}

// ---- instances ---------------------------------------------------------------------------------

namespace {

// One row: a tile of a tileset at map pixels (left, bottom) of its image's bottom-left,
// with Tiled's flips.
Handle spawn_tile(Table& tiles, const TilemapAsset& m, u32 gid, f32 left, f32 bottom, f32 w, f32 h, point2 origin, i32 layer, color tint, bool ysort) {
    u32 local = 0;
    const MapTileset* ts = tilemap_tileset_of(m, gid, &local);
    if (!ts || local >= (ts->count ? ts->count : 0xFFFFFFFFu)) return Handle{};
    u32 col = local % ts->columns, row = local / ts->columns;
    f32 px = (f32)(ts->margin + col * (ts->tile_w + ts->spacing)), py = (f32)(ts->margin + row * (ts->tile_h + ts->spacing));
    rect2 uv{px / (f32)ts->image_w, py / (f32)ts->image_h, (f32)ts->tile_w / (f32)ts->image_w, (f32)ts->tile_h / (f32)ts->image_h};
    // Tiled's flips as uv flips and a quarter turn: diagonal is a quarter turn
    // counter-clockwise with u flipped unless vertical and v flipped if horizontal.
    bool H = gid & KV_TILE_FLIP_H, V = gid & KV_TILE_FLIP_V, D = gid & KV_TILE_FLIP_D;
    bool fu = D ? !V : H, fv = D ? H : V;
    if (fu) { uv.x += uv.w; uv.w = -uv.w; }
    if (fv) { uv.y += uv.h; uv.h = -uv.h; }
    Sprite sp;
    sp.material = ts->material;
    sp.uv    = uv;
    sp.size  = D ? vec2{h, w} : vec2{w, h};
    sp.tint  = tint;
    sp.layer = layer;
    if (ysort) {
        // Sorted by the feet: the tile's bottom edge, raised by its `foot` property (pixels).
        const MapProperty* foot = tilemap_tile_property(m, gid, "foot");
        sp.sort  = SpriteSort::Feet;
        sp.order = -h * 0.5f + (foot ? (f32)foot->num : 0.0f);
    }
    point2 c = tilemap_to_world(origin, left + w * 0.5f, bottom - h * 0.5f);
    return tiles.spawn(transform2_at(c, D ? 1.57079633f : 0.0f), sp);
}

} // namespace

Handle tilemap_spawn(TickContext& ctx, Handle map, const TilemapPlace& place) {
    Engine& e = *ctx.engine;
    TilemapState* s = state(e);
    const TilemapAsset* m = s ? s->maps.get(map) : nullptr;
    if (!m || m->state != AssetState::Ready) return Handle{};
    Table* tiles = ctx.write_kind(s->tile_kind);
    if (!tiles) return Handle{};
    Array<Handle> rows;
    rows.alloc = s->general;
    for (u32 li = 0; li < m->layer_count; ++li) {
        const MapLayer& l = m->layers[li];
        if (!l.visible) continue;
        color tint = color_rgba(l.tint.r, l.tint.g, l.tint.b, l.tint.a * l.opacity);
        i32 layer = place.base_layer + (i32)li;
        const MapProperty* ys = tilemap_property(*m, l.props, "ysort");
        bool ysort = ys && ys->type == MapPropType::Bool && ys->num != 0;
        if (l.kind == MapLayerKind::Tiles) {
            for (u32 y = 0; y < l.height; ++y)
                for (u32 x = 0; x < l.width; ++x) {
                    u32 gid = l.gids[(usize)y * l.width + x];
                    if (!(gid & KV_TILE_GID_MASK)) continue;
                    const MapTileset* ts = tilemap_tileset_of(*m, gid, nullptr);
                    if (!ts) continue;
                    f32 left = (f32)(x * m->tile_w) + l.offset.x, bottom = (f32)((y + 1) * m->tile_h) + l.offset.y;
                    Handle r = spawn_tile(*tiles, *m, gid, left, bottom, (f32)ts->tile_w, (f32)ts->tile_h, place.origin, layer, tint, ysort);
                    if (!r.is_null()) rows.push(r);
                }
        } else if (l.kind == MapLayerKind::Objects) {
            for (u32 k = 0; k < l.objects.count; ++k) {
                const MapObject& o = m->objects[l.objects.first + k];
                if (!(o.gid & KV_TILE_GID_MASK)) continue;
                Handle r = spawn_tile(*tiles, *m, o.gid, o.x + l.offset.x, o.y + l.offset.y, o.width, o.height, place.origin, layer, tint, ysort);
                if (!r.is_null()) rows.push(r);
            }
        }
    }
    Instance in;
    in.map = map;
    in.owner = place.owner;
    in.row_count = rows.len;
    in.rows = (Handle*)alloc_in(s->general, sizeof(Handle) * (rows.len ? rows.len : 1));
    if (rows.len) memcpy(in.rows, rows.data, sizeof(Handle) * rows.len);
    rows.free();
    Handle h = s->instances.create(in);
    if (h.is_null()) {
        for (u32 k = 0; k < in.row_count; ++k) tiles->destroy(in.rows[k]);
        free_in(s->general, in.rows, sizeof(Handle) * (in.row_count ? in.row_count : 1));
    }
    return h;
}

bool tilemap_despawn(TickContext& ctx, Handle instance) {
    TilemapState* s = state(*ctx.engine);
    Instance* in = s ? s->instances.get(instance) : nullptr;
    if (!in) return false;
    if (Table* tiles = ctx.write_kind(s->tile_kind)) for (u32 k = 0; k < in->row_count; ++k) tiles->destroy(in->rows[k]);
    free_in(s->general, in->rows, sizeof(Handle) * (in->row_count ? in->row_count : 1));
    s->instances.destroy(instance);
    return true;
}

u32 tilemap_instance_rows(Engine& e, Handle instance) {
    TilemapState* s = state(e);
    Instance* in = s ? s->instances.get(instance) : nullptr;
    return in ? in->row_count : 0;
}

} // namespace kv
