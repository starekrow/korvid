// example_arcfire: the parallax planes and the ground's scroll. Three planes of seamless
// space backgrounds (each a grid of still tiles on its own layer) and the ground map (three
// layers of still tiles) all scroll by the renderer's layer offset (sprite/layers.h): one
// value a layer a tick, interpolated between ticks, never a Set delta. A plane's offset
// wraps by a whole tile (the tiles repeat, so the jump is invisible and sent as a snap); the
// ground's is the stage's distance, so the map moves exactly as the ground rows do, and it
// snaps back when the distance does (a loop, a restart).
//
// The dial `scroll_rows` moves every plane tile instead, the way slice 1 did, for the
// notebook's comparison: a Set delta per tile per tick. (The map's tiles are a still kind,
// which the extract does not look at once sent: they scroll by layer either way.)
#include "arcfire.h"
#include "korvid/core/log.h"
#include "korvid/sprite/layers.h"
#include <math.h>

namespace kv::arcfire {

namespace {

struct PlaneDesc { Tex tex; f32 rate; i32 layer; color tint; };
// Back to front: deep stars, the high nebula (tinted down), the low cloud over the
// ground and under the air (faint).
constinit const PlaneDesc k_planes[] = {
    {TEX_STARS,  0.15f, Z_DEEP,  color{1.0f, 1.0f, 1.0f, 1.0f}},
    {TEX_NEBULA, 0.40f, Z_HIGH,  color{0.75f, 0.7f, 0.85f, 0.55f}},
    {TEX_CLOUD,  1.60f, Z_CLOUD, color{0.8f, 0.85f, 1.0f, 0.22f}},
};
constexpr f32 kTileFull = 1024.0f; // a 512 px background at 2x
constexpr f32 kGroundRate = 1.0f;  // the ground moves at the stage's speed

f32 plane_tile(const Dials& d) { return kTileFull / (f32)(d.plane_density ? d.plane_density : 1); }

} // namespace

void planes_spawn(Engine& e, Game& g, const Dials& d) {
    Table* planes = engine_table(e, g_plane_kind);
    f32 tile = plane_tile(d);
    u32 cols = (u32)ceilf(kFieldW / tile);
    u32 rows = (u32)ceilf(kFieldH / tile) + 1;
    u32 made = 0;
    for (u32 p = 0; p < KV_COUNTOF(k_planes); ++p) {
        if (!(g.stage.planes & (1u << p))) continue;
        for (u32 r = 0; r < rows; ++r)
            for (u32 c = 0; c < cols; ++c) {
                point2 at{((f32)c - (f32)(cols - 1) * 0.5f) * tile, kFieldBottom + tile * 0.5f + (f32)r * tile};
                Sprite s = sprite_of(g, k_planes[p].tex, rect2_unit(), vec2{tile, tile}, k_planes[p].layer, k_planes[p].tint);
                if (!planes->spawn(transform2_at(at), s, Plane{k_planes[p].rate, r, c, rows}).is_null()) ++made;
            }
    }
    KV_INFO("arcfire", "planes: %u tiles of %.0f units, %u x %u a plane", made, (f64)tile, cols, rows);
}

// Phase Sim: the layers' offsets, from the scroll's speed (the planes) and the stage's
// distance (the ground).
void planes(TickContext& ctx) {
    static const u32 k_layers = ctx.resource_id("sprite.layers");
    Game& g = ctx.state_as<Game>();
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    if (!stage_row.len) return;
    const Stage& st = stage_row[0];
    const Dials& d = dials_row[0];
    SpriteLayers* layers = ctx.resource<SpriteLayers>(k_layers);
    f32 tile = plane_tile(d);
    f32 ground = -(st.distance - (f32)st.loops * g.stage.length) * kGroundRate;

    if (d.scroll_rows) {
        // Slice 1's way, for measuring: every tile written every tick.
        Table* planes_table = ctx.write_kind(g_plane_kind);
        Span<Transform2> t = planes_table->rows_of<Transform2>();
        Span<const Plane> p = planes_table->rows_of<Plane>();
        for (u32 i = 0; i < planes_table->rows(); ++i) {
            if (!planes_table->is_alive(i)) continue;
            t[i].position.y -= st.speed * p[i].rate;
            if (t[i].position.y < kFieldBottom - tile * 0.5f) {
                t[i].position.y += tile * (f32)p[i].rows;
                *planes_table->prev<Transform2>(planes_table->handle_at(i)) = t[i];
            }
        }
        // The map's tiles are a still kind and cannot be moved: it scrolls by layer either way.
        for (u32 l = 0; l < kMapLayers; ++l) layers->set(Z_MAP + (i32)l, vec2{0.0f, ground}, ground > g.ground_offset);
        g.ground_offset = ground;
        return;
    }

    for (u32 p = 0; p < KV_COUNTOF(k_planes); ++p) {
        f32 before = fmodf(g.plane_scrolled[p], tile);
        g.plane_scrolled[p] += st.speed * k_planes[p].rate;
        f32 now = fmodf(g.plane_scrolled[p], tile);
        layers->set(k_planes[p].layer, vec2{0.0f, -now}, now < before); // wrapped: the same picture, no sweep
    }
    bool jumped = ground > g.ground_offset; // the distance went back: a loop or a restart
    for (u32 l = 0; l < kMapLayers; ++l) layers->set(Z_MAP + (i32)l, vec2{0.0f, ground}, jumped);
    g.ground_offset = ground;
}

} // namespace kv::arcfire
