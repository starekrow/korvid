# example_stones

The engine used the other way round. `example_rocks` is written the way the engine would
like: kinds, systems with declared sets, handlers, timers, a trigger. This one is written
the way you would write it on a Saturday: a `Stone` struct, an `Array<Stone>`, and one
function that does input, motion, collision and drawing in order, every tick.

The point is to show the floor. Everything the engine asks of this program fits in a
short list:

- three strings in the system's declaration, naming what it touches: the sprite kind, the
  keys, the window;
- `ctx.write_kind("sprite.entity")` for the sprite table, a `spawn` per stone and a
  `get<Transform2>` per stone per tick to move its sprite;
- two lines of reflection for `Stone` (`KV_FIELDS`, `KV_COMPONENT`), used only to print it;
- a texture and two materials at startup;
- a loop over `ExitRequested` to quit.

Everything else is plain C++ and the engine never sees it. The dev build only speaks up if
one of the three strings is missing, and then it names the system and the resource.

What comes free: render interpolation (the engine copies transforms at the top of every
tick, you write them during it), null for a stale sprite handle, handles that print as
`sprite.entity#3`, and `--hash` over every sprite position.

What you give up, quietly: the engine cannot see your velocities or bump counts, so they
are not hashed, traced, snapshotted or visible to a tool; the scheduler sees one system
and can never run anything in parallel; collision is yours to keep right.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory,
where the shaders and this module's `assets/` have been copied next to it:

```bash
tools/kormake/bin/linux/kormake build linux-debug
build/linux-debug/bin/example_stones
```

`kormake build linux-debug` is everything checked with the validation layers on;
`linux-dev` is the build to measure with; `linux-ship` has neither. `kormake build`
builds every module and example; `kormake test linux-debug` runs the test suites.

## Small changes that pay off

None of these is a rewrite. Each is a few lines, and each can be taken on its own.

**Put `Stone` in a kind.** The two reflection lines are already the declaration a kind
needs. Add the kind, and spawn into it instead of pushing into the array:

```cpp
constinit u32 g_stone_kind = 0;
constinit const char* const k_stone_components[] = {"Stone", "Transform2", "Sprite"};
constinit const KindDesc k_kinds[] = {
    {.name = "example_stones.stone", .components = k_stone_components, .capacity = 256, .id = &g_stone_kind},
};
// in the module table:  .kinds = k_kinds
// in the tick:          Table& stones = *ctx.write_kind(g_stone_kind);
//                       stones.spawn(Stone{...}, transform2_at(at), sprite);
```

Now `--hash` covers velocities and bump counts, two runs can be compared tick for tick,
`--hash-systems` says which system diverged, `kv_handle_describe` prints a whole stone,
the eventual CLI can list and inspect them, and a snapshot of the world includes them.
The `sprite` handle field goes away: the stone *is* the sprite's entity, and the extract
walks every kind that has `Transform2` and `Sprite`. The n-squared loop and the
wall bounces stay exactly as they are, over `stones.rows_of<Stone>()` instead of the array.

**Use a timer instead of counting ticks.** `g.ticks % 10` for the drop rate becomes a
scheduled event and a four-line handler:

```cpp
struct DropStone { u32 nothing; };
void on_drop(TickContext& ctx, Span<const DropStone>) { /* add_stone(...); */ ctx.schedule_in(DropStone{}, 10); }
// in the module table:  .events = {KV_EVENT(DropStone)}, .handlers = {{.name = "example_stones.drop", KV_ON(DropStone, on_drop)}}
```

A timer survives a pause, can be cancelled, is visible in the trace, and does not drift
when the tick rate changes.

**Emit an event when stones bump.** One line in `bounce`:

```cpp
ctx.emit(Bump{a.sprite, b.sprite});
```

With a schema on `Bump`, `--trace-events` prints every collision with who emitted it, a
bump that names a stone destroyed in the same tick is dropped for you, and anything else
that cares (a sound, a score, a particle) subscribes with a handler instead of being
wired into the loop.

**Let the collision module find the pairs.** Add `Collider2` to the kind above, put the
`collision` module in the list, and the n-squared loop is replaced by `OverlapBegin`
events arriving at the next phase boundary. The bounce response stays yours; only the
search moves out.

**Split the tick when it grows.** Input, motion, collision and sprite sync are four
systems waiting to happen. Give each its sets and the scheduler orders them from what they
read and write, rejects a conflict at startup, and later runs the independent ones on
other cores. Until then, one fat system with a write on everything is legal and costs
nothing.

**Watch a value instead of polling it.** A trigger on a field of a `prev` component fires
an event on the edge:

```cpp
{.name = "example_stones.tenth_bump", .field = "example_stones.stone.Stone.bumps", .edge = TriggerEdge::CrossedAbove, .value = 9}
```

**Publish the stones for someone else.** Another module (a scoreboard, a replay recorder,
a debug overlay) reads them through `ctx.resource<const Game>("example_stones.game")` once
this module declares the resource and publishes it at startup, and the scheduler then
knows that module reads what this one writes.

Each of these is a la carte. The engine does not mind which you take, or when.
