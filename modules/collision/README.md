# collision

Bodies that move and stop in the same tick, and overlaps delivered as events. Give a kind
a `Collider2` next to its `Transform2` and it takes part, as one of three bodies:

- a **sensor** (the default) reports when it starts and stops overlapping things, and
  blocks nothing: bullets and rocks, exits, trigger zones;
- a **solid** blocks moving bodies and never moves on its own, unless it is pushable and
  something that pushes walks into it: walls, villagers standing still, boulders;
- a **kinematic** body moves by its intent, is stopped by solids, other kinematic bodies and
  the static world (a tile map), and stops other kinematic bodies: the player, a villager.

What a stop or an overlap means is the game's business; the module moves, stops, pushes
and reports.

## How it works

Every tick, in phase `Sim`:

1. **Intents.** The game writes `Move2::delta` on its kinematic bodies: how far each wants
   to go this tick. A body that may move (kinematic, or a pushable solid) has `Move2` in its
   kind; a kind that wants to know what happened has `MoveResult2` too.
2. **`collision.resolve`** runs after every system that writes a `Move2`, `Transform2` or
   `Collider2` (the scheduler derives that from its sets). It takes the intents in a fixed
   order, kinds by name and then slots, and moves each one along x and then y, swept
   against the static world and against every solid and kinematic body *as it stands at
   that moment*: the second mover sees where the first ended this tick, so two bodies never
   end up in one place. A pusher that meets a pushable solid moves it, at the solid's
   `pace`, as far as the pusher goes past it, against everything but the pusher, one deep.
   Then it writes `Transform2` (a child kind's `Local2` by the same delta) and
   `MoveResult2`: what was asked, what moved, which axes stopped, what stopped it, what it
   pushed, who pushed it. `collision.moved` lists the bodies something else moved.
3. **The game reacts** in a Sim system that reads `MoveResult2` or `collision.moved`: the
   scheduler puts it after the resolve. Intent, resolve, react: one tick.
4. **`collision.compose`** places the children that have a collider (`Parent`, `Local2`
   and `Collider2`: a boss's parts, a ship's graze ring) where their parents now are, by
   `scene_compose_with`, so they are tested where they will be drawn and not where the
   last tick's compose left them. The resolve, before it, sees child bodies where that
   last compose put them.
5. **`collision.broadphase`** finds every pair of colliders that overlap where everything
   ended, and diffs the list against last tick's: a new pair is an `OverlapBegin`, a pair
   that is gone an `OverlapEnd`. Handlers run at the next boundary, the same tick.
   Colliders are grouped by their (layer, mask) first: each group has a grid sized to its
   own largest member, a group is paired with itself only if it meets itself and with
   another only if their masks meet, and across two groups the smaller is walked over the
   larger's grid. Bullets that never meet bullets are never paired with each other, and a
   few bodies against tens of thousands of bullets is a few lookups (`src/pairs.cc`).

`collision.clear` zeroes the intents in PostSim, so an intent lasts one tick.

The **static world** is one sweep function the resolve calls before the bodies
(`collision_static`); the tilemap module installs a map's solids with
`tilemap_collide(e, map, origin)`. The module itself knows nothing about tile maps.

**Queries.** `collision_query(ctx, rect, layers, out)` lists the colliders overlapping a
rectangle, `collision_cast(ctx, box, delta, layers)` the first one a moving box would enter
and when, `collision_cast_all(ctx, box, delta, layers, out)` every one it meets, nearest
first; all three over the colliders as they stand when called. The hammer's reach is a
query; arcfire's beam is a cast of them all.

## Using it

**Declare the dependency.** In your `module.lua`:

```lua
module "mygame" {
  deps = { "core", "math", "render", "sprite", "collision", ... },
}
```

kormake adds the collision module to your executable's module list from that.

**Give a kind a collider.** `Collider2` goes next to `Transform2` in the kind's
component list, and the spawn passes one:

```cpp

// layers are bits you choose
constexpr u32 L_SHIP = 1, L_BULLET = 2, L_ROCK = 4;

rocks.spawn(transform2_at(at), sprite, velocity,
            collider_circle(44.0f, L_ROCK, L_SHIP | L_BULLET)); // on ROCK; meets ships and bullets
```

A box is `collider_box(vec2{20, 10}, layer, mask)`, half extents, axis aligned.

**Subscribe to the events.** A handler with `KV_ON`, and the module table lists it:

```cpp
void on_overlap(TickContext& ctx, Span<const OverlapBegin> overlaps) {
    Game& g = ctx.state_as<Game>();
    for (const OverlapBegin& o : overlaps) {
        // Which kinds met? A handle's kind is the table it belongs to; note them at startup.
        u16 ka = o.a.kind(), kb = o.b.kind();
        if (ka == g.bullet_kind && kb == g.rock_kind) ctx.emit(RockHit{o.b, o.a});
        else if (ka == g.rock_kind && kb == g.bullet_kind) ctx.emit(RockHit{o.a, o.b});
    }
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".on_overlap", KV_ON(OverlapBegin, on_overlap)},
};
```

The handle kinds come from the tables once the engine is up, in your startup:

```cpp
g->rock_kind   = engine_table(engine, g_rock_kind)->kind;
g->bullet_kind = engine_table(engine, g_bullet_kind)->kind;
```

Because the handles in a delivered event are live, a handler that holds a write on a
column may index it directly: `ctx.write<Health>(k_hp)[o.a.index()]`.

**Make it a body.** A kind that moves by intent has `Collider2, Move2, MoveResult2`; a
system writes the intent, another reads the result:

```cpp
{.name = ".hero", .components = "Transform2, Sprite, Collider2, Move2, MoveResult2", .capacity = 1},

// spawn: a feet box, kinematic, pushing boulders
heroes.spawn(transform2_at(at), sprite, collider_body(BodyKind::Kinematic, vec2{5, 2.5f}, vec2{0, -5.5f}, L_ACTOR, L_ACTOR, ColliderPusher), Move2{}, MoveResult2{});

// Sim, writes .hero.Move2: the intent
move[i].delta = dir * speed * (f32)ctx.dt;
// Sim, reads .hero.MoveResult2 (after the resolve, this tick): the reaction
if (res[i].blocker.kind() == g.villager_hk) lose_heart();
```

A pushable boulder is `collider_body(BodyKind::Solid, ..., ColliderPushable)` with `pace`
set and `Move2` in its kind (the resolve writes its position).

**Pick layers deliberately.** A collider's `layer` is the set it is on; its `mask` is the
set it will meet. Both directions must agree, so a bullet with `mask = L_ROCK` never
meets a ship even if the ship accepts bullets. Bullets on layer `L_BULLET` with mask
`L_ROCK` and rocks with mask `L_SHIP | L_BULLET` give bullet-rock and ship-rock pairs
and nothing else; rocks never test against rocks.

**Read the count.** `collision_pair_count(engine)` is how many pairs were overlapping
after the last broadphase, for an fps line or a test.

## Quick reference

Header `korvid/collision/collider.h`:

| name | kind | what |
|---|---|---|
| `Collider2` | component | `radius` (circle), `half` (box), `offset`, `layer`, `mask`, `pace` (a pushable's), `shape`, `body`, `flags` |
| `ColliderShape`, `BodyKind`, `ColliderFlags` | enums | `Circle`, `Box`; `Sensor`, `Solid`, `Kinematic`; `ColliderPushable`, `ColliderPusher` |
| `collider_circle(radius, layer, mask)`, `collider_box(half, layer, mask)` | functions | sensors |
| `collider_body(body, half, offset, layer, mask, flags)` | function | a box body |
| `Move2` | component | `delta`: the intent, this tick |
| `MoveResult2` | component | `asked`, `moved`, `blocker`, `pushed`, `pushed_by`, `hit_x`, `hit_y`, `displaced` |
| `OverlapBegin{a, b}` | event | the pair started overlapping this tick; `a` has the lower handle bits |
| `OverlapEnd{a, b}` | event | the pair stopped overlapping this tick, or one of them moved out |

Header `korvid/collision/module.h`:

| name | kind | what |
|---|---|---|
| `collision_module` | `ModuleDesc` | the module record; kormake puts it in the list for you |
| `collision_pair_count(Engine&)` | function | pairs overlapping after the last broadphase |
| `collision_stats(Engine&)` | function | the last resolve (bodies, moves, blocked axis moves, pushes) and broadphase (colliders, candidate pairs tested, (layer, mask) groups) |
| `collision_static(e, fn, user)` | function | the static world's sweep; `tilemap_collide` sets it |
| `collision_query(ctx, rect, layers, out)`, `collision_cast(ctx, box, delta, layers, ignore)`, `collision_cast_all(ctx, box, delta, layers, out)` | functions | colliders in a rectangle; the first a moving box enters; every one it meets, nearest first (a beam's) |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Collider2`, `Move2`, `MoveResult2` | components | sim state (hashed, saved) |
| `OverlapBegin`, `OverlapEnd` | event types | schemas on both; stale handles drop the event |
| `collision.moved` | resource | `Array<Handle>`: the bodies something else moved this tick |
| `collision.resolve` | system, phase Sim | reads `*[Collider2].Transform2` (child bodies' early, `^`), `*[Collider2].Collider2`, `*[Move2].Move2`; writes `*[Move2].Transform2`, `*[Move2].Local2`, `*[MoveResult2].MoveResult2`, `collision.moved` |
| `collision.compose` | system, phase Sim | after the resolve and every Sim writer of a `Transform2`; reads `*.Transform2`, `*[Parent+Collider2].Local2`, `.Parent`; writes `*[Parent+Collider2].Transform2` |
| `collision.broadphase` | system, phase Sim | after the compose; reads the same columns as the resolve |
| `collision.clear` | system, phase PostSim | writes `*[Move2].Move2` |

Kinds without `Move2` are only read, so a game that moves its own sensors (rocks) keeps
doing so and the broadphase reports them after its movers.

## Behaviour worth knowing

- **Rotation and scale are ignored.** The circle is around the position; the box is
  axis aligned around it. A rotating sprite keeps an upright box.
- **Overlap means strictly inside.** Two circles touching at exactly their radii sum do
  not overlap; the test is `distance < ra + rb`. Boxes likewise.
- **One event per transition.** An overlap that persists for a hundred ticks is one
  `OverlapBegin` and, when it ends, one `OverlapEnd`.
- **Cell size follows each group's largest collider.** One huge collider coarsens only its
  own group's grid; it costs its own lookups over the groups it meets, not everyone's.
  Within a group that meets itself, keep radii within an order of magnitude.
- **A fresh sim forgets the pairs.** On `SimReset` (`engine_sim_reset`) the last tick's
  pairs go, since the new run is issued the same handles: every overlap of the new run
  begins.
- **Mask and layer edits take effect next tick.** The broadphase reads the column once
  per tick, after the movers.
- **Determinism.** Pairs are found in slot order and sorted, so the same world gives
  the same events on any thread count. The broadphase itself runs on one thread today.
- **Cost.** Linear in colliders plus the candidate pairs that can meet. A few thousand
  colliders is a fraction of a millisecond; the test suite runs 400 circles on a lattice
  and checks it finds exactly their 760 touching pairs, and holds sixty random scenes of
  mixed sizes, shapes and masks against a test of every pair (`tests/pairs_test.cc`).

- **Order decides.** Two bodies walking into each other resolve by order: the first takes
  the gap, the second stops at it. The order is the same in every run (kinds by name,
  slots), not symmetric. A game that wants fairness alternates or slows both.
- **Read your position early.** A system that reads where a body is to decide its intent
  reads a column the resolve writes. Mark the read `^` (`.reads = "^.npc.Local2"`): it is
  this phase's earlier value, and the scheduler runs that system before the resolve
  instead of after it.
- **Teleport before the resolve.** A system that writes a body's `Transform2` after the
  resolve (in a reaction) is a cycle: the resolve reads it. Teleport in the system that
  writes the intent, before.
- **Blocking is boxes.** A circle blocks and is blocked as its bounding box; overlaps still
  test circles exactly.
- **Inside another body**, a box may move away from the other's middle, not toward it.
- **Children** move their `Local2` by the world delta, right for a child of an unrotated,
  unscaled parent (a level's root) and documented as the limit.

## Not in this module

Physics (velocities, mass, restitution), chains of pushes, rotated boxes, polygons, round
blocking, contact points and depths, continuous rotation, and a grid kept across ticks
(the resolve builds its own each tick, which is cheap for the hundreds of bodies a game
like arcanum has).
