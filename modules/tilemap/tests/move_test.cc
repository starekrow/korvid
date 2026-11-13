// tilemap_move's fenceposts, on a hand-built 10x8 map of 16 px cells (origin 0, 0; the map
// spans x [0, 160), world y (-128, 0]):
//
//   row 0  . . . . . . . . . .      cells (x, y); '#' solid
//   row 1  . . . . . . . . . .
//   row 2  . . . . # . . . . .      a pillar at (4, 2)
//   row 3  . . . . . . . . . .
//   row 4  . . . . . . . # . .      a wall column at x = 7, rows 4..6
//   row 5  . . . . . . . # . .
//   row 6  # # # # # # # # # #      the floor, row 6 (world y -96 .. -112)
//   row 7  . . . . . . . . . .
//
// plus one solid rectangle: map pixels (32, 32, 4, 8), a post inside cell (2, 2).
#include <stdlib.h>
#include "korvid/tilemap/tilemap.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct TestMap {
    TilemapAsset m;
    u8 solid[80] = {};
    rect2 rects[1];
    TestMap() {
        m.state = AssetState::Ready;
        m.width = 10; m.height = 8; m.tile_w = 16; m.tile_h = 16;
        auto set = [&](u32 x, u32 y) { solid[y * 10 + x] = 1; };
        set(4, 2);
        set(7, 4); set(7, 5);
        for (u32 x = 0; x < 10; ++x) set(x, 6);
        m.solid = solid;
        rects[0] = rect2{32, 32, 4, 8};
        m.solid_rects = rects;
        m.solid_rect_count = 1;
    }
};

// A 12x10 box with its min corner at world (x, y).
rect2 box(f32 x, f32 y) { return rect2{x, y, 12, 10}; }
const point2 kOrigin{0, 0};

} // namespace

void test_tilemap_move(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tilemap move");
    TestMap tm;
    const TilemapAsset& m = tm.m;
    // The floor's top is world y -96: a box resting on it has y == -96.
    const f32 floor_top = -96.0f;

    // Free movement.
    TileMove r = tilemap_move(m, kOrigin, box(10, -50), vec2{3.5f, -2.25f});
    KV_CHECK(c, r.box.x == 13.5f && r.box.y == -52.25f && !r.hit_x && !r.hit_y && !r.started_inside);

    // Falling onto the floor stops exactly on it; resting there is touching, not inside.
    r = tilemap_move(m, kOrigin, box(10, -80), vec2{0, -37.3f});
    KV_CHECK(c, r.hit_y && r.box.y == floor_top && !r.started_inside);
    r = tilemap_move(m, kOrigin, r.box, vec2{0, 0});
    KV_CHECK(c, !r.started_inside && r.box.y == floor_top);
    // Pressing down for a thousand ticks drifts nowhere.
    rect2 b = box(10, floor_top);
    for (u32 i = 0; i < 1000; ++i) b = tilemap_move(m, kOrigin, b, vec2{0, -0.37f}).box;
    KV_CHECK(c, b.y == floor_top && b.x == 10);
    // Lifting off is free.
    r = tilemap_move(m, kOrigin, box(10, floor_top), vec2{0, 2});
    KV_CHECK(c, !r.hit_y && r.box.y == floor_top + 2);

    // Sliding along the floor across many tile seams (up to the wall column standing on it
    // at x 112), both ways, pressed into it or not.
    r = tilemap_move(m, kOrigin, box(1, floor_top), vec2{80, 0});
    KV_CHECK(c, !r.hit_x && r.box.x == 81 && r.box.y == floor_top);
    r = tilemap_move(m, kOrigin, box(95, floor_top), vec2{-84, -1});
    KV_CHECK(c, !r.hit_x && r.hit_y && r.box.x == 11 && r.box.y == floor_top);
    r = tilemap_move(m, kOrigin, box(1, floor_top), vec2{200, 0}); // and into the column's foot
    KV_CHECK(c, r.hit_x && r.box.x == 100 && r.box.y == floor_top);
    for (u32 i = 0; i < 170; ++i) {
        TileMove s = tilemap_move(m, kOrigin, box(1.0f + (f32)i * 0.5f, floor_top), vec2{0.73f, -0.5f});
        if (s.hit_x || s.box.y != floor_top) { KV_CHECK(c, false); break; }
    }

    // Into the wall column (x = 7: world x [112, 128)) from the left: stops on its face,
    // exactly; touching it, pushing in goes nowhere, moving away and along are free.
    r = tilemap_move(m, kOrigin, box(60, -80), vec2{57.9f, 0});
    KV_CHECK(c, r.hit_x && r.box.x == 112 - 12);
    rect2 face = r.box;
    for (u32 i = 0; i < 500; ++i) face = tilemap_move(m, kOrigin, face, vec2{0.61f, 0}).box;
    KV_CHECK(c, face.x == 100);
    r = tilemap_move(m, kOrigin, face, vec2{-1, 0});
    KV_CHECK(c, !r.hit_x && r.box.x == 99);
    r = tilemap_move(m, kOrigin, face, vec2{0, 3}); // up along the wall: the seam between its two cells is nothing
    KV_CHECK(c, !r.hit_x && !r.hit_y && r.box.y == -77);
    r = tilemap_move(m, kOrigin, box(100, -94), vec2{0.5f, 20}); // pressed in, climbing past its top (y -64)
    KV_CHECK(c, r.hit_x && !r.hit_y && r.box.x == 100 && r.box.y == -74);
    // From the right.
    r = tilemap_move(m, kOrigin, box(150, -80), vec2{-40, 0});
    KV_CHECK(c, r.hit_x && r.box.x == 128);

    // No tunnelling: a move far longer than a tile, or than the map, stops at the first solid.
    r = tilemap_move(m, kOrigin, box(10, -80), vec2{5000, 0});
    KV_CHECK(c, r.hit_x && r.box.x == 100);
    r = tilemap_move(m, kOrigin, box(10, -20), vec2{-5000, 0}); // the map's left edge
    KV_CHECK(c, r.hit_x && r.box.x == 0);
    r = tilemap_move(m, kOrigin, box(10, -20), vec2{0, 5000});  // its top
    KV_CHECK(c, r.hit_y && r.box.y == -10);
    r = tilemap_move(m, kOrigin, box(130, -20), vec2{5000, 0}); // its right
    KV_CHECK(c, r.hit_x && r.box.x == 160 - 12);

    // The pillar (4, 2): world x [64, 80), y (-48, -32]. A diagonal into its corner region.
    // Inside corner: the floor and the wall column meet at (112, -96).
    r = tilemap_move(m, kOrigin, box(95, -90), vec2{10, -10});
    KV_CHECK(c, r.hit_x && r.hit_y && r.box.x == 100 && r.box.y == floor_top);
    // Outside corner: approaching the pillar's top-left corner diagonally from up-left. x
    // first: the box is above the pillar (no overlap in y) so x goes all the way, then y
    // meets the pillar's top and stops there.
    r = tilemap_move(m, kOrigin, box(55, -25), vec2{10, -10});
    KV_CHECK(c, !r.hit_x && r.hit_y && r.box.x == 65 && r.box.y == -32);
    // The same corner from the left and level with it: x stops on its face.
    r = tilemap_move(m, kOrigin, box(50, -40), vec2{10, -1});
    KV_CHECK(c, r.hit_x && !r.hit_y && r.box.x == 52 && r.box.y == -41);

    // A solid rectangle smaller than a cell: the post at map (32, 32, 4, 8) is world x
    // [32, 36), y (-40, -32].
    r = tilemap_move(m, kOrigin, box(10, -38), vec2{30, 0});
    KV_CHECK(c, r.hit_x && r.box.x == 20);
    r = tilemap_move(m, kOrigin, box(10, -50), vec2{30, 0}); // under it: free
    KV_CHECK(c, !r.hit_x && r.box.x == 40);

    // Started inside the pillar: not pushed; along and away are allowed, deeper is not.
    r = tilemap_move(m, kOrigin, box(60, -40), vec2{0, 0});
    KV_CHECK(c, r.started_inside);
    r = tilemap_move(m, kOrigin, box(60, -40), vec2{3, 0}); // the pillar is to the right: deeper
    KV_CHECK(c, r.started_inside && r.hit_x && r.box.x == 60);
    r = tilemap_move(m, kOrigin, box(60, -40), vec2{-3, 0}); // away
    KV_CHECK(c, r.started_inside && !r.hit_x && r.box.x == 57);
    r = tilemap_move(m, kOrigin, box(60, -40), vec2{0, 1}); // along
    KV_CHECK(c, !r.hit_y && r.box.y == -39);

    // A map that is not Ready moves freely; a degenerate box too.
    TilemapAsset pending;
    r = tilemap_move(pending, kOrigin, box(0, 0), vec2{1, 1});
    KV_CHECK(c, r.box.x == 1 && r.box.y == 1);
}

