// Korvid math: 2D vectors and affine points. point - point = vec, point + vec = point,
// and point + point does not compile. Plain structs, free functions, header-only.
#pragma once
#include "korvid/core/types.h"
#include <math.h>

namespace kv {

struct vec2 {
    f32 x = 0.0f, y = 0.0f;
};

struct point2 {
    f32 x = 0.0f, y = 0.0f;
};

// ---- vec2 ------------------------------------------------------------------
inline vec2 operator+(vec2 a, vec2 b) { return vec2{a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a, vec2 b) { return vec2{a.x - b.x, a.y - b.y}; }
inline vec2 operator-(vec2 a)         { return vec2{-a.x, -a.y}; }
inline vec2 operator*(vec2 a, f32 s)  { return vec2{a.x * s, a.y * s}; }
inline vec2 operator*(f32 s, vec2 a)  { return vec2{a.x * s, a.y * s}; }
inline vec2 operator*(vec2 a, vec2 b) { return vec2{a.x * b.x, a.y * b.y}; } // component-wise
inline vec2 operator/(vec2 a, f32 s)  { return vec2{a.x / s, a.y / s}; }
inline vec2& operator+=(vec2& a, vec2 b) { a.x += b.x; a.y += b.y; return a; }
inline vec2& operator-=(vec2& a, vec2 b) { a.x -= b.x; a.y -= b.y; return a; }
inline vec2& operator*=(vec2& a, f32 s)  { a.x *= s; a.y *= s; return a; }
inline bool operator==(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(vec2 a, vec2 b) { return !(a == b); }

inline f32  dot(vec2 a, vec2 b)      { return a.x * b.x + a.y * b.y; }
inline f32  cross(vec2 a, vec2 b)    { return a.x * b.y - a.y * b.x; } // z of the 3D cross
inline f32  length_sq(vec2 a)        { return dot(a, a); }
inline f32  length(vec2 a)           { return sqrtf(length_sq(a)); }
inline vec2 perp(vec2 a)             { return vec2{-a.y, a.x}; }       // rotated +90 degrees
inline vec2 normalize(vec2 a) {
    f32 l = length(a);
    return l > 0.0f ? a / l : vec2{};
}
inline vec2 lerp(vec2 a, vec2 b, f32 t) { return a + (b - a) * t; }

// ---- point2 ----------------------------------------------------------------
inline point2 operator+(point2 p, vec2 v) { return point2{p.x + v.x, p.y + v.y}; }
inline point2 operator+(vec2 v, point2 p) { return point2{p.x + v.x, p.y + v.y}; }
inline point2 operator-(point2 p, vec2 v) { return point2{p.x - v.x, p.y - v.y}; }
inline vec2   operator-(point2 a, point2 b) { return vec2{a.x - b.x, a.y - b.y}; }
inline point2& operator+=(point2& p, vec2 v) { p.x += v.x; p.y += v.y; return p; }
inline point2& operator-=(point2& p, vec2 v) { p.x -= v.x; p.y -= v.y; return p; }
inline bool operator==(point2 a, point2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(point2 a, point2 b) { return !(a == b); }

inline f32    distance(point2 a, point2 b)    { return length(a - b); }
inline point2 lerp(point2 a, point2 b, f32 t) { return a + (b - a) * t; }

// Explicit conversions only; the types are different on purpose.
inline vec2   as_vec(point2 p)   { return vec2{p.x, p.y}; }
inline point2 as_point(vec2 v)   { return point2{v.x, v.y}; }

// ---- scalars ---------------------------------------------------------------
constexpr f32 KV_PI = 3.14159265358979323846f;

inline f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline bool nearly(f32 a, f32 b, f32 eps = 1e-5f) { return fabsf(a - b) <= eps; }
inline bool nearly(vec2 a, vec2 b, f32 eps = 1e-5f) { return nearly(a.x, b.x, eps) && nearly(a.y, b.y, eps); }
inline bool nearly(point2 a, point2 b, f32 eps = 1e-5f) { return nearly(a.x, b.x, eps) && nearly(a.y, b.y, eps); }

} // namespace kv
