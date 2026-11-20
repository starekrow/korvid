# sprite

What a drawable thing is, on the game's side of the render boundary. The module declares
two components, `Transform2` and `Sprite`; any ktable that has both is drawn, whoever
declared it. Before each frame is drawn the extract looks at every such ktable, finds what
is new, moved, changed or gone since it last looked, and writes a delta stream the frame
loop hands to the renderer. The renderer keeps a persistent world keyed by entity handle
and never sees an entity, a component or a ktable. The module never includes a graphics
API.

## How it works

**The components.** `Transform2` is a `Placement2`: position, rotation (radians,
counter-clockwise) and scale, with `CompPrev` so the engine keeps last tick's copy.
`Sprite` is the material (a renderer handle), a UV rect, a size in world units, a tint
and a layer. Both carry field tables, so they hash, compare, print and trace like any
component.

**The ktable.** `sprite.entity` is a ktable with exactly those two components, a million
slots of address space, for things that are only sprites: a background, a particle, a
stone in a game that keeps its own structs. `SpriteWorld` is the convenience surface over
that ktable (`create`, `destroy`, typed lookups). A game's own ktables add whatever else they
want next to the two components and are drawn the same way.

**The extract.** The system `sprite.extract` runs in the Frame cycle
(`docs/design/cycles.md`): once before each frame drawn, after every tick that frame
holds and after every Frame-cycle writer of a `Transform2` or `Sprite` column, reading
`*.Transform2` and `*.Sprite`. Per ktable, it
keeps the handle it last sent for each slot and the `Sprite` it last described. Then:

1. A slot whose handle changed or died since the last extract becomes a `Remove` for the
   handle the renderer knows there. No ktable needs a destroy hook.
2. A live slot the renderer has not seen, or whose `Transform2` or `Sprite` differs from
   what was last sent, becomes a `Set` with the whole `RenderSpriteState`. A transform
   that differs from what was sent but equals the engine's previous copy is a teleport:
   the `Set` carries `snap`, and the renderer shows the sprite there at once instead of
   interpolating across the gap.
3. When ticks ran since the last extract, a `Tick` marker, so the renderer knows where
   "previous" ends. A frame with no tick in it has none, and the renderer keeps
   interpolating the last tick. When more than one ran, a sprite that moved first gets a
   `Set` with `snap` at its previous copy (where it was when the last tick began), before
   the marker: what the renderer interpolates is the last tick's motion, as when the
   extract ran every tick.

4. After the `Tick` marker, a `Layer` delta for each layer whose scroll offset changed
   (the resource `sprite.layers`): see layer scroll below.

