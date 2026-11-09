// example_arcfire: paths. A formation flies a path of up to eight points as a Catmull-Rom
// spline through them, at a speed along its length: the spline is measured once, segment
// by segment, so `path_at(p, s)` is where a mover is after `s` units of travel. Members of
// one formation start on the same path at different ticks and keep their spacing because
// they move by distance, not by parameter. Plain code, no engine: the tests take it whole.
#pragma once
#include "korvid/math/vec2.h"

namespace kv::arcfire {

constexpr u32 kPathPoints = 8;
constexpr u32 kPathSamples = 16; // per segment, for the length

struct Path {
    point2 pts[kPathPoints];
    f32    seg[kPathPoints - 1]; // each segment's length
    f32    length = 0.0f;        // the sum
    u32    count  = 0;
};

// Measures the spline through pts[0..count): count of 2 is a line, 1 is a point.
void path_build(Path& p, const point2* pts, u32 count);
// The point after `s` units along the path, clamped to its ends.
point2 path_at(const Path& p, f32 s);
// The spline's point at parameter t in [0, 1] of segment i (Catmull-Rom, the ends doubled).
point2 path_segment_point(const Path& p, u32 i, f32 t);

} // namespace kv::arcfire
