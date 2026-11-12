// example_arcfire: Catmull-Rom paths by arc length.
#include "path.h"
#include <math.h>

namespace kv::arcfire {

point2 path_segment_point(const Path& p, u32 i, f32 t) {
    if (p.count == 0) return point2{0, 0};
    if (p.count == 1) return p.pts[0];
    u32 last = p.count - 1;
    point2 p1 = p.pts[i];
    point2 p2 = p.pts[i + 1 <= last ? i + 1 : last];
    point2 p0 = i > 0 ? p.pts[i - 1] : p1;
    point2 p3 = i + 2 <= last ? p.pts[i + 2] : p2;
    f32 t2 = t * t, t3 = t2 * t;
    f32 a = -0.5f * t3 + t2 - 0.5f * t;
    f32 b = 1.5f * t3 - 2.5f * t2 + 1.0f;
    f32 c = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    f32 d = 0.5f * t3 - 0.5f * t2;
    return point2{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

void path_build(Path& p, const point2* pts, u32 count) {
    p.count = count > kPathPoints ? kPathPoints : count;
    for (u32 i = 0; i < p.count; ++i) p.pts[i] = pts[i];
    p.length = 0.0f;
    for (u32 i = 0; i + 1 < p.count; ++i) {
        f32 len = 0.0f;
        point2 at = p.pts[i];
        for (u32 k = 1; k <= kPathSamples; ++k) {
            point2 next = path_segment_point(p, i, (f32)k / (f32)kPathSamples);
            len += distance(at, next);
            at = next;
        }
        p.seg[i] = len;
        p.length += len;
    }
}

// Within a segment the parameter is found by walking the same samples the length was
// measured with, then interpolating inside the one that holds `s`: exact at the samples,
// a fraction of a unit off between them.
point2 path_at(const Path& p, f32 s) {
    if (p.count < 2) return p.count ? p.pts[0] : point2{0, 0};
    if (s <= 0.0f) return p.pts[0];
    for (u32 i = 0; i + 1 < p.count; ++i) {
        if (s > p.seg[i]) { s -= p.seg[i]; continue; }
        point2 at = p.pts[i];
        for (u32 k = 1; k <= kPathSamples; ++k) {
            point2 next = path_segment_point(p, i, (f32)k / (f32)kPathSamples);
            f32 step = distance(at, next);
            if (s <= step) return step > 0.0f ? lerp(at, next, s / step) : next;
            s -= step;
            at = next;
        }
        return at;
    }
    return p.pts[p.count - 1];
}

} // namespace kv::arcfire
