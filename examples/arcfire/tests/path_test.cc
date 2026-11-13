// Paths by arc length: a straight path is its own length and walks evenly; a curve's
// length is measured, its ends are its first and last points, and two movers a fixed
// distance apart on it stay that far apart along it whatever the tick.
#include "../src/path.cc"
#include "korvid/core/test.h"
#include "korvid/math/scalar.h"

void test_path(kv::TestCtx& c) {
    using namespace kv;
    using namespace kv::arcfire;
    c.begin("path");

    Path line;
    const point2 two[] = {{0, 0}, {300, 400}};
    path_build(line, two, 2);
    KV_CHECK(c, kv::fabs(line.length - 500.0f) < 0.01f);
    KV_CHECK(c, nearly(path_at(line, 250.0f), point2{150, 200}, 0.05f));
    KV_CHECK(c, path_at(line, -5.0f) == two[0] && path_at(line, 900.0f) == two[1]);

    Path curve;
    const point2 pts[] = {{-200, 440}, {-180, 200}, {-60, 40}, {120, -80}, {330, -120}};
    path_build(curve, pts, 5);
    f32 chord = 0.0f;
    for (u32 i = 0; i + 1 < 5; ++i) chord += distance(pts[i], pts[i + 1]);
    KV_CHECK(c, curve.length >= chord && curve.length < chord * 1.2f); // a spline through the points is no shorter than the polyline
    KV_CHECK(c, nearly(path_at(curve, 0.0f), pts[0]) && nearly(path_at(curve, curve.length), pts[4], 0.01f));
    // It passes through every point, at the sum of the segments before it.
    f32 s = 0.0f;
    for (u32 i = 0; i < 4; ++i) {
        KV_CHECK(c, nearly(path_at(curve, s), pts[i], 0.05f));
        s += curve.seg[i];
    }
    // Spacing by distance: one unit of travel moves the point about one unit, everywhere.
    f32 worst = 0.0f;
    for (f32 at = 0.0f; at + 1.0f < curve.length; at += 7.0f) {
        f32 step = distance(path_at(curve, at), path_at(curve, at + 1.0f));
        worst = kv::fabs(step - 1.0f) > worst ? kv::fabs(step - 1.0f) : worst;
    }
    KV_CHECK(c, worst < 0.02f);
    // More points than a path holds: the first eight.
    point2 many[10];
    for (u32 i = 0; i < 10; ++i) many[i] = point2{(f32)i * 10.0f, 0.0f};
    Path capped;
    path_build(capped, many, 10);
    KV_CHECK(c, capped.count == kPathPoints && kv::fabs(capped.length - 70.0f) < 0.01f);
}