A ktable declared `still` (`KTableDesc::still`: a map's tiles) is looked at only when it has
spawned, lost or restored a row since the last extract, and then only for its births and
deaths (a restore: one full compare), so a map of any size costs nothing a tick once
sent.

Nothing is copied twice: a sprite that did not move and did not change produces nothing.
The deltas accumulate per frame (several ticks in one frame append in order) and the
frame loop applies and clears them before the render.

**Layer scroll.** `SpriteLayers` (`sprite_layers(engine)`, or the resource `sprite.layers`
for a system that declares the write) holds an offset per layer, world units, zero by
default. The renderer draws every world sprite on that layer moved by it, interpolated
from the last tick's offset like a placement; `set(layer, offset, true)` snaps instead
(a wrap, a restart). `sprite.layers_tick` (PreSim, every tick) keeps each offset as the
tick began (`tick_start`), so a frame holding several ticks sends the layer there first
and interpolates only the last tick's scroll, as for sprites. A parallax plane or a scrolling map is one value a tick however many
rows it has; the rows themselves stay where they were spawned. Screen sprites ignore it.

**Interpolation** is the renderer's, from the previous and current placement of each
sprite it was told about, at the clock's alpha. The extract makes that possible by
sending a `Set` whenever the transform moved, which the `CompPrev` copy detects.

## Using it

**A ktable that is drawn:**

```cpp
constinit const KTableDesc k_ktables[] = {{.name = ".rock", .components = "Transform2, Sprite, Velocity", .capacity = 1024}};

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
| `Sprite` | component | `material` (Handle, null: the default), `uv` (rect2, unit by default), `size` (vec2, 64x64), `tint` (color, white), `layer` (i32, 0), `space` (`SpriteSpace::World` or `Screen`), `anchor` (`ScreenAnchor`, `Center`: where a screen sprite's position is measured from), `sort` (`SpriteSort`, `Row`), `order` (f32, 0) |
| `transform2_schema()`, `sprite_schema()` | functions | the two `ComponentDesc`s, for comparing or printing rows |

Header `korvid/sprite/camera.h`:

| name | kind | what |
|---|---|---|
| `SpriteSort` | enum | `Row` (slot order), `Key` (by `order`), `Feet` (by the feet's y: position + `order`), within a layer |
| `Camera2` | record | `center` (point2), `zoom` (f32, 1: a world unit per pixel), `rotation` (f32 radians), `follow` (an entity), `offset`, `bounds` (rect2; a zero size: no clamp), `pixel_snap`; published as the resource `sprite.camera`, written by whichever system declares it |
| `camera_resolve(e, cam, width, height, alpha)` | function | the camera a frame draws with: follow at the interpolation point, clamp, snap; the frame loop calls it |
| `camera_view(cam, width, height)` | function | the world rect the camera shows in a window that size, before rotation |
| `sprite_camera(engine)` | function | the camera, for code outside the schedule (the frame loop reads it at render rate) |

Header `korvid/sprite/world.h`:

| name | kind | what |
|---|---|---|
| `SpriteWorld` | record | over the `sprite.entity` ktable: `create(t, s)`, `destroy(h)`, `valid(h)`, `live()`, `kind()`, `transform(h)`, `prev_transform(h)`, `sprite(h)` |

Header `korvid/sprite/extract.h`:

| name | kind | what |
|---|---|---|
| `ExtractState`, `ExtractKTable` | records | per ktable: the handle and the `Sprite` last sent, by slot |
| `extract_sprites(engine, state, layers, out, ticks)` | function | the deltas `ticks` ticks after the last extract appended to `out` (catch-up Sets with two or more, a `Tick` with one or more); `layers` may be null |

Header `korvid/sprite/layers.h`:

| name | kind | what |
|---|---|---|
| `SpriteLayers` | record | `set(layer, offset, snap = false)`, `get(layer)`; layers outside [-128, 127] are ignored |
| `KV_SPRITE_LAYER_MIN`, `KV_SPRITE_LAYER_MAX` | constants | -128, 127 |
| `sprite_layers(engine)` | function | the module's layer offsets, or null |

Header `korvid/sprite/module.h`:

| name | kind | what |
|---|---|---|
| `sprite_module` | `ModuleDesc` | the record |
| `sprite_world(engine)` | function | the module's world, or null |
| `sprite_deltas(engine)` | function | the deltas extracted since the last take; apply and clear |
| `sprite_rows_hint(engine, rows)` | function | the rows the game expects: the extract sizes its tables once (the frame loop calls it) |

From the render interface (`korvid/render/sprite.h`): `Placement2`, `RenderSpriteState`,
`SpriteDelta{kind, entity, state}`, `SpriteDeltaKind` (`Tick`, `Set`, `Remove`, `Layer`:
`state.layer`'s offset in `state.placement.position`, `state.snap`, a null entity).

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `Transform2` | component | 20 bytes, 3 fields, `CompPrev`, sim |
| `Sprite` | component | 64 bytes, 8 fields, sim |
| `sprite.entity` | ktable | `Transform2`, `Sprite`; 1 M slots of address space |
| `sprite.deltas` | resource | the delta array; the extract writes it |
| `sprite.layers` | resource | the layer offsets; a game's scroll writes it, the extract reads what changed |
| `sprite.layers_tick` | system, phase PreSim, `Always` | writes `sprite.layers`: the offsets as each tick begins |
| `sprite.extract` | system, phase Frame | reads `*.Transform2`, `*.Sprite`; writes `sprite.deltas`, `sprite.layers` |

## Behaviour worth knowing

- **Both components are sim state.** They are hashed; a game whose sprites are cosmetic
  only still hashes their placement. Debris that must not affect the hash goes in a
  ktable with its own cosmetic components and the two sim ones.
- **A `Set` is the whole state.** The renderer does not merge fields; the extract sends
  everything when anything changed. Changing a tint every tick costs the same as moving.
- **The previous copy is the engine's, taken at the top of PreSim.** A transform written
  twice in one tick (moved, then wrapped) is one delta with the final value, and the
  renderer interpolates from where the tick began to where it ended: a wrap across the
  screen would sweep across it for one frame. **To teleport, write the previous copy
  too** (`k.prev<Transform2>()[i] = t[i]` in a system holding the write): the extract
  sees a transform that moved without a tick's worth of travel behind it and sends the
  `Set` with `snap`. The rocks `wrap` does this.
- **Rotation interpolates as a scalar.** Keep angles continuous.
- **Layer is draw order; `sort` orders within it.** Equal layers draw in slot order,
  stable frame to frame, unless a sprite asks otherwise: `SpriteSort::Key` sorts by
  `order`, `SpriteSort::Feet` by the feet's height (the interpolated position's y plus
  `order`), lower on screen in front, computed on the GPU at the same interpolation point
  the sprite is drawn at, so two characters swap the frame they cross. Ties keep slot
  order. Only buckets (layer and blend) holding keyed sprites pay for the extra pass, and
  it orders up to 2,048 visible sprites per bucket. A layer that mixes blend modes sorts
  within each mode: keep a sorted layer to one blend. The renderer clamps layers to
  [-128, 127]; screen-space and world sprites share the order, so a HUD is a high layer.
- **Screen space is window pixels**, +y up, whatever the camera does, measured from the
  sprite's `anchor`: the window's center by default, or a corner or an edge
  (`ScreenAnchor::TopRight` and so on), which the renderer resolves against the window's
  size as it draws. A HUD anchored to a corner stays there on any window, and its
  position, which is sim state, never holds the window's size. `size` is in pixels. The
  platform's `mouse_screen` puts the pointer in the center-anchored space.
- **The camera is read at render rate.** A camera a system moves to follow an entity
  lags it by the frame's alpha and the followed sprite jitters; set `follow` instead and
  the frame loop centers on the entity's interpolated position at the alpha the sprites
  use, so it stands still on screen. Then `bounds` clamps and `pixel_snap` puts the view's
  corner on a world pixel, which keeps pixel art from shimmering at an integer zoom.
- **The extract runs whatever the sim does.** It is in the Frame cycle, so menus and
  overlays drawn while the sim holds still reach the renderer, and a frame that held no
  tick costs it only the compare.
- **The extract is one thread** and linear in the rows of every drawable ktable, dead
  slots included; a ktable with a huge capacity and few live rows still costs its
  high-water mark. A `still` ktable is the exception: nothing, until it spawns or loses a row.
  Its cost is about 50 to 100 ns a delta in a dev build, linear.
- **Its tables grow, unless told the size.** The side tables (by slot: the handle, the
  `Sprite` and the `Transform2` last sent) and the delta list grow as a ktable's high-water
  mark and a frame's deltas climb; each growth copies the table, megabytes in one tick at
  stress (12 ms in a dev build, which fills new memory). `sprite_rows_hint(e, rows)` (the
  frame loop passes `FrameLoopConfig::sprite_rows`) sizes a ktable's side tables at its first
  growth to its capacity or `rows`, the smaller, and the delta list to twice `rows` at the
  first extract, so they do not grow mid-run.
- **A still ktable's rows cannot move.** The extract does not look at a living row of a
  still ktable, so a write to one is never drawn (and its previous copy is not retaken).
  Scroll it by layer; to change one tile, destroy it and spawn another.
- **A layer's offset is per frame, not per sprite.** Everything on the layer moves
  together, sprites spawned this tick included, and the camera applies on top.

## Not in this module

Animation (frames are a UV rect the game writes), sprite sheets and atlases, text,
parenting and hierarchies, z-sorting beyond the layer, render-rate entities, tile maps,
and anything the renderer does with the deltas (`docs/design/renderer.md`).
