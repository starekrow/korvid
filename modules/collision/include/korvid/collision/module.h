// Korvid collision: the module record.
//
// What the module declares, in the engine's terms:
//
//   components Collider2, Move2, MoveResult2
//   events     OverlapBegin, OverlapEnd  with schemas, so a stale handle drops the event
//   resource   collision.moved          Array<Handle>: the bodies something else moved this tick
//   systems    collision.resolve        Sim; reads the Transform2 and Collider2 of every kind
//                                       with a collider and every Move2; writes Transform2,
//                                       Local2 and MoveResult2 of the kinds that have Move2
//                                       (and MoveResult2), and collision.moved
//              collision.broadphase     Sim, after the resolve; reads the same columns
//              collision.clear          PostSim; zeroes every Move2
//
// The scheduler runs the resolve after every Sim system that writes a Move2, a Transform2
// or a Collider2, and before every Sim system that reads a MoveResult2 or collision.moved,
// so a game's tick goes intent, resolve, react. Kinds without Move2 are only read: a game
// that moves its own sensors (rocks) keeps doing so, and the broadphase reports them.
//
// To use it: add "collision" to your module's deps and the module to the executable's
// list (kormake does that from the deps), give a kind "Collider2" next to "Transform2",
// and subscribe a handler to OverlapBegin or OverlapEnd. See the module's README.
#pragma once
#include "korvid/core/module.h"
#include "korvid/collision/collider.h"
#include "korvid/core/array.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(collision_module);

// How many pairs were overlapping after the last broadphase. For reports and tests; a
// game that wants the pairs themselves listens to the events.
u32 collision_pair_count(Engine& e);

// The last resolve and broadphase, for reports and tests.
struct CollisionStats {
    u32 bodies  = 0; // solid and kinematic bodies
    u32 moves   = 0; // intents that were not zero
    u32 blocked = 0; // axis moves that stopped short
    u32 pushes  = 0; // bodies pushed
    // The broadphase: every collider, the candidate pairs it looked at (pairs whose layers
    // and masks meet, in neighbouring cells of the larger group's grid), and how many
    // (layer, mask) groups the colliders fell into.
    u32 colliders = 0;
    u32 tested    = 0;
    u32 groups    = 0;
};
const CollisionStats* collision_stats(Engine& e);

// The static world: one sweep function the resolve calls for every axis move before the
// bodies, with a world box (min corner, +y up) and a move along one axis. It returns where
// the box ends, setting `hit` when something stopped it and `started_inside` when it began
// in a solid, with tilemap_move's rules. The tilemap module installs one for a map
// (tilemap_collide); null removes it. Main thread, outside the tick.
typedef rect2 (*CollisionStaticFn)(void* user, rect2 box, vec2 delta, bool* hit, bool* started_inside);
void collision_static(Engine& e, CollisionStaticFn fn, void* user);

// Queries over every collider as it stands when called (call them after the resolve to
// see this tick's positions). Read the columns through the context: a system that calls
// them declares "*[Collider2].Transform2, *[Collider2].Collider2" in its reads.
//   collision_query  the colliders on any of `layers` whose box overlaps `area`, appended
//                    to `out` in kind and slot order; returns how many
//   collision_cast   the first collider on any of `layers` a box moving by `delta` would
//                    enter (not `ignore`), and the fraction of the move at which it would
//                    (1 and null when it meets none)
struct CollisionHit { Handle entity; f32 t = 1.0f; };
u32          collision_query(TickContext& ctx, rect2 area, u32 layers, Array<Handle>& out);
CollisionHit collision_cast(TickContext& ctx, rect2 box, vec2 delta, u32 layers, Handle ignore = Handle{});

} // namespace kv
