// Korvid scene: module record and helpers. The module declares the components Parent and
// Local2 and one system:
//
//   scene.compose   phase PostSim, Always: reads *.Local2, *.Parent, writes *.Transform2
//                   and *.Parent (orphans detached); destroys orphans by command
//
// and scene_compose_with, the same composition for the children of some kinds only, which
// the collision module runs in Sim for children with colliders.
//
// Attaching is a row write: a child kind's row gets a Parent and a Local2, at spawn or
// later with scene_attach; scene_detach clears the parent. Both need the caller to hold
// the write on the child's kind. See the module's README.
#pragma once
#include "korvid/core/module.h"
#include "korvid/scene/parent.h"

namespace kv {

struct Table;

KV_MODULE_DECLARE(scene_module);

constexpr u32 KV_SCENE_MAX_DEPTH = 16; // a chain longer than this is treated as a cycle

// Makes `child` (a live row of `table`, which must have Parent and Local2) a child of
// `parent` at `local`. False if the handle is stale or the kind has no Parent column.
bool scene_attach(Table& table, Handle child, Handle parent, const Local2& local, OrphanPolicy orphan = OrphanPolicy::Destroy);
// Clears the parent; the child keeps its current Transform2 as a root. False as above.
bool scene_detach(Table& table, Handle child);

// Composes now, in depth order, the children in the kinds that have `component` (a
// component id) as well as Parent and Local2: for a system that needs them where their
// parents are before PostSim (the collision module's `collision.compose`, for children
// with colliders). Orphans and cycles are left to scene.compose. A child whose parent is
// itself a child of a kind without `component` composes against that parent's last
// placement. The caller declares the writes; returns the rows composed.
u32 scene_compose_with(Engine& e, u32 component);

// Counters, for tests and the overlay.
struct SceneStats {
    u32 composed  = 0; // children composed last tick
    u32 orphaned  = 0; // orphans handled last tick
    u32 cycles    = 0; // rows skipped as cycles last tick
    u32 max_depth = 0; // deepest child last tick
};
const SceneStats* scene_stats(Engine& e);

} // namespace kv
