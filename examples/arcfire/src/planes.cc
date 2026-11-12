// example_arcfire: the parallax planes and the ground's scroll. Planes of seamless
// textures (each a grid of still tiles on its own layer: space's three, the sky's three,
// a stage showing the ones it names) and the ground map (three layers of still tiles) all
// scroll by the renderer's layer offset (sprite/layers.h), each at its own rate: one
// value a layer a tick, interpolated between ticks, never a Set delta. A plane's offset
// wraps by its texture's period (the picture repeats, so the jump is invisible and sent as
// a snap); the
// ground's is the stage's distance, so the map moves exactly as the ground rows do, and it
// snaps back when the distance does (a loop, a restart).
//
// The dial `scroll_rows` moves every plane tile instead, the way slice 1 did, for the
// notebook's comparison: a Set delta per tile per tick. (The map's tiles are a still kind,
// which the extract does not look at once sent: they scroll by layer either way.)
#include "arcfire.h"
#include "korvid/asset/material.h"
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
constexpr f32 kGasRate = 0.1f;     // the gas, slower than the stars

// The dial `plane_density` cuts each plane's picture into more tiles (n x n to a repeat of
// its texture), for measuring what a plane of many tiles costs; the picture keeps its
// scale: each tile shows its cell of the texture.
u32 plane_cuts(const Dials& d) { return d.plane_density ? d.plane_density : 1; }
f32 plane_tile(const Dials& d, u32 p) { return k_planes[p].full / (f32)plane_cuts(d); }

} // namespace

void planes_spawn(Engine& e, Game& g, const Dials& d) {
    Table* planes = engine_table(e, g_plane_kind);
    planes->destroy_all(); // the last stage's, its segments with them
    u32 made = 0, shown = 0;
    for (u32 p = 0; p < KV_COUNTOF(k_planes); ++p) {
        if (!(g.stage.planes & (1u << p))) continue;
        ++shown;
        const PlaneDesc& pd = k_planes[p];
        u32 n = plane_cuts(d);
        f32 tile = plane_tile(d, p);
        // On one grid whatever the cut (the texture's origin at x = -full/2, the field's
        // bottom), so the picture is the same at any density: the columns that cover the
        // field and its offset, and up the field and a whole repeat more, which the scroll
        // wraps by (whole repeats, so a tile moved up by all of them shows the same).
        f32 origin = -0.5f * pd.full;
        i32 first = (i32)floorf((-0.5f * kFieldW - fabsf(pd.offset.x) - origin) / tile);
        i32 last = (i32)ceilf((0.5f * kFieldW + fabsf(pd.offset.x) - origin) / tile);
        u32 rows = n * (u32)ceilf((kFieldH + pd.full) / pd.full);
        for (u32 r = 0; r < rows; ++r)
            for (i32 k = first; k <= last; ++k) {
                point2 at{origin + ((f32)k + 0.5f) * tile, kFieldBottom + ((f32)r + 0.5f) * tile};
                at += pd.offset;
                // Its cell of the texture: up the world is up the picture (uv rows run down).
                u32 col = (u32)(((k % (i32)n) + (i32)n) % (i32)n);
                rect2 uv{(f32)col / (f32)n, (f32)(n - 1 - r % n) / (f32)n, 1.0f / (f32)n, 1.0f / (f32)n};
                // Over the gas the stars are added (their black adds nothing), so the gas shows
                // through them; elsewhere the deep plane is the back of the picture.
                bool over_gas = p == 0 && (g.stage.planes & kPlaneShader);
                Sprite s = sprite_of(g, pd.tex, uv, vec2{tile, tile}, pd.layer, pd.tint, over_gas);
                if (!planes->spawn(transform2_at(at), s, Plane{k_planes[p].rate, tile, r, rows}).is_null()) ++made;
            }
    }
    // The gas: one sprite over the whole field under the stars, drawn by the nebula shader
    // (shaders/arcfire_nebula.slang), on a layer of its own that never scrolls: its uv does.
    g.nebula_row = Handle{};
    if (g.stage.planes & kPlaneShader) {
        if (const MaterialAsset* m = asset_material(e, g.nebula)) {
            Sprite s;
            s.material = m->material;
            s.uv = rect2{0.0f, 0.0f, 1.0f, kFieldH / kFieldW};
            s.size = vec2{kFieldW + 8.0f, kFieldH + 8.0f};
            s.layer = Z_GAS;
            g.nebula_row = planes->spawn(transform2_at(point2{0, 0}), s, Plane{0.0f, 0.0f, 0, 0});
            ++shown;
        }
    }
    KV_INFO("arcfire", "planes: %u of them, %u tiles", shown, made);
}

