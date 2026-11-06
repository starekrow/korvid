// Korvid math: linear RGBA color, float per channel. Tints multiply.
#pragma once
#include "korvid/math/vec2.h"

namespace kv {

struct color {
    f32 r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

inline color operator*(color a, color b) { return color{a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a}; }
inline color operator*(color a, f32 s)   { return color{a.r * s, a.g * s, a.b * s, a.a * s}; }
inline bool  operator==(color a, color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
inline bool  operator!=(color a, color b) { return !(a == b); }
inline color lerp(color a, color b, f32 t) {
    return color{lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t)};
}

// 8-bit sRGB-ish constants are not provided on purpose: everything here is linear.
inline color color_white()   { return color{1, 1, 1, 1}; }
inline color color_black()   { return color{0, 0, 0, 1}; }
inline color color_red()     { return color{1, 0, 0, 1}; }
inline color color_green()   { return color{0, 1, 0, 1}; }
inline color color_blue()    { return color{0, 0, 1, 1}; }
inline color color_clear()   { return color{0, 0, 0, 0}; }
inline color color_rgba(f32 r, f32 g, f32 b, f32 a = 1.0f) { return color{r, g, b, a}; }

} // namespace kv
