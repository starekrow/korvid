// tilemap_path: A* over a map's cells, for a box that must fit where it stands.
#include "korvid/tilemap/tilemap.h"
#include "korvid/core/arena.h"
#include "korvid/core/memory.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

// Whether a box of `size` (map pixels) centred in cell (x, y) touches no solid.
bool walkable(const TilemapAsset& m, i32 x, i32 y, vec2 size) {
    if (x < 0 || y < 0 || x >= (i32)m.width || y >= (i32)m.height || tilemap_solid(m, x, y)) return false;
    f32 cx = ((f32)x + 0.5f) * (f32)m.tile_w, cy = ((f32)y + 0.5f) * (f32)m.tile_h;
    f32 x0 = cx - size.x * 0.5f, x1 = cx + size.x * 0.5f, y0 = cy - size.y * 0.5f, y1 = cy + size.y * 0.5f;
    // A box wider than a cell reaches into its neighbours: they must be clear too.
    for (i32 ny = (i32)kv::floor(y0 / (f32)m.tile_h); ny <= (i32)kv::floor((y1 - 0.001f) / (f32)m.tile_h); ++ny)
        for (i32 nx = (i32)kv::floor(x0 / (f32)m.tile_w); nx <= (i32)kv::floor((x1 - 0.001f) / (f32)m.tile_w); ++nx)
            if (tilemap_solid(m, nx, ny)) return false;
    for (u32 i = 0; i < m.solid_rect_count; ++i) {
        const rect2& r = m.solid_rects[i];
        if (r.x < x1 && x0 < r.x + r.w && r.y < y1 && y0 < r.y + r.h) return false;
    }
    return true;
}

struct Open { u32 f; u32 cell; };

// A binary min-heap on (f, cell): equal costs come out in cell order, so the path is the
// same in every run.
bool less(const Open& a, const Open& b) { return a.f != b.f ? a.f < b.f : a.cell < b.cell; }
void push(Open* h, u32& n, Open v) {
    u32 i = n++;
    h[i] = v;
    while (i && less(h[i], h[(i - 1) / 2])) { Open t = h[i]; h[i] = h[(i - 1) / 2]; h[(i - 1) / 2] = t; i = (i - 1) / 2; }
}
Open pop(Open* h, u32& n) {
    Open top = h[0];
    h[0] = h[--n];
    u32 i = 0;
    for (;;) {
        u32 l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && less(h[l], h[m])) m = l;
        if (r < n && less(h[r], h[m])) m = r;
        if (m == i) break;
        Open t = h[i]; h[i] = h[m]; h[m] = t;
        i = m;
    }
    return top;
}

} // namespace

u32 tilemap_path(const TilemapAsset& m, point2 origin, point2 from, point2 to, const PathQuery& q, Array<point2>& out) {
    if (m.state != AssetState::Ready || !m.width || !m.height) return 0;
    i32 fx, fy, tx, ty;
    if (!tilemap_cell_of(m, origin, from, &fx, &fy) || !tilemap_cell_of(m, origin, to, &tx, &ty)) return 0;
    if (!walkable(m, tx, ty, q.box)) return 0;
    u32 W = m.width, H = m.height, cells = W * H;
    u32 start = (u32)fy * W + (u32)fx, goal = (u32)ty * W + (u32)tx;
    if (start == goal) { out.push(tilemap_cell_center(m, origin, tx, ty)); return 1; }
    MemScope scope;
    u32* cost = alloc_items<u32>(mem_scratch(), cells);
    u32* from_cell = alloc_items<u32>(mem_scratch(), cells);
    u8*  shut = (u8*)alloc_in(mem_scratch(), cells);
    Open* heap = alloc_items<Open>(mem_scratch(), cells * 8 + 8);
    for (u32 i = 0; i < cells; ++i) { cost[i] = 0xFFFFFFFFu; from_cell[i] = 0xFFFFFFFFu; }
    memset(shut, 0, cells);
    // Octile distance in tenths: 10 straight, 14 diagonal.
    auto h = [&](u32 c) {
        u32 dx = (u32)abs((i32)(c % W) - tx), dy = (u32)abs((i32)(c / W) - ty);
        return q.diagonal ? 10 * (dx > dy ? dx : dy) + 4 * (dx < dy ? dx : dy) : 10 * (dx + dy);
    };
    u32 n = 0, expanded = 0;
    cost[start] = 0;
    push(heap, n, Open{h(start), start});
    static const i32 k_dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, k_dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    while (n) {
        Open o = pop(heap, n);
        if (shut[o.cell]) continue;
        shut[o.cell] = 1;
        if (o.cell == goal) { found = true; break; }
        if (++expanded > q.max_nodes) break;
        i32 cx = (i32)(o.cell % W), cy = (i32)(o.cell / W);
        for (u32 d = 0; d < (q.diagonal ? 8u : 4u); ++d) {
            i32 nx = cx + k_dx[d], ny = cy + k_dy[d];
            if (!walkable(m, nx, ny, q.box)) continue;
            // No corner cutting: a diagonal needs both of its sides open.
            if (d >= 4 && (!walkable(m, cx + k_dx[d], cy, q.box) || !walkable(m, cx, cy + k_dy[d], q.box))) continue;
            u32 nc = (u32)ny * W + (u32)nx;
            if (shut[nc]) continue;
            u32 g = cost[o.cell] + (d < 4 ? 10u : 14u);
            if (g >= cost[nc]) continue;
            cost[nc] = g;
            from_cell[nc] = o.cell;
            if (n < cells * 8) push(heap, n, Open{g + h(nc), nc});
        }
    }
    if (!found) return 0;
    // Back from the goal, then turned around.
    u32 first = out.len;
    for (u32 c = goal; c != start && c != 0xFFFFFFFFu; c = from_cell[c]) out.push(tilemap_cell_center(m, origin, (i32)(c % W), (i32)(c / W)));
    for (u32 i = first, j = out.len - 1; i < j; ++i, --j) { point2 t = out[i]; out[i] = out[j]; out[j] = t; }
    return out.len - first;
}

} // namespace kv
