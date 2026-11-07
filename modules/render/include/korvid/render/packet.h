// Korvid render: the render packet. The one type shared by the outer engine (which writes
// it in the extract) and the inner engine (which reads it). Nothing else crosses.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/math.h"

namespace kv {

// A 2D placement as the sim stores it: interpolated per field, then built into an affine2.
struct Placement2 {
    point2 position;
    f32    rotation = 0.0f; // radians, counter-clockwise
    vec2   scale{1.0f, 1.0f};
};

inline Placement2 lerp(const Placement2& a, const Placement2& b, f32 t) {
    Placement2 r;
    r.position = lerp(a.position, b.position, t);
    r.rotation = lerp(a.rotation, b.rotation, t);
    r.scale    = lerp(a.scale, b.scale, t);
    return r;
}

struct RenderPacket {
    Handle     entity;
    Placement2 prev;      // previous tick
    Placement2 cur;       // current tick
    Handle     material;  // renderer-issued; null or stale -> the default material
    rect2      uv;        // texture rect; outside [0, 1] repeats
    vec2       size;      // quad size in world units, before the placement's scale
    color      tint;      // multiplies the material's tint
    i32        layer;     // drawn in ascending order
};

} // namespace kv
