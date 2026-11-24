// The pipeline without a renderer (no images, no materials; everything else the same):
// a map and its external tileset read through the mounts into a scope; Ready waits for
// both; an instance's rows; an instance goes when its owner dies and when its map retires.
#include "korvid/tilemap/module.h"
#include "korvid/core/format.h"
#include "korvid/tilemap/tilemap.h"
#include "korvid/asset/module.h"
#include "korvid/asset/scope.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/sprite/module.h"
#include "korvid/anim/module.h"
#include "korvid/collision/module.h"
#include "korvid/scene/module.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"
#include "fixtures.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

struct Want { Handle map; Handle owner; Handle instance; bool spawn = false, despawn = false; f32 scale = 1.0f; };
Want g_want;

void sys_spawn(TickContext& ctx) {
    if (g_want.spawn) {
        TilemapPlace place;
        place.origin = point2{0, 0};
        place.base_layer = 10;
        place.owner = g_want.owner;
        place.scale = g_want.scale;
        g_want.instance = tilemap_spawn(ctx, g_want.map, place);
        g_want.spawn = false;
    }
    if (g_want.despawn) { tilemap_despawn(ctx, g_want.instance); g_want.despawn = false; }
}

constinit const KTableDesc k_ktables[] = {
    {.name = ".owner", .components = "Transform2", .capacity = 4},
    {.name = ".child", .components = "Transform2, Local2, Parent", .capacity = 1}, // what the scene and anim modules need to see
    {.name = ".body", .components = "Transform2, Collider2, Move2", .capacity = 1},
};
constinit const SystemDesc k_systems[] = {{.name = ".spawn", .phase = Phase::Sim, .fn = sys_spawn, .writes = "tilemap.tile, tilemap.animtile", .main_thread = true}};
KV_MODULE_DEFINE(k_tt) = {.name = "tt", .ktables = k_ktables, .systems = k_systems};

bool put(const char* path, const char* text) {
    return io_write_file(path, text, strlen(text));
}

} // namespace

