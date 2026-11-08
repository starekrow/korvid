// tilemap_move: a box swept through the solids, one axis at a time, in map pixels (y down).
#include "korvid/tilemap/tilemap.h"
#include <math.h>

namespace kv {

namespace {

struct Box { f32 x0, y0, x1, y1; }; // half-open, map pixels, y down

// Calls fn(rect) for every solid that may touch [x0, x1) x [y0, y1): whole solid cells
// (and the cells outside the map), then the solid rectangles. Returns false if fn did.
template <class F>
void each_solid(const TilemapAsset& m, f32 x0, f32 y0, f32 x1, f32 y1, F fn) {
    f32 tw = (f32)m.tile_w, th = (f32)m.tile_h;
    i32 cx0 = (i32)floorf(x0 / tw) - 1, cx1 = (i32)floorf(x1 / tw) + 1;
    i32 cy0 = (i32)floorf(y0 / th) - 1, cy1 = (i32)floorf(y1 / th) + 1;
    for (i32 cy = cy0; cy <= cy1; ++cy)
        for (i32 cx = cx0; cx <= cx1; ++cx)
            if (tilemap_solid(m, cx, cy)) fn(Box{(f32)cx * tw, (f32)cy * th, (f32)(cx + 1) * tw, (f32)(cy + 1) * th});
    for (u32 i = 0; i < m.solid_rect_count; ++i) {
        const rect2& r = m.solid_rects[i];
        if (r.x < x1 && x0 < r.x + r.w && r.y < y1 && y0 < r.y + r.h) fn(Box{r.x, r.y, r.x + r.w, r.y + r.h});
    }
}

// One axis. `along` picks x (0) or y (1); d is the move on it. Returns the new low edge.
f32 sweep(const TilemapAsset& m, const Box& b, u32 along, f32 d, bool* hit, bool* inside) {
    f32 lo = along ? b.y0 : b.x0, hi = along ? b.y1 : b.x1, size = hi - lo;
    if (d == 0.0f) return lo;
    // The region the box covers now and on its way, padded by the cell walk.
    Box r = b;
    if (along) { if (d > 0) r.y1 += d; else r.y0 += d; } else { if (d > 0) r.x1 += d; else r.x0 += d; }
    f32 best = d;      // how far it may go
    f32 stop = 0.0f;   // the low edge where it stops, exact, when blocked by a solid ahead
    bool blocked = false, deepening = false;
    each_solid(m, r.x0, r.y0, r.x1, r.y1, [&](const Box& s) {
        // Overlap on the other axis, half-open: touching is not overlapping.
        f32 olo = along ? s.x0 : s.y0, ohi = along ? s.x1 : s.y1;
        f32 blo = along ? b.x0 : b.y0, bhi = along ? b.x1 : b.y1;
        if (!(olo < bhi && blo < ohi)) return;
        f32 slo = along ? s.y0 : s.x0, shi = along ? s.y1 : s.x1;
        if (slo < hi && lo < shi) { // overlapping now
            *inside = true;
            if ((d > 0 && lo < slo) || (d < 0 && hi > shi)) deepening = true; // it lies ahead: no further in
            return;
        }
        if (d > 0 && slo >= hi && slo - hi < best) { best = slo - hi; stop = slo - size; blocked = true; }
        if (d < 0 && shi <= lo && shi - lo > best) { best = shi - lo; stop = shi; blocked = true; }
    });
    if (deepening) { *hit = true; return lo; }
    if (blocked) { *hit = true; return stop; }
    return lo + d;
}

} // namespace

TileMove tilemap_move(const TilemapAsset& m, point2 origin, rect2 box, vec2 delta) {
    TileMove out;
    out.box = box;
    if (m.state != AssetState::Ready || box.w <= 0.0f || box.h <= 0.0f) {
        out.box.x += delta.x; out.box.y += delta.y;
        out.moved = delta;
        return out;
    }
    // World (+y up, min corner) to map pixels (y down, top-left).
    Box b{box.x - origin.x, origin.y - (box.y + box.h), 0, 0};
    b.x1 = b.x0 + box.w;
    b.y1 = b.y0 + box.h;
    each_solid(m, b.x0, b.y0, b.x1, b.y1, [&](const Box& s) {
        if (s.x0 < b.x1 && b.x0 < s.x1 && s.y0 < b.y1 && b.y0 < s.y1) out.started_inside = true;
    });
    f32 nx = sweep(m, b, 0, delta.x, &out.hit_x, &out.started_inside);
    b.x0 = nx; b.x1 = nx + box.w;
    f32 ny = sweep(m, b, 1, -delta.y, &out.hit_y, &out.started_inside);
    out.box = rect2{nx + origin.x, origin.y - ny - box.h, box.w, box.h};
    out.moved = vec2{out.box.x - box.x, out.box.y - box.y};
    return out;
}

} // namespace kv
