# scene

The transform hierarchy, or the beginning of one: an entity in any ktable can be the child
of any entity that has a `Transform2`, follow it through position, rotation and scale, and
go with it when it dies. Two components make a child, `Parent` and `Local2`; one system,
`scene.compose`, writes every child's world `Transform2` at the end of every tick, in
depth order, before the sprite extract reads it. Nothing else in the engine knows about
parents: the renderer sees world placements, the sim sees world placements, and a child
is an ordinary row of an ordinary ktable.

## How it works

**The components.** `Parent { Handle parent; OrphanPolicy orphan; }` names the parent by
handle, in any ktable, and says what to do when it stops resolving. `Local2` is a placement
relative to the parent: a position in the parent's scaled, rotated frame, a rotation
added to the parent's, a scale multiplied by it. A ktable with `Transform2`, `Local2` and
`Parent` is a child ktable. A row whose `parent` is null is not a child this tick and its
`Local2` is ignored.

**The system.** `scene.compose` runs in PostSim on every tick, paused or not, reading
`*.Local2` and writing `*.Transform2` and `*.Parent`. It walks every child ktable once:

1. A child whose parent does not resolve (destroyed, or never valid) is an orphan:
   `OrphanPolicy::Destroy` (the default) destroys it through the context, so it is gone
   at the end of the phase; `Detach` clears the parent handle and leaves the row where it
   was, a root from now on.
2. Otherwise the child's depth is the length of its parent chain, walked up through
   `Parent` columns until a root (or an orphan, which composes on its own). A chain longer
   than `KV_SCENE_MAX_DEPTH` (16) is a cycle: the row is skipped, counted, and logged once.
3. Children are then composed depth by depth, so a parent that is itself a child has its
   world placement written before any of its children read it. The ktables' order never matters.

The scratch arena holds the per-tick lists; the module keeps only counters
(`scene_stats`: composed, orphaned, cycles, deepest).

**Interpolation.** The child's `Transform2` has a previous copy like any other, taken by
the engine at PreSim, so last tick's composed placement is this tick's previous one and
the renderer interpolates a child of a moving parent exactly as it does the parent.

## Using it

A child ktable, and a child spawned with its parent's handle:

```cpp
constinit const KTableDesc k_ktables[] = {{.name = ".flame", .components = "Transform2, Sprite, Local2, Parent", .capacity = 4}};

// In a handler that holds the write on mygame.flame:
KTable* flames = ctx.write_ktable(g_flame_ktable);
flames->spawn(Transform2{}, flame_sprite, local2_at(point2{-26, 0}), Parent{ship});
```

Pass a `Local2` when spawning: rows start zeroed, and a zero scale makes the child
vanish. `scene_attach(table, child, parent, local)` and `scene_detach(table, child)` do
the same writes later. The `Transform2` passed at spawn is what the child shows until the
end of its first tick, when the compose overwrites it.

Add `"scene"` to the module's `deps` in `module.lua`; the generated module list puts it
after `sprite` and before the game.

## Quick reference

Header `korvid/scene/parent.h`:

| name | kind | what |
|---|---|---|
| `Parent` | component | `parent` (Handle; null: not a child), `orphan` (`OrphanPolicy::Destroy` or `Detach`) |
| `Local2` | component | `position` (point2), `rotation` (f32 radians), `scale` (vec2); relative to the parent |
| `local2_at(position, rotation = 0, scale = {1, 1})` | function | a constructor |
| `placement_compose(parent, local)` | function | the math on its own: parent (world) composed with local |

Header `korvid/scene/module.h`:

| name | kind | what |
|---|---|---|
| `scene_module` | record | the module |
| `scene_attach(table, child, parent, local, orphan = Destroy)`, `scene_detach(table, child)` | functions | the row writes; false for a stale handle or a ktable without `Parent` |
| `scene_stats(engine)` | function | last tick's counters: `composed`, `orphaned`, `cycles`, `max_depth` |
| `KV_SCENE_MAX_DEPTH` | constant | 16 |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Parent`, `Local2` | components | sim state, hashed |
| `scene.compose` | system, phase PostSim, `Always` | reads `*.Local2`; writes `*.Transform2`, `*.Parent`; destroys orphans by command |

Depends on: core, math, render, sprite.

## Behaviour worth knowing

- **A child's world placement is one tick old to Sim**, unless the child has a collider.
  It is written at the end of the tick, so a Sim system that reads it sees where the
  child was at the end of the last tick. Children with a `Collider2` are composed in Sim
  as well, by `collision.compose` (`scene_compose_with`), after the resolve and before
  the broadphase, so they collide where they are drawn; a Sim system that reads them
  after that pass sees them composed.
- **A destroyed parent's child lingers one frame.** The orphan is destroyed by command at
  the end of PostSim; the frame's extract still sends its last placement, and the next
  frame's removes it.
- **Only placement is inherited.** Not tint, layer, space or visibility; a child in screen
  space with a parent in world space composes pixels against world units, which is almost
  never what anyone wants.
- **The parent must have a `Transform2`.** A handle to a ktable without one is an orphan.
- **Depth is walked, not stored.** Cost is rows times depth per tick; a stored,
  depth-sorted forest is the upgrade if a profile ever asks for it.

## Not in this module

Inherited anything but placement, a stored forest, attach and detach events, finding
children from a parent, bones and skinning (an animation clip driving a child's `Local2`
is where that starts), and any editor notion of a scene.
