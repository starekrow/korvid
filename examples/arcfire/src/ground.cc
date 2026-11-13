// arcfire: the ground. A stage may name a Tiled map (Shoreline's sea and islands,
// tools/make_maps.py), which is spawned once as still tiles on the map's layers and scrolled
// by the renderer's layer offset (planes.cc), so a map of thousands of tiles costs one
// value a tick. Its `enemies` layer places the ground rows: each comes into view at the
// stage distance where its map position crosses the field's top, and from there rides the
// scroll as a ground row does. Turrets stand; tanks drive their road; bunkers launch
// popcorn.
#include "arcfire.h"
#include "korvid/asset/scope.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/tilemap/tilemap.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv::arcfire {

namespace {

constexpr f32 kSpawnAbove = 24.0f;   // a ground row comes into view this far above the field
constexpr f32 kTankSpeed = 0.7f;     // units a tick along its road
constexpr u32 kBunkerFirst = 90; // ticks to its first launch (enemies.cc spaces the rest)

const char* prop_str(const TilemapAsset& m, const MapObject& o, const char* name, const char* dflt) {
    const MapProperty* p = tilemap_property(m, o.props, name);
    return p && p->str && p->str[0] ? p->str : dflt;
}
f64 prop_num(const TilemapAsset& m, const MapObject& o, const char* name, f64 dflt) {
    const MapProperty* p = tilemap_property(m, o.props, name);
    return p ? p->num : dflt;
}

// A bunker's launch: a formation of popcorn on a stage path, relative to the bunker.
bool bunker_formation(StageDef& st, const TilemapAsset& m, const MapObject& o, u32* index, char* why, usize cap) {
    const char* path = prop_str(m, o, "path", "");
    u32 p = kMaxPaths;
    for (u32 i = 0; i < st.path_count; ++i)
        if (!strcmp(st.path_names[i], path)) p = i;
    if (p == kMaxPaths) { format(why, cap, "a bunker's path \"%s\" is not one of the stage's", path); return false; }
    if (st.formation_count == kMaxFormations) { format(why, cap, "too many formations with the bunkers'"); return false; }
    FormationDef f{};
    f.path = p;
    f.count = (u32)prop_num(m, o, "count", 4);
    f.spacing = 12;
    f.look = Look::Red;
    f.health = 1;
    f.score = 50;
    f.fire_at = 0.3f;
    f.speed = 2.6f;
    format(f.pattern, sizeof f.pattern, "%s", prop_str(m, o, "launch", "aimed"));
    f.entry = KV_BML_NONE;
    *index = st.formation_count;
    st.formations[st.formation_count++] = f;
    return true;
}

} // namespace

bool ground_map_ready(TickContext& ctx, Game& g, StageDef& st, char* why, usize why_cap) {
    Engine& e = *ctx.engine;
    if (!st.map[0]) return true;
    if (g.map_parsed) { // one map for the game (Shoreline's), read once
        if (strcmp(st.map, g.stages[0].map) && strcmp(st.map, g.stages[1].map)) { format(why, why_cap, "a second map"); return false; }
        return true;
    }
    if (g.map.is_null()) g.map = tilemap_load(e, st.map, asset_scope_global(e));
    const TilemapAsset* m = tilemap_get(e, g.map);
    if (!m || m->state == AssetState::Pending) return false;
    if (m->state != AssetState::Ready) { format(why, why_cap, "the map %s did not load", st.map); return false; }

    // Where the map is at distance 0: centered on the field, its last row's bottom on the
    // field's bottom, so the map's bottom is the stage's start and its top the stage's end.
    f32 w = (f32)(m->width * m->tile_w) * kMapScale, h = (f32)(m->height * m->tile_h) * kMapScale;
    g.map_origin = point2{-w * 0.5f, kFieldBottom + h};

    // Its enemies, by the distance at which each comes into view.
    st.map_ground_count = 0;
    if (const MapLayer* layer = tilemap_layer_find(*m, "enemies")) {
        for (u32 k = 0; k < layer->objects.count; ++k) {
            const MapObject& o = m->objects[layer->objects.first + k];
            if (st.map_ground_count == kMaxGrounds) { format(why, why_cap, "the map has more than %u enemies", kMaxGrounds); return false; }
            GroundDef gd{};
            gd.kind = !strcmp(o.name, "tank") ? GroundTank : !strcmp(o.name, "bunker") ? GroundBunker : GroundTurret;
            gd.x = g.map_origin.x + o.x * kMapScale;
            gd.at = g.map_origin.y - o.y * kMapScale - (kFieldTop + kSpawnAbove);
            gd.health = (i32)prop_num(*m, o, "health", 12);
            gd.score = (i32)prop_num(*m, o, "score", 500);
            gd.route = (f32)prop_num(*m, o, "route", 0);
            if (!drop_named(prop_str(*m, o, "drop", ""), &gd.drop)) { format(why, why_cap, "the map's %s drops something unknown", o.name); return false; }
            format(gd.pattern, sizeof gd.pattern, "%s", prop_str(*m, o, "pattern", "ring"));
            gd.entry = KV_BML_NONE;
            if (gd.kind == GroundBunker && !bunker_formation(st, *m, o, &gd.launch, why, why_cap)) return false;
            // In order of `at`, so the stage's cursor walks them as the distance grows.
            u32 i = st.map_ground_count++;
            for (; i > 0 && st.map_grounds[i - 1].at > gd.at; --i) st.map_grounds[i] = st.map_grounds[i - 1];
            st.map_grounds[i] = gd;
        }
    }
    g.map_parsed = true;
    KV_INFO("arcfire", "ground: %s, %u x %u tiles, %u enemies", st.map, m->width, m->height, st.map_ground_count);
    return true;
}

void ground_map_show(TickContext& ctx, Game& g, bool show) {
    if (show == !g.map_instance.is_null() || (show && !g.map_parsed)) return;
    if (!show) {
        tilemap_despawn(ctx, g.map_instance);
        g.map_instance = Handle{};
        return;
    }
    TilemapPlace place;
    place.origin = g.map_origin;
    place.base_layer = Z_MAP;
    place.scale = kMapScale;
    g.map_instance = tilemap_spawn(ctx, g.map, place);
}

void ground_spawn(TickContext& ctx, const Game& g, const Dials& d, const GroundDef& gd, point2 at, bool bench) {
    Look look = gd.kind == GroundTank ? Look::Tank : gd.kind == GroundBunker ? Look::Bunker : Look::Turret;
    Health h{bench ? 1 << 30 : (i32)kv::ceil((f32)gd.health * d.enemy_health), 0, gd.score, (u32)look};
    Emitter em{gd.entry, 0.0f, EmitIdle, 0};
    Ground gr{bench ? 0.0f : 1.0f, bench ? 1u : 0u, (u32)gd.kind, gd.launch, gd.kind == GroundBunker ? kBunkerFirst : 0u,
              gd.kind == GroundTank ? kTankSpeed : 0.0f, at.x, gd.route * 0.5f, gd.drop, 0};
    Collider2 col = collider_circle(14.0f, L_GROUND, bench ? 0u : L_SHOT); // nothing hits the bench
    ctx.spawn_later(g_ground_kind, transform2_at(at), enemy_sprite(g, look), col, h, em, gr, Runner{});
}

} // namespace kv::arcfire