void test_tilemap_load(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tilemap load");
    const ModuleDesc* const list[] = {&platform_module, &asset_module, &sprite_module, &scene_module, &collision_module, &anim_module, &tilemap_module, &k_tt};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    char root[512], path[600];
    // The fixtures go in a scratch directory mounted over the defaults, not beside the
    // executable: an iOS app's bundle is signed and read-only.
    char scratch[400];
    KV_CHECK(c, io_make_temp_dir("korvid-tilemap-test", scratch, sizeof scratch) && platform_io(e)->mount_dir(scratch));
    format(root, sizeof root, "%s/tilemap_test", scratch);
    format(path, sizeof path, "%s/maps", root); io_make_dirs(path);
    format(path, sizeof path, "%s/tiles", root); io_make_dirs(path);
    format(path, sizeof path, "%s/maps/town.tmj", root);
    KV_CHECK(c, put(path, k_map));
    format(path, sizeof path, "%s/tiles/town.tsj", root);
    KV_CHECK(c, put(path, k_tileset));
    auto frames = [&](u32 n) { for (u32 i = 0; i < n; ++i) { platform_pump(e); engine_frame(e, 1.0 / 60.0); } };

    Handle scope = asset_scope_create(e, "town");
    Handle map = tilemap_load(e, "tilemap_test/maps/town.tmj", scope);
    KV_CHECK(c, !map.is_null() && tilemap_load(e, "tilemap_test/maps/town.tmj", scope) == map && asset_refs(e, map) == 1);
    KV_CHECK(c, asset_scope_progress(e, scope).pending == 1);
    // Frames until a load is in, a millisecond apart, for up to two seconds: a read takes
    // real time, not a number of frames (io_uring punts a read from tmpfs to a worker thread).
    auto settle = [&](Handle h) { for (u32 i = 0; i < 2000 && tilemap_get(e, h)->state == AssetState::Pending; ++i) { frames(1); time_sleep(0.001); } };
    settle(map);
    const TilemapAsset* m = tilemap_get(e, map);
    KV_CHECK(c, m->state == AssetState::Ready && m->tileset_count == 2 && strcmp(m->tilesets[0].image, "tilemap_test/tiles/town.png") == 0);
    KV_CHECK(c, m->tilesets[0].tiles.count == 3 && m->solid_rect_count == 3 && tilemap_solid(*m, 0, 1));
    frames(1);
    KV_CHECK(c, asset_scope_progress(e, scope).pending == 0 && asset_scope_progress(e, scope).ready == 1);

    // The map as the collision module's static world: a body walking west into the solid
    // cell (0, 1), world x [0, 16), y [-32, -16), stops on its edge.
    tilemap_collide(e, map, point2{0, 0});
    KTable* bodies = engine_ktable(e, "tt.body");
    Handle body = bodies->spawn(transform2_at(point2{40, -24}), collider_body(BodyKind::Kinematic, vec2{4, 4}, vec2{0, 0}, 1, 1), Move2{vec2{-60, 0}});
    frames(1);
    KV_CHECK(c, bodies->get<Transform2>(body)->position.x == 20.0f);
    tilemap_collide(e, Handle{}, point2{0, 0});
    bodies->get<Move2>(body)->delta = vec2{-60, 0};
    frames(1);
    KV_CHECK(c, bodies->get<Transform2>(body)->position.x == -40.0f);
    bodies->destroy(body);

    // An instance: one row per non-empty cell of the visible tile layers (ground 12, detail
    // 2, walls 3; gz is hidden) and one per tile object (the tree).
    KTable* owners = engine_ktable(e, "tt.owner");
    g_want.map = map;
    g_want.owner = owners->spawn(transform2_at(point2{0, 0}));
    g_want.spawn = true;
    frames(1);
    KV_CHECK(c, !g_want.instance.is_null() && tilemap_instance_rows(e, g_want.instance) == 18);
    KTable* tiles = engine_ktable(e, "tilemap.tile");
    KTable* anims = engine_ktable(e, "tilemap.animtile");
    u32 animated = anims->live();
    KV_CHECK(c, tiles && animated > 0 && tiles->live() + animated == 18);
    // The animated tile (local 10, frames 10 and 11 at 250 ms) plays its map's clip.
    u32 tile_gid = m->tilesets[0].first_gid + 10;
    char want[64];
    format(want, sizeof want, "tilemap_test/maps/town.tmj#tile%u", tile_gid);
    Handle clip = anim_find(e, want);
    const AnimClip* ac = anim_clip(e, clip);
    KV_CHECK(c, ac && ac->state == AssetState::Ready && ac->duration == 0.5f && ac->keys.len == 2 && ac->loop == AnimLoop::Loop);
    for (u32 i = 0; i < anims->rows(); ++i)
        if (anims->is_alive(i)) KV_CHECK(c, anims->rows_of<AnimPlayer>()[i].clip == clip);
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
    KV_CHECK(c, tiles->live() == 0 && anims->live() == 0 && tilemap_instance_rows(e, g_want.instance) == 0);

    // A second instance, then the scope goes: the map retires, its instance with it.
    g_want.owner = Handle{};
    g_want.spawn = true;
    frames(1);
    KV_CHECK(c, tiles->live() + anims->live() == 18);
    // At scale 2 (a 16 px tileset drawn as 32-unit tiles): the same cell, twice as far in
    // and twice the size.
    g_want.despawn = true;
    frames(1);
    g_want.scale = 2.0f;
    g_want.spawn = true;
    frames(1);
    bool scaled = false;
    for (u32 i = 0; i < tiles->rows(); ++i) {
        if (!tiles->is_alive(i)) continue;
        const Transform2& t = tiles->rows_of<Transform2>()[i];
        const Sprite& s = tiles->rows_of<Sprite>()[i];
        if (s.layer == 10 && t.position.x == 16 && t.position.y == -16) { scaled = s.size.x == 32 && s.size.y == 32; break; }
    }
    KV_CHECK(c, scaled && tiles->live() + anims->live() == 18);
    g_want.scale = 1.0f;
    asset_scope_release(e, scope);
    frames(2);
    KV_CHECK(c, tilemap_get(e, map) == nullptr && tiles->live() == 0 && anims->live() == 0);
    KV_CHECK(c, anim_clip(e, clip) == nullptr); // the map held its tiles' clips
    KV_CHECK(c, asset_lifetime_stats(e).dying == 0);

    // A map that does not parse fails with its reason.
    format(path, sizeof path, "%s/maps/bad.tmj", root);
    KV_CHECK(c, put(path, "{\"width\": 1,"));
    Handle bad = tilemap_load(e, "tilemap_test/maps/bad.tmj", Handle{});
    asset_ref(e, bad);
    frames(1);
    settle(bad);
    KV_CHECK(c, tilemap_get(e, bad)->state == AssetState::Failed && strstr(tilemap_get(e, bad)->error_text, "JSON"));
    asset_unref(e, bad);
    frames(1);
    KV_CHECK(c, tilemap_get(e, bad) == nullptr);

    engine_shutdown(e);
    io_remove_tree(scratch);
}
