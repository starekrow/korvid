// example_arcfire: the parallax planes and the ground's scroll. Planes of seamless
// textures (each a grid of still tiles on its own layer: space's three, the sky's three,
// a stage showing the ones it names) and the ground map (three layers of still tiles) all
// scroll by the renderer's layer offset (sprite/layers.h), each at its own rate: one
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

// A plane: a seamless texture tiled over the field, its tile `full` units across (at
// plane density 1), shifted by `offset`, scrolled at `rate` times the speed on a layer of
// its own. Space's three, back to front: deep stars, the high nebula (tinted down), a faint
// cloud over the ground. The sky's three (Shoreline's): the low clouds' shadows (the same
// texture darkened, down and to the right, at the same rate, so each shadow keeps under its
// cloud), the low clouds a little faster than the ground, the high wisps faster still.
struct PlaneDesc { Tex tex; f32 rate; i32 layer; color tint; f32 full; vec2 offset; };
constinit const PlaneDesc k_planes[kPlaneCount] = {
    {TEX_STARS,       0.15f, Z_DEEP,         color{1.0f, 1.0f, 1.0f, 1.0f},     1024.0f, vec2{0, 0}},
    {TEX_NEBULA,      0.40f, Z_HIGH,         color{0.75f, 0.7f, 0.85f, 0.55f},  1024.0f, vec2{0, 0}},
    {TEX_CLOUD,       1.60f, Z_CLOUD,        color{0.8f, 0.85f, 1.0f, 0.22f},   1024.0f, vec2{0, 0}},
    {TEX_CLOUDS_LOW,  1.35f, Z_CLOUD_SHADOW, color{0.0f, 0.02f, 0.08f, 0.20f},  1024.0f, vec2{36, -60}},
    {TEX_CLOUDS_LOW,  1.35f, Z_LOW_CLOUD,    color{1.0f, 1.0f, 1.0f, 0.85f},    1024.0f, vec2{0, 0}},
    {TEX_CLOUDS_HIGH, 2.10f, Z_HIGH_CLOUD,   color{1.0f, 1.0f, 1.0f, 0.75f},    1024.0f, vec2{0, 0}},
};
constexpr f32 kGroundRate = 1.0f;  // the ground moves at the stage's speed

f32 plane_tile(const Dials& d, u32 p) { return k_planes[p].full / (f32)(d.plane_density ? d.plane_density : 1); }

} // namespace

void planes_spawn(Engine& e, Game& g, const Dials& d) {
    Table* planes = engine_table(e, g_plane_kind);
    u32 made = 0, shown = 0;
    for (u32 p = 0; p < KV_COUNTOF(k_planes); ++p) {
        if (!(g.stage.planes & (1u << p))) continue;
        ++shown;
        f32 tile = plane_tile(d, p);
        // Enough to cover the field and its offset, and a row more for the scroll.
        u32 cols = (u32)ceilf((kFieldW + 2.0f * fabsf(k_planes[p].offset.x)) / tile) + 1;
        u32 rows = (u32)ceilf(kFieldH / tile) + 1;
        for (u32 r = 0; r < rows; ++r)
            for (u32 c = 0; c < cols; ++c) {
                point2 at{((f32)c - (f32)(cols - 1) * 0.5f) * tile, kFieldBottom + tile * 0.5f + (f32)r * tile};
                at += k_planes[p].offset;
                Sprite s = sprite_of(g, k_planes[p].tex, rect2_unit(), vec2{tile, tile}, k_planes[p].layer, k_planes[p].tint);
                if (!planes->spawn(transform2_at(at), s, Plane{k_planes[p].rate, tile, r, rows}).is_null()) ++made;
            }
    }
    KV_INFO("arcfire", "planes: %u of them, %u tiles", shown, made);
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
    f32 ground = -map_clock(st) * kGroundRate;

    if (d.scroll_rows) {
        // Slice 1's way, for measuring: every tile written every tick.
        Table* planes_table = ctx.write_kind(g_plane_kind);
        Span<Transform2> t = planes_table->rows_of<Transform2>();
        Span<const Plane> p = planes_table->rows_of<Plane>();
        for (u32 i = 0; i < planes_table->rows(); ++i) {
            if (!planes_table->is_alive(i)) continue;
            t[i].position.y -= st.speed * p[i].rate;
            if (t[i].position.y < kFieldBottom - p[i].tile * 0.5f) {
                t[i].position.y += p[i].tile * (f32)p[i].rows;
                *planes_table->prev<Transform2>(planes_table->handle_at(i)) = t[i];
            }
        }
        // The map's tiles are a still kind and cannot be moved: it scrolls by layer either way.
        for (u32 l = 0; l < kMapLayers; ++l) layers->set(Z_MAP + (i32)l, vec2{0.0f, ground}, ground > g.ground_offset);
        g.ground_offset = ground;
        return;
    }

    for (u32 p = 0; p < KV_COUNTOF(k_planes); ++p) {
        f32 tile = plane_tile(d, p);
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
