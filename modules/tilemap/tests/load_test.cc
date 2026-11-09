// The pipeline without a renderer (no images, no materials; everything else the same):
// a map and its external tileset read through the mounts into a scope; Ready waits for
// both; an instance's rows; an instance goes when its owner dies and when its map retires.
#include "korvid/tilemap/module.h"
#include "korvid/tilemap/tilemap.h"
#include "korvid/asset/module.h"
#include "korvid/asset/scope.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Want { Handle map; Handle owner; Handle instance; bool spawn = false, despawn = false; };
Want g_want;

void sys_spawn(TickContext& ctx) {
    if (g_want.spawn) {
        TilemapPlace place;
        place.origin = point2{0, 0};
        place.base_layer = 10;
        place.owner = g_want.owner;
        g_want.instance = tilemap_spawn(ctx, g_want.map, place);
        g_want.spawn = false;
    }
    if (g_want.despawn) { tilemap_despawn(ctx, g_want.instance); g_want.despawn = false; }
}

constinit const KindDesc k_kinds[] = {{.name = ".owner", .components = "Transform2", .capacity = 4}};
constinit const SystemDesc k_systems[] = {{.name = ".spawn", .phase = Phase::Sim, .fn = sys_spawn, .writes = "tilemap.tile", .main_thread = true}};
KV_MODULE_DEFINE(k_tt) = {.name = "tt", .kinds = k_kinds, .systems = k_systems};

bool put(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

} // namespace

void test_tilemap_load(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tilemap load");
    const ModuleDesc* const list[] = {&platform_module, &asset_module, &sprite_module, &tilemap_module, &k_tt};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    char root[512], path[600];
    snprintf(root, sizeof root, "%sassets/tilemap_test", platform_base_path(e));
    snprintf(path, sizeof path, "%s/maps", root); io_make_dirs(path);
    snprintf(path, sizeof path, "%s/tiles", root); io_make_dirs(path);
    snprintf(path, sizeof path, "%s/maps/town.tmj", root);
    KV_CHECK(c, put(path, k_map));
    snprintf(path, sizeof path, "%s/tiles/town.tsj", root);
    KV_CHECK(c, put(path, k_tileset));
    auto frames = [&](u32 n) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); } };

    Handle scope = asset_scope_create(e, "town");
    Handle map = tilemap_load(e, "tilemap_test/maps/town.tmj", scope);
    KV_CHECK(c, !map.is_null() && tilemap_load(e, "tilemap_test/maps/town.tmj", scope) == map && asset_refs(e, map) == 1);
    KV_CHECK(c, asset_scope_progress(e, scope).pending == 1);
    for (u32 i = 0; i < 50 && tilemap_get(e, map)->state == AssetState::Pending; ++i) frames(1);
    const TilemapAsset* m = tilemap_get(e, map);
    KV_CHECK(c, m->state == AssetState::Ready && m->tileset_count == 2 && strcmp(m->tilesets[0].image, "tilemap_test/tiles/town.png") == 0);
    KV_CHECK(c, m->tilesets[0].tiles.count == 3 && m->solid_rect_count == 3 && tilemap_solid(*m, 0, 1));
    frames(1);
    KV_CHECK(c, asset_scope_progress(e, scope).pending == 0 && asset_scope_progress(e, scope).ready == 1);

    // An instance: one row per non-empty cell of the visible tile layers (ground 12, detail
    // 2, walls 3; gz is hidden) and one per tile object (the tree).
    Table* owners = engine_table(e, "tt.owner");
    g_want.map = map;
    g_want.owner = owners->spawn(transform2_at(point2{0, 0}));
    g_want.spawn = true;
    frames(1);
    KV_CHECK(c, !g_want.instance.is_null() && tilemap_instance_rows(e, g_want.instance) == 18);
    Table* tiles = engine_table(e, "tilemap.tile");
    KV_CHECK(c, tiles && tiles->live() == 18);
    // Layer order and placement: the ground's first cell at layer 10, its center 8 px in.
    bool found = false;
    for (u32 i = 0; i < tiles->rows(); ++i) {
        if (!tiles->is_alive(i)) continue;
        const Transform2& t = tiles->rows_of<Transform2>()[i];
        const Sprite& s = tiles->rows_of<Sprite>()[i];
        if (s.layer == 10 && t.position.x == 8 && t.position.y == -8) { found = s.uv.w > 0 && s.size.x == 16; break; }
    }
    KV_CHECK(c, found);

    // The owner dies: the rows go with it at the next pump.
    owners->destroy(g_want.owner);
    frames(1);
    KV_CHECK(c, tiles->live() == 0 && tilemap_instance_rows(e, g_want.instance) == 0);

    // A second instance, then the scope goes: the map retires, its instance with it.
    g_want.owner = Handle{};
    g_want.spawn = true;
    frames(1);
    KV_CHECK(c, tiles->live() == 18);
    asset_scope_release(e, scope);
    frames(2);
    KV_CHECK(c, tilemap_get(e, map) == nullptr && tiles->live() == 0);
    KV_CHECK(c, asset_lifetime_stats(e).dying == 0);

    // A map that does not parse fails with its reason.
    snprintf(path, sizeof path, "%s/maps/bad.tmj", root);
    KV_CHECK(c, put(path, "{\"width\": 1,"));
    Handle bad = tilemap_load(e, "tilemap_test/maps/bad.tmj", Handle{});
    asset_ref(e, bad);
    frames(4);
    KV_CHECK(c, tilemap_get(e, bad)->state == AssetState::Failed && strstr(tilemap_get(e, bad)->error_text, "JSON"));
    asset_unref(e, bad);
    frames(1);
    KV_CHECK(c, tilemap_get(e, bad) == nullptr);

    engine_shutdown(e);
    snprintf(path, sizeof path, "%s/maps/town.tmj", root); io_remove(path);
    snprintf(path, sizeof path, "%s/maps/bad.tmj", root); io_remove(path);
    snprintf(path, sizeof path, "%s/tiles/town.tsj", root); io_remove(path);
    snprintf(path, sizeof path, "%s/maps", root); io_remove(path);
    snprintf(path, sizeof path, "%s/tiles", root); io_remove(path);
    io_remove(root);
}
