// Korvid render: the sprite delta stream, the one thing the outer engine hands the
// renderer about sprites. The renderer keeps a persistent world of sprites keyed by
// entity handle; each tick the outer engine sends what changed. A Tick marker starts
// each tick's deltas so the renderer knows where "previous" ends, whatever the
// tick-to-frame ratio. No full copy per frame.
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

inline bool operator==(const Placement2& a, const Placement2& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}
inline bool operator!=(const Placement2& a, const Placement2& b) { return !(a == b); }

// Everything the renderer keeps per sprite.
struct RenderSpriteState {
    Placement2 placement;
    Handle     material;  // renderer-issued; null or stale -> the default material
    rect2      uv;        // texture rect; outside [0, 1] repeats
    vec2       size;      // quad size in world units, before the placement's scale
    color      tint;      // multiplies the material's tint
    i32        layer;     // drawn in ascending order; the renderer clamps to [-128, 127]
};

enum class SpriteDeltaKind : u8 {
    Tick,   // a sim tick begins: current becomes previous for every sprite
    Set,    // create or update the sprite for `entity`
    Remove, // the entity is gone
};

struct SpriteDelta {
    SpriteDeltaKind   kind;
    Handle            entity;
    RenderSpriteState state; // for Set
};

} // namespace kv
