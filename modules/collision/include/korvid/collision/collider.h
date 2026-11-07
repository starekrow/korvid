// Korvid collision: the Collider2 component and the overlap events. Any kind that has
// Transform2 and Collider2 takes part in the broadphase. Two colliders are tested when
// each is on a layer the other's mask accepts. Overlaps are a set of pairs the engine
// diffs tick to tick: OverlapBegin when a pair appears, OverlapEnd when it is gone. A
// pair is canonical, the lower handle first. There is no response and no contact data;
// the game decides what an overlap means.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/vec2.h"

namespace kv {

enum class ColliderShape : u8 { Circle, Box };

struct Collider2 {
    f32  radius = 0.0f;       // Circle
    vec2 half;                // Box: half extents, axis aligned (rotation is ignored)
    u32  layer  = 1;          // bits this collider is on
    u32  mask   = 0xFFFFFFFFu; // bits it collides with
    u8   shape  = (u8)ColliderShape::Circle;
};

inline Collider2 collider_circle(f32 radius, u32 layer, u32 mask) {
    Collider2 c;
    c.radius = radius; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Circle;
    return c;
}
inline Collider2 collider_box(vec2 half, u32 layer, u32 mask) {
    Collider2 c;
    c.half = half; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Box;
    return c;
}

struct OverlapBegin { Handle a, b; }; // a < b
struct OverlapEnd   { Handle a, b; };

} // namespace kv