// Phase Sim, every tick: the layers' offsets, from the scroll's speed (the planes) and the
// stage's distance (the ground). Paused, nothing moves; but on the title, which holds the
// sim, the sky goes by at the stage's speed over the map's first screen (cosmetic: the
// offsets are the renderer's, never the sim's).
void planes(TickContext& ctx) {
    static const u32 k_layers = ctx.resource_id("sprite.layers");
    Game& g = ctx.state_as<Game>();
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    Span<const Dials> dials_row = ctx.read<Dials>(ARC_WORLD(Dials));
    SpriteLayers* layers = ctx.resource<SpriteLayers>(k_layers);
    bool title = g.mode == Mode::Title;
    if (!title && (ctx.paused || !stage_row.len)) return;
    f32 speed = title ? g.stage.speed : stage_row[0].speed;
    f32 ground = title ? 0.0f : -map_clock(stage_row[0]) * kGroundRate;

    if (!title && dials_row[0].scroll_rows) {
        const Stage& st = stage_row[0];
        // Slice 1's way, for measuring: every tile written every tick.
        Table* planes_table = ctx.write_kind(g_plane_kind);
        Span<Transform2> t = planes_table->rows_of<Transform2>();
        Span<const Plane> p = planes_table->rows_of<Plane>();
        for (u32 i = 0; i < planes_table->rows(); ++i) {
            if (!planes_table->is_alive(i) || p[i].rate < 0.0f) continue; // a segment rides the ground's layer
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
        f32 repeat = k_planes[p].full; // the texture's period, whatever the tiles' size
        f32 before = fmodf(g.plane_scrolled[p], repeat);
        g.plane_scrolled[p] += speed * k_planes[p].rate;
        f32 now = fmodf(g.plane_scrolled[p], repeat);
        layers->set(k_planes[p].layer, vec2{0.0f, -now}, now < before); // wrapped: the same picture, no sweep
    }
    // The gas comes down with the stage at the deep plane's rate: its uv moves, the sprite does not.
    if (Sprite* gas = ctx.write_kind(g_plane_kind)->get<Sprite>(g.nebula_row)) {
        g.gas_scrolled += speed * kGasRate;
        gas->uv.y = -g.gas_scrolled / kFieldW;
    }
    bool jumped = ground > g.ground_offset; // the distance went back: a loop or a restart
    for (u32 l = 0; l < kMapLayers; ++l) layers->set(Z_MAP + (i32)l, vec2{0.0f, ground}, jumped);
    g.ground_offset = ground;
    // A station segment (rate -1, on the ground's layer) is placed by the distance; when it
    // goes back, the segments shown go (their turrets went with the restart), and so does
    // one that has scrolled past the field's bottom.
    Table* pt = ctx.write_kind(g_plane_kind);
    Span<const Plane> pl = pt->rows_of<Plane>();
    Span<const Transform2> px = pt->rows_of<Transform2>();
    for (u32 i = 0; i < pt->rows(); ++i)
        if (pt->is_alive(i) && pl[i].rate < 0.0f && (jumped || px[i].position.y + ground < kFieldBottom - 120.0f)) pt->destroy(pt->handle_at(i));
}

} // namespace kv::arcfire
