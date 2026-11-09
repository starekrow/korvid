// example_arcfire: the parallax planes. Three planes under and over the field, each a
// grid of tiles of one seamless texture moving down at its own rate times the scroll's
// speed; a tile that has left the bottom moves up by the grid's height (a teleport, so the
// renderer does not slide it across the field). This is the game-side version the PRD
// asks to be measured: every tile is a row, so every tile is a Set delta every tick.
// The stress dial shrinks the tiles (more of them) to make that cost visible.
#include "arcfire.h"
#include "korvid/core/log.h"
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

} // namespace

void planes_spawn(Engine& e, Game& g, const Dials& d) {
    Table* planes = engine_table(e, g_plane_kind);
    u32 density = d.plane_density ? d.plane_density : 1;
    f32 tile = kTileFull / (f32)density;
    u32 cols = (u32)ceilf(kFieldW / tile);
    u32 rows = (u32)ceilf(kFieldH / tile) + 1;
    u32 made = 0;
    for (const PlaneDesc& p : k_planes) {
        for (u32 r = 0; r < rows; ++r)
            for (u32 c = 0; c < cols; ++c) {
                point2 at{((f32)c - (f32)(cols - 1) * 0.5f) * tile, kFieldBottom + tile * 0.5f + (f32)r * tile};
                Sprite s = sprite_of(g, p.tex, rect2_unit(), vec2{tile, tile}, p.layer, p.tint);
                if (!planes->spawn(transform2_at(at), s, Plane{p.rate, r, c, rows}).is_null()) ++made;
            }
    }
    KV_INFO("arcfire", "planes: %u tiles of %.0f units, %u x %u a plane", made, (f64)tile, cols, rows);
}

// Phase Sim: every tile down by its plane's share of the scroll; wrapped as a teleport.
void planes(TickContext& ctx) {
    Span<const Stage> stage_row = ctx.read<Stage>(ARC_WORLD(Stage));
    if (!stage_row.len) return;
    f32 speed = stage_row[0].speed;
    for (KindRows k : ctx.view<Transform2, const Plane>()) {
        Span<Transform2> t = k.col<Transform2>();
        Span<Transform2> prev = k.prev<Transform2>();
        Span<const Plane> p = k.col<const Plane>();
        Span<const Sprite> s = k.col<const Sprite>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            f32 tile = s[i].size.y;
            t[i].position.y -= speed * p[i].rate;
            if (t[i].position.y < kFieldBottom - tile * 0.5f) {
                t[i].position.y += tile * (f32)p[i].rows;
                if (prev.len) prev[i] = t[i];
            }
        }
    }
}

} // namespace kv::arcfire
