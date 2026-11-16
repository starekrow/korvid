// patherizer: strokes widened into outlines (stroke.cc).
#pragma once
#include "korvid/patherizer/patherizer.h"

namespace kv {

// Outlines to fill nonzero: points (x, y pairs) and where each outline starts in them.
struct StrokeOutlines {
    Array<f32> points;
    Array<u32> starts; // in points' floats
};

// A polyline's stroke (n points, x, y pairs), closed or open, dashed by `dashes` (the
// stroke's dash_count of them). `tol` is the flattening tolerance in the polyline's units, for
// round joins and caps.
void stroke_polyline(StrokeOutlines& out, const f32* pts, u32 n, bool closed, const PathStroke& style, const f32* dashes, f32 tol);

} // namespace kv
