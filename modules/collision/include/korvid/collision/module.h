// Korvid collision: module record. Declares Collider2 and the overlap events, and the
// system "collision.broadphase" (phase Sim, after every writer of a Transform2 or
// Collider2 column) that diffs this tick's overlap pairs against the last tick's.
#pragma once
#include "korvid/core/module.h"
#include "korvid/collision/collider.h"

namespace kv {

struct Engine;

KV_MODULE_DECLARE(collision_module);

// How many pairs overlapped after the last broadphase; for reports.
u32 collision_pair_count(Engine& e);

} // namespace kv