void test_tilemap_path(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tilemap path");
    TestMap tm;
    const TilemapAsset& m = tm.m;
    auto center = [&](i32 x, i32 y) { return tilemap_cell_center(m, kOrigin, x, y); };
    auto cell = [&](point2 p, i32* x, i32* y) { return tilemap_cell_of(m, kOrigin, p, x, y); };
    PathQuery q;
    q.box = vec2{8, 8};
    Array<point2> path;
    // Along the open top row: nine steps east, every one a cell of row 0.
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 0), center(9, 0), q, path) == 9);
    bool row0 = true;
    for (point2 p : path) { i32 x, y; row0 &= cell(p, &x, &y) && y == 0; }
    KV_CHECK(c, row0 && path.last() == center(9, 0));
    // Around the wall column (7, 4..5) from its west side to its east: over the top, never
    // through a solid, never across a corner.
    path.clear();
    u32 n = tilemap_path(m, kOrigin, center(6, 5), center(8, 5), q, path);
    KV_CHECK(c, n >= 5 && path.last() == center(8, 5));
    i32 px = 6, py = 5;
    bool clean = true;
    for (point2 p : path) {
        i32 x, y;
        clean &= cell(p, &x, &y) && !tilemap_solid(m, x, y) && abs(x - px) <= 1 && abs(y - py) <= 1;
        if (x != px && y != py) clean &= !tilemap_solid(m, x, py) && !tilemap_solid(m, px, y);
        px = x; py = y;
    }
    KV_CHECK(c, clean);
    // The same question, the same answer.
    Array<point2> again;
    tilemap_path(m, kOrigin, center(6, 5), center(8, 5), q, again);
    KV_CHECK(c, again.len == path.len && !memcmp(again.data, path.data, sizeof(point2) * path.len));
    // Below the floor: no way through. Into a wall: no box fits. Four-connected: longer.
    path.clear();
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 0), center(3, 7), q, path) == 0);
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 0), center(4, 2), q, path) == 0);
    // The post in cell (2, 2) (map px 32..36, 32..40): an 8 px box centred there only
    // touches it (fits); a 10 px one overlaps it (does not).
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 2), center(2, 2), q, path) == 2);
    q.box = vec2{10, 8};
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 2), center(2, 2), q, path) == 0);
    q.box = vec2{8, 8};
    q.diagonal = false;
    path.clear();
    KV_CHECK(c, tilemap_path(m, kOrigin, center(0, 0), center(3, 3), q, path) == 6);
    path.free(); again.free();
}
