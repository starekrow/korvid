# collision

Overlap detection between entities, delivered as events. Give a kind a `Collider2`
next to its `Transform2` and the module tells you, every tick, which pairs of entities
started or stopped overlapping. What an overlap means is the game's business: a bullet
hits a rock, a player enters a trigger volume, two stones bump. The module finds the
pairs; your handlers do the rest.

## How it works

Once per tick, in phase `Sim`, the system `collision.broadphase` runs after every
system that moves or spawns things (the scheduler derives that from its read set: every
`Transform2` and `Collider2` column). It:

1. Gathers every live entity of every kind that has both components.
2. Drops them into a uniform grid whose cell is the diameter of the largest collider, so
   any two overlapping colliders sit in the same or neighbouring cells.
3. Tests each candidate pair once: layers against masks first, then the shapes.
4. Sorts the pairs it found into a canonical list and diffs it against last tick's list.
   A pair that is new becomes an `OverlapBegin` event; a pair that is gone becomes an
   `OverlapEnd` event. A pair that is still there produces nothing.

The events are emitted during `Sim`, so handlers subscribed to them run at the next
phase boundary of the same tick, and any `PostSim` system can read them as a batch.

Pairs are canonical, the handle with the lower bits first. The events carry handle
fields with schemas, so the bus drops an event whose entity has since been destroyed:
destroy a rock in one handler and the `OverlapEnd` that would have named it never
arrives. Entities that spawn and overlap in the same tick are reported that tick.

There is no response. Nothing is pushed apart, no normal or depth is computed. If you
want bouncing, do it in the handler with the two transforms, or keep a plain loop like
`example_stones` does.

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
| `Collider2` | component | `radius` (circle), `half` (box, vec2), `layer` (u32 bits), `mask` (u32 bits), `shape` (u8) |
| `ColliderShape` | enum | `Circle`, `Box` |
| `collider_circle(radius, layer, mask)` | function | a circle collider |
| `collider_box(half, layer, mask)` | function | an axis-aligned box collider |
| `OverlapBegin{a, b}` | event | the pair started overlapping this tick; `a` has the lower handle bits |
| `OverlapEnd{a, b}` | event | the pair stopped overlapping this tick, or one of them moved out |

Header `korvid/collision/module.h`:

| name | kind | what |
|---|---|---|
| `collision_module` | `ModuleDesc` | the module record; kormake puts it in the list for you |
| `collision_pair_count(Engine&)` | function | pairs overlapping after the last broadphase |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Collider2` | component | 24 bytes, 5 fields, sim state (hashed, described) |
| `OverlapBegin`, `OverlapEnd` | event types | schemas on both; stale handles drop the event |
| `collision.broadphase` | system, phase Sim | reads `*.Transform2`, `*.Collider2`; writes nothing |

Resources a system of yours might name: none. The broadphase reads columns; to read a
collider yourself, name `kind.Collider2` in your sets like any other column.

## Behaviour worth knowing

- **Rotation and scale are ignored.** The circle is around the position; the box is
  axis aligned around it. A rotating sprite keeps an upright box.
- **Overlap means strictly inside.** Two circles touching at exactly their radii sum do
  not overlap; the test is `distance < ra + rb`. Boxes likewise.
- **One event per transition.** An overlap that persists for a hundred ticks is one
  `OverlapBegin` and, when it ends, one `OverlapEnd`.
- **Cell size follows the largest collider.** One huge collider among many small ones
  makes the grid coarse and the candidate lists longer. Keep radii within an order of
  magnitude of each other, or expect the broadphase to show up in `--prof-report`.
- **Mask and layer edits take effect next tick.** The broadphase reads the column once
  per tick, after the movers.
- **Determinism.** Pairs are found in slot order and sorted, so the same world gives
  the same events on any thread count. The broadphase itself runs on one thread today.
- **Cost.** Linear in colliders plus the candidate pairs. A few thousand colliders is a
  fraction of a millisecond; the test suite runs 400 circles on a lattice and checks it
  finds exactly their 760 touching pairs.

## Not in this module

Collision response, contacts, raycasts, swept shapes, polygons, rotated boxes, spatial
queries you call yourself ("what is near this point"), and a narrowphase beyond circle
and box. The grid stays inside the system; a query API over it would be the natural next
addition if a game needs one.
