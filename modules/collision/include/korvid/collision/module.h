// Korvid collision: the module record.
//
// What the module declares, in the engine's terms:
//
//   component  Collider2               radius, half, layer, mask, shape
//   events     OverlapBegin, OverlapEnd  with schemas, so a stale handle drops the event
//   system     collision.broadphase     phase Sim; reads *.Transform2 and *.Collider2
//
// The system reads every kind that has both components, so the scheduler runs it after
// every system that writes a Transform2 or Collider2 column in Sim: movement, wrapping
// and spawning are done by the time it looks. It writes nothing of the game's and keeps
// only its own list of last tick's pairs.
//
// To use it: add "collision" to your module's deps and the module to the executable's
// list (kormake does that from the deps), give a kind "Collider2" next to "Transform2",
// and subscribe a handler to OverlapBegin or OverlapEnd. See the module's README.
#pragma once
#include "korvid/core/module.h"
#include "korvid/collision/collider.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(collision_module);

// How many pairs were overlapping after the last broadphase. For reports and tests; a
// game that wants the pairs themselves listens to the events.
u32 collision_pair_count(Engine& e);

} // namespace kv
