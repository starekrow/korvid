// Korvid scene: the two components that make an entity a child. A kind that has
// Transform2, Local2 and Parent is a child kind: each tick the system "scene.compose"
// (phase PostSim, on paused ticks too) writes every child's Transform2 as the parent's
// world placement composed with the child's Local2. Parents are any entity with a
// Transform2, in any kind, children included; depth is walked up the chain, so a parent's
// world placement is final before its children read it.
//
// A child's Transform2 is written at the end of the tick: a Sim system that reads it
// (collision) sees last tick's placement. Its previous copy is the engine's, so a child
// of a moving parent interpolates like anything else.
//
// A parent that no longer resolves makes the child an orphan: with OrphanPolicy::Destroy
// (the default) the child is destroyed through the context at that tick's end; with
// Detach the parent handle is cleared and the child stays where it last was, a root.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/render/sprite.h"

namespace kv {

enum class OrphanPolicy : u8 { Destroy, Detach };

struct Parent {
    Handle       parent;                         // null: not a child this tick; Local2 is ignored
    OrphanPolicy orphan = OrphanPolicy::Destroy; // what happens when `parent` stops resolving
};

// The placement relative to the parent: position in the parent's scaled, rotated frame,
// rotation added, scale multiplied. Spawned rows are zero, so pass a Local2 (scale 1)
// when spawning a child: a zero scale makes the child vanish.
struct Local2 {
    point2 position;
    f32    rotation = 0.0f;
    vec2   scale{1.0f, 1.0f};
};

inline Local2 local2_at(point2 position, f32 rotation = 0.0f, vec2 scale = vec2{1.0f, 1.0f}) {
    Local2 l;
    l.position = position;
    l.rotation = rotation;
    l.scale    = scale;
    return l;
}

// parent (world) composed with local: what a child's Transform2 becomes.
Placement2 placement_compose(const Placement2& parent, const Local2& local);

} // namespace kv
