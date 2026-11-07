# sprite

What a drawable thing is, on the game's side of the render boundary. The module declares
two components, `Transform2` and `Sprite`; any kind that has both is drawn, whoever
declared it. At the end of every tick the extract looks at every such kind, finds what is
new, moved, changed or gone since it last looked, and writes a delta stream the frame
loop hands to the renderer. The renderer keeps a persistent world keyed by entity handle
and never sees an entity, a component or a kind. The module never includes a graphics
API.

## How it works

**The components.** `Transform2` is a `Placement2`: position, rotation (radians,
counter-clockwise) and scale, with `CompPrev` so the engine keeps last tick's copy.
`Sprite` is the material (a renderer handle), a UV rect, a size in world units, a tint
and a layer. Both carry field tables, so they hash, compare, print and trace like any
component.

**The kind.** `sprite.entity` is a kind with exactly those two components, a million
slots of address space, for things that are only sprites: a background, a particle, a
stone in a game that keeps its own structs. `SpriteWorld` is the convenience surface over
that table (`create`, `destroy`, typed lookups). A game's own kinds add whatever else they
want next to the two components and are drawn the same way.

**The extract.** The system `sprite.extract` runs in PostSim, after every writer of a
`Transform2` or `Sprite` column, reading `*.Transform2` and `*.Sprite`. Per kind, it
keeps the handle it last sent for each slot and the `Sprite` it last described. Then:

1. A slot whose handle changed or died since the last extract becomes a `Remove` for the
   handle the renderer knows there. No table needs a destroy hook.
2. A live slot the renderer has not seen, or whose `Transform2` differs from its previous
   copy, or whose `Sprite` differs from what was last sent, becomes a `Set` with the
   whole `RenderSpriteState`.
3. The tick's deltas start with a `Tick` marker, so the renderer knows where "previous"
   ends whatever the tick-to-frame ratio.

Nothing is copied twice: a sprite that did not move and did not change produces nothing.
The deltas accumulate per frame (several ticks in one frame append in order) and the
frame loop applies and clears them before the render.

**Interpolation** is the renderer's, from the previous and current placement of each
sprite it was told about, at the clock's alpha. The extract makes that possible by
sending a `Set` whenever the transform moved, which the `CompPrev` copy detects.

## Using it

**A kind that is drawn:**

```cpp
constinit const char* const k_rock_components[] = {"Transform2", "Sprite", "Velocity"};
constinit const KindDesc k_kinds[] = {{.name = "mygame.rock", .components = k_rock_components, .capacity = 1024}};

Sprite s;
s.material = g.rock_material;      // from a material asset
s.size     = vec2{96.0f, 96.0f};   // world units (pixels, with the default view)
s.layer    = 1;                    // ascending draw order; clamped by the renderer to [-128, 127]
rocks.spawn(transform2_at(at, rotation), s, Velocity{drift});
```

Then move it by writing `Transform2` in a system that declares the write; the extract
notices. Change its look by writing `Sprite`: a new tint, a new UV rect for a frame of
animation, a new material.

**A sprite that is nothing else:**

```cpp
Sprite bg;
bg.material = g.bg_material;
bg.size     = vec2{w, h};
bg.uv       = rect2{0.0f, 0.0f, w / 128.0f, h / 128.0f};   // tiles a 128-pixel texture
bg.layer    = -1;
ctx.spawn_later("sprite.entity", transform2_at(point2{0.0f, 0.0f}), bg);
```

or, outside the schedule, `sprite_world(engine)->create(transform, sprite)`.

**Reading what was sent** (a tool, a test): `sprite_deltas(engine)` is the array the
frame loop consumes; `extract_sprites(engine, state, out)` runs the extract by hand.

## Quick reference

Header `korvid/sprite/transform2.h`, `sprite.h`, `components.h`:

| name | kind | what |
|---|---|---|
| `Transform2` | component | a `Placement2`: `position` (point2), `rotation` (f32 radians), `scale` (vec2); `CompPrev` |
| `transform2_at(position, rotation = 0, scale = {1, 1})` | function | a constructor |
| `Sprite` | component | `material` (Handle, null: the default), `uv` (rect2, unit by default), `size` (vec2, 64x64), `tint` (color, white), `layer` (i32, 0) |
| `transform2_schema()`, `sprite_schema()` | functions | the two `ComponentDesc`s, for comparing or printing rows |

Header `korvid/sprite/world.h`:

| name | kind | what |
|---|---|---|
| `SpriteWorld` | record | over the `sprite.entity` table: `create(t, s)`, `destroy(h)`, `valid(h)`, `live()`, `kind()`, `transform(h)`, `prev_transform(h)`, `sprite(h)` |

Header `korvid/sprite/extract.h`:

| name | kind | what |
|---|---|---|
| `ExtractState`, `ExtractTable` | records | per table: the handle and the `Sprite` last sent, by slot |
| `extract_sprites(engine, state, out)` | function | one tick's deltas appended to `out`, starting with `Tick` |

Header `korvid/sprite/module.h`:

| name | kind | what |
|---|---|---|
| `sprite_module` | `ModuleDesc` | the record |
| `sprite_world(engine)` | function | the module's world, or null |
| `sprite_deltas(engine)` | function | the deltas extracted since the last take; apply and clear |

From the render interface (`korvid/render/sprite.h`): `Placement2`, `RenderSpriteState`,
`SpriteDelta{kind, entity, state}`, `SpriteDeltaKind` (`Tick`, `Set`, `Remove`).

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Transform2` | component | 20 bytes, 3 fields, `CompPrev`, sim |
| `Sprite` | component | 56 bytes, 5 fields, sim |
| `sprite.entity` | kind | `Transform2`, `Sprite`; 1 M slots of address space |
| `sprite.deltas` | resource | the delta array; the extract writes it |
| `sprite.extract` | system, phase PostSim | reads `*.Transform2`, `*.Sprite`; writes `sprite.deltas` |

## Behaviour worth knowing

- **Both components are sim state.** They are hashed; a game whose sprites are cosmetic
  only still hashes their placement. Debris that must not affect the hash goes in a
  kind with its own cosmetic components and the two sim ones.
- **A `Set` is the whole state.** The renderer does not merge fields; the extract sends
  everything when anything changed. Changing a tint every tick costs the same as moving.
- **The previous copy is the engine's, taken at the top of PreSim.** A transform written
  twice in one tick (moved, then wrapped) is one delta with the final value, and the
  renderer interpolates from where the tick began to where it ended; a wrap across the
  screen therefore sweeps across it for one frame. Teleports that must not interpolate
  are a renderer feature not built yet.
- **Rotation interpolates as a scalar.** Keep angles continuous.
- **Layer is draw order, not depth.** Equal layers draw in slot order, stable frame to
  frame; the renderer clamps to [-128, 127].
- **The extract is one thread** and linear in the rows of every drawable kind, dead
  slots included; a kind with a huge capacity and few live rows still costs its
  high-water mark.

## Not in this module

Animation (frames are a UV rect the game writes), sprite sheets and atlases, text,
cameras and views, parenting and hierarchies, z-sorting beyond the layer, render-rate
entities, tile maps, and anything the renderer does with the deltas (`docs/design/renderer.md`).
