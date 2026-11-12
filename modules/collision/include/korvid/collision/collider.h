// Korvid collision: the Collider2 component, bodies that move by intent, and the overlap
// events.
//
// Give an entity kind a Collider2 next to its Transform2 and it takes part. What it does
// depends on its body:
//
//   Sensor     (the default) reports overlaps as events and blocks nothing:
//                OverlapBegin{a, b}  the pair started overlapping this tick
//                OverlapEnd{a, b}    the pair stopped overlapping this tick
//   Solid      blocks kinematic bodies; never moves on its own; is pushed one deep by a
//              body with ColliderPusher when it has ColliderPushable
//   Kinematic  moves by its intent (Move2), blocked by solids, other kinematic bodies and
//              the static world (a tile map; see collision_static), and blocks other
//              kinematic bodies
//
// A body that may move (kinematic, or a pushable solid) needs Move2 in its kind; a kind
// that reads where it went adds MoveResult2. The game writes Move2::delta in Sim; the
// system "collision.resolve" (Sim, after every writer of Move2) moves every intent in a
// fixed order, kinds by name and then slots, each one swept along x and then y against
// everything as it stands at that moment, so the second mover sees where the first one
// ended this tick. Then it writes Transform2 (and Local2 by the same delta for a child
// kind, which is right for a child of an unrotated, unscaled parent, a level's root) and
// MoveResult2, before any system that reads them: intent, resolve, react, in one tick.
// "collision.clear" (PostSim) zeroes the intents, so an intent lasts one tick.
//
// Layers and masks decide who meets whom, overlaps and blocks alike. Each collider is on a
// set of layers (bits in `layer`) and accepts a set of layers (bits in `mask`); two meet
// only when each is on a layer the other accepts: (a.layer & b.mask) && (b.layer & a.mask).
//
// Shapes. A circle of `radius`, or an axis-aligned box of `half` extents, around the
// transform's position plus `offset` (a character's feet). Rotation and scale are ignored.
// Blocking is box against box: a circle blocks and is blocked as its bounding box.
// Boxes are half-open: touching is not overlapping, and a stopped box ends exactly on the
// edge it met.
//
// Overlap pairs are canonical, the handle with the lower bits first, found after the
// resolve on where everything ended, so an overlap arrives the tick of the move that made
// it. An OverlapEnd for an entity destroyed meanwhile is dropped by the bus.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/rect2.h"
#include "korvid/math/vec2.h"

namespace kv {

enum class ColliderShape : u8 {
    Circle, // `radius` around the position
    Box,    // `half` extents around the position, axis aligned
};

enum class BodyKind : u8 { Sensor, Solid, Kinematic };

enum ColliderFlags : u8 {
    ColliderPushable = 1, // a Solid that a pusher moves out of its way (it needs Move2 in its kind)
    ColliderPusher   = 2, // a Kinematic that pushes pushable solids instead of stopping at them
};

// The component. Plain data; set it at spawn or edit the column later.
struct Collider2 {
    f32  radius = 0.0f;        // Circle: radius in world units (pixels, in the examples)
    vec2 half;                 // Box: half width and half height; the box is 2*half across
    vec2 offset;               // the shape's center from the position
    u32  layer  = 1;           // which layers this collider is on; one bit per layer, any number of them
    u32  mask   = 0xFFFFFFFFu; // which layers it meets; the default is every layer
    f32  pace   = 1.0f;        // a pushable solid: the fraction of a pusher's step it moves at
    u8   shape  = (u8)ColliderShape::Circle;
    u8   body   = (u8)BodyKind::Sensor;
    u8   flags  = 0;           // ColliderFlags
    u8   pad    = 0;
};

// A circle on `layer` that meets `mask`: a sensor.
inline Collider2 collider_circle(f32 radius, u32 layer, u32 mask) {
    Collider2 c;
    c.radius = radius; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Circle;
    return c;
}

// An axis-aligned box of `half` extents on `layer` that meets `mask`: a sensor.
inline Collider2 collider_box(vec2 half, u32 layer, u32 mask) {
    Collider2 c;
    c.half = half; c.layer = layer; c.mask = mask; c.shape = (u8)ColliderShape::Box;
    return c;
}

// A box body: `half` extents around position + `offset`.
inline Collider2 collider_body(BodyKind body, vec2 half, vec2 offset, u32 layer, u32 mask, u8 flags = 0) {
    Collider2 c = collider_box(half, layer, mask);
    c.offset = offset; c.body = (u8)body; c.flags = flags;
    return c;
}

// The intent: how far the body wants to go this tick, world units. Written in Sim,
// consumed by collision.resolve, zeroed in PostSim.
struct Move2 { vec2 delta; };

// What the resolve did with this tick's intent, on a kind that has it.
struct MoveResult2 {
    vec2   asked;       // the intent
    vec2   moved;       // what the body moved, its own and pushed together
    Handle blocker;     // the first body that stopped it; null for the world or nothing
    Handle pushed;      // the body it pushed this tick, if any
    Handle pushed_by;   // the body that pushed it this tick, if any
    u8     hit_x = 0, hit_y = 0;  // stopped on that axis (by a body or the world)
    u8     displaced = 0;         // something else moved it this tick
    u8     pad = 0;
};

// Emitted by "collision.broadphase" in phase Sim, after the resolve: a consumer in Sim runs
// after it, and PostSim's see them the same tick.
struct OverlapBegin { Handle a, b; };
struct OverlapEnd   { Handle a, b; };

} // namespace kv
