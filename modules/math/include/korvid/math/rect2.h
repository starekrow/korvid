// Korvid math: an axis-aligned rectangle, origin plus size. Used for UV rects and views.
#pragma once
#include "korvid/math/vec2.h"

namespace kv {

struct rect2 {
    f32 x = 0.0f, y = 0.0f; // min corner
    f32 w = 0.0f, h = 0.0f;
};

inline rect2  rect2_from_center(point2 c, vec2 size) { return rect2{c.x - size.x * 0.5f, c.y - size.y * 0.5f, size.x, size.y}; }
inline point2 rect_min(const rect2& r)    { return point2{r.x, r.y}; }
inline point2 rect_max(const rect2& r)    { return point2{r.x + r.w, r.y + r.h}; }
inline point2 rect_center(const rect2& r) { return point2{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
inline vec2   rect_size(const rect2& r)   { return vec2{r.w, r.h}; }
inline bool   rect_contains(const rect2& r, point2 p) {
    return p.x >= r.x && p.y >= r.y && p.x <= r.x + r.w && p.y <= r.y + r.h;
}
inline bool   rect_overlaps(const rect2& a, const rect2& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// The full-texture UV rect.
inline rect2 rect2_unit() { return rect2{0.0f, 0.0f, 1.0f, 1.0f}; }

} // namespace kv
