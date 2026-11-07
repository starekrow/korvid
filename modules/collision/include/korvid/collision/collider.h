// Korvid collision: the Collider2 component and the overlap events.
//
// Give an entity kind a Collider2 next to its Transform2 and it takes part in the
// broadphase: every tick, the "collision.broadphase" system finds every pair of
// colliders that overlap and tells you what changed since the last tick, as events.
//
//   OverlapBegin{a, b}  the pair started overlapping this tick
//   OverlapEnd{a, b}    the pair stopped overlapping this tick
//
// That is the whole contract. There is no collision response, no contact point, no
// normal and no penetration depth: the game decides what an overlap means, in a handler
// on the event. A pair that stays overlapped is reported once, at its beginning.
//
// Layers and masks. Each collider is on a set of layers (bits in `layer`) and accepts
// a set of layers (bits in `mask`). Two colliders are tested only when each is on a
// layer the other accepts: (a.layer & b.mask) && (b.layer & a.mask). A bullet on layer 2
// with mask 4 and a rock on layer 4 with mask 2|1 meet; two bullets never do.
//
// Shapes. A circle of `radius` around the transform's position, or an axis-aligned box
// of `half` extents around it. The transform's rotation and scale are ignored by both:
// a rotating box sprite keeps an upright collision box.
//
// Pairs are canonical, the handle with the lower bits first, so a pair never appears in
// both orders. The handles in an event were live when it was delivered; an OverlapEnd
// for an entity destroyed in the meantime is dropped by the bus and never arrives.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/vec2.h"

namespace kv {

enum class ColliderShape : u8 {
    Circle, // `radius` around the position
    Box,    // `half` extents around the position, axis aligned
};

// The component. Plain data; set it at spawn or edit the column later, the broadphase
// reads it every tick. Mask and layer edits take effect on the next tick.
struct Collider2 {
    f32  radius = 0.0f;        // Circle: radius in world units (pixels, in the examples)
    vec2 half;                 // Box: half width and half height; the box is 2*half across
    u32  layer  = 1;           // which layers this collider is on; one bit per layer, any number of them
    u32  mask   = 0xFFFFFFFFu; // which layers it collides with; the default is every layer
    u8   shape  = (u8)ColliderShape::Circle;
};

// A circle on `layer` that collides with `mask`.
inline Collider2 collider_circle(f32 radius, u32 layer, u32 mask) {
    Collider2 c;
    c.radius = radius; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Circle;
    return c;
}

// An axis-aligned box of `half` extents on `layer` that collides with `mask`.
inline Collider2 collider_box(vec2 half, u32 layer, u32 mask) {
    Collider2 c;
    c.half = half; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Box;
    return c;
}

// Emitted by "collision.broadphase" in phase Sim, so handlers run at the next phase
// boundary of the same tick (PostSim's), and PostSim systems see them too. a and b are
// the entities' handles, a having the lower bits; use a handle's kind() to tell which
// kind each is, or resolve it against the table you expect.
struct OverlapBegin { Handle a, b; };
struct OverlapEnd   { Handle a, b; };

} // namespace kv
