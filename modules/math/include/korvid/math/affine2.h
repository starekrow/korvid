// Korvid math: 2x3 affine transform (rotation, scale, translation), column-major:
//   [ a c tx ]   maps p -> (a*x + c*y + tx, b*x + d*y + ty)
//   [ b d ty ]
// Points get the translation, vectors do not. Also the 2D ortho projection, which is affine.
#pragma once
#include "korvid/math/rect2.h"

namespace kv {

struct affine2 {
    f32 a = 1.0f, b = 0.0f; // first column  (x axis)
    f32 c = 0.0f, d = 1.0f; // second column (y axis)
    f32 tx = 0.0f, ty = 0.0f;
};

inline affine2 affine2_identity() { return affine2{}; }
inline affine2 affine2_translate(vec2 t) { return affine2{1, 0, 0, 1, t.x, t.y}; }
inline affine2 affine2_scale(vec2 s)     { return affine2{s.x, 0, 0, s.y, 0, 0}; }
inline affine2 affine2_rotate(f32 radians) {
    f32 cs = cosf(radians), sn = sinf(radians);
    return affine2{cs, sn, -sn, cs, 0, 0};
}

// Translate * Rotate * Scale: scale first, then rotate, then move. The sprite transform.
inline affine2 affine2_trs(point2 position, f32 radians, vec2 scale) {
    f32 cs = cosf(radians), sn = sinf(radians);
    return affine2{cs * scale.x, sn * scale.x, -sn * scale.y, cs * scale.y, position.x, position.y};
}

inline point2 transform_point(const affine2& m, point2 p) {
    return point2{m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty};
}
inline vec2 transform_vec(const affine2& m, vec2 v) {
    return vec2{m.a * v.x + m.c * v.y, m.b * v.x + m.d * v.y};
}
inline point2 operator*(const affine2& m, point2 p) { return transform_point(m, p); }
inline vec2   operator*(const affine2& m, vec2 v)   { return transform_vec(m, v); }

// (m * n) applies n first, then m, like the matrices it stands for.
inline affine2 operator*(const affine2& m, const affine2& n) {
    affine2 r;
    r.a  = m.a * n.a + m.c * n.b;
    r.b  = m.b * n.a + m.d * n.b;
    r.c  = m.a * n.c + m.c * n.d;
    r.d  = m.b * n.c + m.d * n.d;
    r.tx = m.a * n.tx + m.c * n.ty + m.tx;
    r.ty = m.b * n.tx + m.d * n.ty + m.ty;
    return r;
}

inline f32 determinant(const affine2& m) { return m.a * m.d - m.b * m.c; }

// Inverse of a non-singular transform. A singular input returns the identity.
inline affine2 inverse(const affine2& m) {
    f32 det = determinant(m);
    if (det == 0.0f) return affine2{};
    f32 id = 1.0f / det;
    affine2 r;
    r.a =  m.d * id;
    r.b = -m.b * id;
    r.c = -m.c * id;
    r.d =  m.a * id;
    r.tx = -(r.a * m.tx + r.c * m.ty);
    r.ty = -(r.b * m.tx + r.d * m.ty);
    return r;
}

// Component-wise. Correct for translation and scale; rotation shrinks through the lerp,
// so interpolate (position, rotation, scale) and rebuild with affine2_trs where it matters.
inline affine2 lerp(const affine2& m, const affine2& n, f32 t) {
    return affine2{lerp(m.a, n.a, t), lerp(m.b, n.b, t), lerp(m.c, n.c, t), lerp(m.d, n.d, t),
                   lerp(m.tx, n.tx, t), lerp(m.ty, n.ty, t)};
}

inline bool nearly(const affine2& m, const affine2& n, f32 eps = 1e-5f) {
    return nearly(m.a, n.a, eps) && nearly(m.b, n.b, eps) && nearly(m.c, n.c, eps) &&
           nearly(m.d, n.d, eps) && nearly(m.tx, n.tx, eps) && nearly(m.ty, n.ty, eps);
}

// Orthographic projection of a world rect onto Vulkan clip space: x in [-1, 1] left to
// right, y in [-1, 1] top to bottom (Vulkan's y points down), so world +y is up on screen.
inline affine2 ortho2d(const rect2& view) {
    f32 sx = 2.0f / view.w;
    f32 sy = -2.0f / view.h;
    return affine2{sx, 0, 0, sy, -1.0f - view.x * sx, 1.0f - view.y * sy};
}

} // namespace kv
