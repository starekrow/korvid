// Korvid collision: the overlap pairs, by groups. Slice 1 of arcfire found the single
// uniform grid testing every pair in neighbouring cells before looking at masks: a ring
// of bullets fired from one point sat in one cell and was 1.65M pairs in a tick, every one
// thrown out by mask. Grouping by (layer, mask) first means a pair that cannot meet is never
// a candidate, and each group's grid is sized to its own members, so one large collider no
// longer makes every cell large.
#include "collision_internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/map.h"
#include "korvid/core/sort.h"
#include <math.h>

namespace kv {

namespace {

u64 grid_key(i32 cx, i32 cy) { return ((u64)(u32)cx << 32) | (u32)cy; }

struct Group {
    u32         layer, mask;
    f32         max_r;
    f32         cell;
    Array<u32>  members;  // item indices, ascending
    Map<u64, u32> heads;  // cell -> first item in it (the rest by `next`)
};

int order_pairs(const void* x, const void* y) {
    const CollPair& a = *(const CollPair*)x;
    const CollPair& b = *(const CollPair*)y;
    if (a.a != b.a) return a.a < b.a ? -1 : 1;
    return a.b < b.b ? -1 : a.b > b.b ? 1 : 0;
}

} // namespace

bool collision_items_overlap(const CollItem& a, const CollItem& b) {
    vec2 d = b.p - a.p;
    bool ca = a.shape == (u8)ColliderShape::Circle, cb = b.shape == (u8)ColliderShape::Circle;
    if (ca && cb) { f32 r = a.r + b.r; return length_sq(d) < r * r; }
    if (!ca && !cb) return fabsf(d.x) < a.half.x + b.half.x && fabsf(d.y) < a.half.y + b.half.y;
    // Circle against box: the box's nearest point to the circle's center.
    const CollItem& box = ca ? b : a;
    const CollItem& cir = ca ? a : b;
    vec2 off = cir.p - box.p;
    f32 cx = off.x < -box.half.x ? -box.half.x : off.x > box.half.x ? box.half.x : off.x;
    f32 cy = off.y < -box.half.y ? -box.half.y : off.y > box.half.y ? box.half.y : off.y;
    vec2 gap{off.x - cx, off.y - cy};
    return length_sq(gap) < cir.r * cir.r;
}

void collision_find_pairs(Span<const CollItem> items, Array<CollPair>& out, CollPairStats* stats) {
    out.clear();
    CollPairStats st;
    // The groups and grids in scratch, under the caller's scope: a scope here would release
    // `out` too, were it in scratch and grown here.
    Allocator* scratch = mem_scratch();

    // Groups by signature, in the order their first member comes: a handful in any game.
    Array<Group> groups = array_in<Group>(scratch);
    Array<u32> group_of = array_in<u32>(scratch);
    group_of.resize_fill(items.len, 0);
    for (u32 i = 0; i < items.len; ++i) {
        const CollItem& it = items[i];
        u32 g = 0;
        while (g < groups.len && !(groups[g].layer == it.layer && groups[g].mask == it.mask)) ++g;
        if (g == groups.len) {
            Group ng{};
            ng.layer = it.layer;
            ng.mask = it.mask;
            ng.members = array_in<u32>(scratch);
            ng.heads.init(scratch);
            groups.push(ng);
        }
        Group& grp = groups[g];
        grp.members.push(i);
        if (it.r > grp.max_r) grp.max_r = it.r;
        group_of[i] = g;
    }
    st.groups = groups.len;

    // Each group's grid: a cell its largest member's diameter, members filed by their center,
    // so two members that overlap sit in neighbouring cells.
    Array<u32> next = array_in<u32>(scratch);
    next.resize_fill(items.len, 0xFFFFFFFFu);
    for (Group& grp : groups) {
        grp.cell = 2.0f * (grp.max_r > 0.5f ? grp.max_r : 0.5f);
        for (u32 i : grp.members) {
            u64 key = grid_key((i32)floorf(items[i].p.x / grp.cell), (i32)floorf(items[i].p.y / grp.cell));
            u32* head = grp.heads.get_or_insert(key, 0xFFFFFFFFu);
            next[i] = *head;
            *head = i;
        }
    }

    auto consider = [&](u32 i, u32 j) {
        ++st.tested;
        if (!collision_items_overlap(items[i], items[j])) return;
        CollPair p{items[i].h.bits, items[j].h.bits};
        if (p.a > p.b) { u64 t = p.a; p.a = p.b; p.b = t; }
        out.push(p);
    };

    for (u32 ga = 0; ga < groups.len; ++ga) {
        for (u32 gb = ga; gb < groups.len; ++gb) {
            Group& A = groups[ga];
            Group& B = groups[gb];
            if (!collision_meets(A.layer, A.mask, B.layer, B.mask)) continue;
            if (ga == gb) {
                // Within a group that meets itself: neighbouring cells, each pair once.
                for (u32 i : A.members) {
                    i32 cx = (i32)floorf(items[i].p.x / A.cell), cy = (i32)floorf(items[i].p.y / A.cell);
                    for (i32 dy = -1; dy <= 1; ++dy)
                        for (i32 dx = -1; dx <= 1; ++dx) {
                            u32* head = A.heads.find(grid_key(cx + dx, cy + dy));
                            for (u32 j = head ? *head : 0xFFFFFFFFu; j != 0xFFFFFFFFu; j = next[j])
                                if (j > i) consider(i, j);
                        }
                }
                continue;
            }
            // Across two groups: the smaller walked over the larger's grid, each of its items
            // looking at the cells its reach covers (or at every member, when that is fewer).
            Group& S = A.members.len <= B.members.len ? A : B;
            Group& L = &S == &A ? B : A;
            for (u32 i : S.members) {
                const CollItem& a = items[i];
                f32 reach = a.r + L.max_r;
                i32 x0 = (i32)floorf((a.p.x - reach) / L.cell), x1 = (i32)floorf((a.p.x + reach) / L.cell);
                i32 y0 = (i32)floorf((a.p.y - reach) / L.cell), y1 = (i32)floorf((a.p.y + reach) / L.cell);
                u64 cells = (u64)(x1 - x0 + 1) * (u64)(y1 - y0 + 1);
                if (cells > L.members.len) {
                    for (u32 j : L.members) consider(i, j);
                    continue;
                }
                for (i32 cy = y0; cy <= y1; ++cy)
                    for (i32 cx = x0; cx <= x1; ++cx) {
                        u32* head = L.heads.find(grid_key(cx, cy));
                        for (u32 j = head ? *head : 0xFFFFFFFFu; j != 0xFFFFFFFFu; j = next[j]) consider(i, j);
                    }
            }
        }
    }
    if (out.len > 1) kv::sort(out.data, out.len, sizeof(CollPair), order_pairs);
    if (stats) *stats = st;
}

} // namespace kv
