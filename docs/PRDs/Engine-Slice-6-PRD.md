# Engine Slice 6 — Gamedev QoL

Nov 8, 2026 · @starekrow

## Goal and exit criteria

Slices 4 and 5 made a small game shippable. Slice 6 takes back the work `arcanum` does around the engine: it blocks against villagers with its own loop because the collision module reports a tick late, builds a 120-line menu by hand, writes walk cycles as uv arithmetic, and saves materials by the order it happened to make them in. Each system named here should make that work disappear from the game, and the game grows only where the growth shows what the engine is good at or what it costs: a title screen where the hero is named, dialogue with faces, a hammer that breaks rocks through an aiming minigame or turns a villager into a pursuer at twice the player's speed, a game-over screen, and rocks that stay where they were left.

Six pieces: names for everything a game holds a handle to (entities included), collision that moves bodies and resolves them in the same tick, animation as data (sheets, clips by name, clips written back out), per-entity attachments and property bags, a UI module (a retained tree with layout, rich text, widgets, screens, transitions and text entry, built in code or loaded from JSON), and per-level persistence on top of names. `tilemap_path` comes along for the pursuer.

It is done when:

- Every renderer texture and material, asset record, animation clip, sheet, UI document and scope has a name: its path, the name its descriptor gave, or a derived name (`material:9f2c41d07a6e3b18`) that is a hash of what it is, so it is the same in every run that makes the same thing in any order. A save writes a handle to anything named as `kind@name` and reads it back by name; `Sprite::material` and `AnimPlayer::clip` survive a change to the order a game creates them in.
- An entity can be given a name that is unique among the living, found by name from any thread at any time in a tick, printed and addressed by name in the CLI (`print @old_man`), hashed, saved and restored. Objects spawned from a map are named by their Tiled name or, without one, `<map>#<object id>`. The level's root is named `level.root`, so a save no longer needs a remap for it.
- A kind can declare attachable components and a property bag. An attachment is a component on one entity only, stored sparsely, hashed, saved, destroyed with its entity, and invisible to every engine system: a `Sprite` attached to a label does not draw. A property bag holds named ints, floats, bools, strings and handles; Tiled object properties land in it.
- A kinematic body moves by an intent (`Move2`). `collision.resolve` sweeps every intent in a fixed order against the tile map's solids and every solid and kinematic body as each one stands at that moment, pushes pushable bodies one deep, and writes the result (`MoveResult2`: asked, moved, blocked axes, what blocked it, what it pushed, whether something else moved it) on the same tick, before any system that reads it. The list of bodies something else moved is a resource. Nothing tunnels, touching is not overlapping, a box slides along a row of bodies without snagging between them, and every fence post is a test. Overlap events for sensors come from the resolved positions, in the same tick.
- A sprite sheet (`.sheet.json`) describes a grid, named clips by row, column or cell list, and sets (`walk` by facing); `anim_sheet_load` makes the material and the clips, and arcanum's uv arithmetic goes. Clips live in asset scopes. `anim_write` writes a clip back out as `.anim.json`, and parse after write gives the same clip. Tiled tile animations play.
- The `ui` module draws arcanum's title screen, name entry, pause menu (with save, load, level select and credits), HUD, dialogue box with a faceset and a typewriter, the aiming minigame and the game-over screen, from `.ui.json` files and code, with no game code positioning a label or a sprite. Text wraps, scrolls inside a clipped region and carries inline color, wave, shake and pause effects. Screens push, pop and replace through fade, slide and iris transitions, may hold the sim and take input, and report activations as events by node name. A text field takes UTF-8 typed text (SDL text input on Linux, `WM_CHAR` on Windows) and the demo types into it. `ui reload` reads a changed `.ui.json` without a restart.
- A level remembers its rocks: one pushed and left, or broken, is where it was left (or gone) when the player returns, and after a save and a load. Hostility is level-local: a pursuer forgets when the player leaves the map.
- `tilemap_path` finds the shortest 8-connected path over a map's walkable cells for a body of a given size, without cutting corners, and the pursuer follows it around trees and houses.
- `arcanum --demo --hash` names the hero on the title screen, talks to the old man, pushes a rock and leaves the map and comes back to find it moved, hammers a rock through the minigame and breaks it, hammers a villager and is chased down and killed, gets up from the game-over screen into the save, and matches across runs, thread counts, I/O backends and pack, zip and loose files.
- `rocks --demo --hash` still matches; validation stays clean; the leak report is empty; every changed module's README and header docs say what changed; `docs/design/arcanum-notes.md` lists what the game still works around, with every tick of latency left between cause and effect.

## Names

**Issuers name their handles (R1).** A handle kind's descriptor gains two optional functions, `name(h) -> const char*` and `find(name) -> handle`, so anything holding a handle can ask what it is called without knowing who issued it, and a loader can turn a name back into today's handle. `kv_handle_name(h)` and `kv_handle_find(kind, name)` are the C calls; `handle_name` and `handle_find` the C++ ones. A kind that does not name its handles answers null.

**Every record has one (R2).** A record made from a file is named by its normalized asset path. A record made from a descriptor takes the descriptor's `name` if it has one. Otherwise it gets a derived name, `<what>:<16 hex digits>`, the engine hash of the descriptor's content: for a renderer material, its texture's name, blend, tint, filter and parameters; for a texture, a creation counter is all there is (`texture:3`), so a game that wants a texture saved names it. Two records with the same content get the same derived name, and finding it gives the first: they are interchangeable by construction. Dev builds log each derived name once so a game can pin it. The names are interned; the issuers keep a map from name to handle and drop the entry when the record goes.

- Renderer textures and materials: `TextureDesc::name` (as now) and a new `MaterialDesc::name`; the backend keeps the names and answers `find`.
- Asset records: textures, materials, fonts (`path@px`), sounds, streams, blobs, by path; scopes by their name.
- Asset-made renderer objects take their asset's name: a texture asset's renderer texture is named by the path, a font's atlas material `font:<path>@<px>`.
- `asset_material_for_texture(e, texture_asset, blend, filter, scope)` makes (or finds) the material for a texture asset, named `<path>:<blend>[:linear]`, held by the scope, released with it. Arcanum's sheets move into level scopes through it.
- Clips, sheets, tile maps and UI documents (below) by path, or `path#clip` for a clip a sheet or a map made.

**Saves use names (R3).** `save_handle` writes `kind@name` for any handle whose issuer names it and `kind#index:gen` for the rest (entities without names). `load_handle` finds `kind@name` through the issuer and warns, reading null, when the name is not there. A remap still runs for every handle, so a game can answer for anything it knows better.

**Entities (R4).** An entity can have a name, unique among living entities across every kind. Names are for scripts, tools, saves and the map's designers, a few lookups a tick, not a per-entity hot path:

- `ctx.name_later(h, "old_man")` names an entity at the next phase boundary (as a spawn lands), `ctx.spawn_named_later(kind, "name", components...)` spawns one with a name, and `engine_entity_name_set(e, h, name)` names one now from code outside the schedule (startup, a CLI command, a tool). A name that a living entity already has is refused with a log line; an empty name removes it.
- `engine_entity_find(e, "old_man")` returns the handle or null, from any thread at any point in a tick: the map changes only at boundaries and outside the schedule, and a destroyed entity's name stops resolving as its handle goes stale, before the boundary sweeps the entry. `engine_entity_name(e, h)` is the reverse. Both are lookups, not scans.
- A name belongs to the entity: destroying it releases the name, the table's hash covers it (a sim kind's names are sim state), `save_kind` writes it with the row and `load_kind` restores it, in a pass over all the kinds being loaded before any row's handles are read, so a handle written by name into another kind's row finds its entity.
- The CLI resolves `@name` wherever it takes an entity, and `ls /names` lists them.
- Handles to named entities are written by name (R3), which is what lets a level's root be named `level.root` by the level module and come back as the new root after a load.

**Enum names (R5).** A field trait for enums with names (`KV_ENUM_NAMES(SpriteSpace, "world", "screen")`): `save_fields` writes the name and reads either the name or the number, and the CLI prints the name. `Sprite::space`, `Sprite::sort`, `Sprite::anchor`, `AnimPlayer::clock` and `state`, `Collider2::shape` and `body` use it.

## Collision

The slice 5 module finds overlaps after the move; a game that wants to stop at something has to do it itself or overlap for a tick. This slice moves bodies through the module.

**Bodies (R6).** `Collider2` gains `offset` (the shape's center from the position: a feet box), `body` and `flags`:

- `Sensor` (the default, today's behaviour): reports overlaps, blocks nothing, is never moved.
- `Solid`: blocks kinematic bodies; never moves on its own; moves when pushed if `ColliderPushable` is set.
- `Kinematic`: moved by its intent, blocked by solids, other kinematic bodies and the static world; blocks other kinematic bodies.

Layers and masks decide who blocks whom exactly as they decide who overlaps whom. Shapes stay circles and axis-aligned boxes; blocking is box against box (a circle blocks and is blocked as its bounding box, documented, since nothing in the examples needs round blocking).

**Intents (R7).** `Move2 { vec2 delta; }` on a kinematic body is how far it wants to go this tick, written by the game in Sim. The system `collision.resolve` (Sim, after every writer of `Move2`, which the scheduler derives from its read set) takes the intents in a fixed order (kinds by name, then slot), and for each one:

1. Sweeps the box along x then y, as `tilemap_move` does, against the static world, then against every solid and kinematic body as it stands now, so the second mover sees where the first one ended up this tick.
2. Against a pushable solid, with `ColliderPusher` on the mover, pushes it: the pushed body sweeps by what the mover needs, at its `pace` (a fraction of the mover's step, 1 by default), against the static world and every other body, pushing nothing itself. The mover goes as far as the pushed body went. A body pushed by several movers in a tick is pushed by each in order.
3. Writes `Transform2` (and `Local2` by the same delta for a child kind, which is right for a child of an unrotated, unscaled parent such as a level's root, and documented as the limit), zeroes the intent, and fills `MoveResult2` if the kind has one: `asked`, `moved`, `hit_x`, `hit_y`, `blocker` (the first body it stopped at, null for the world or nothing), `pushed` (what it pushed), `pushed_by` and `displaced` (it was moved by something else this tick).

`collision.moved` is a published resource listing the bodies that something else moved this tick, in resolve order. A system that reads `MoveResult2` or `collision.moved` runs after the resolve, in the same tick: intent, resolve, react. The overlap broadphase runs after the resolve on the resolved positions, so `OverlapBegin` arrives the same tick as the move that caused it.

**The static world (R8).** The module knows nothing about tile maps. It has one static world slot, a sweep function and a user pointer (`collision_static(e, fn, user)`); the tilemap module installs one for a map with `tilemap_collide(e, map, origin)`, which arcanum calls in its build handler. A sweep against the world follows every rule of slice 5's R23.

**Queries (R9).** `collision_query(ctx, rect, layer_mask, out)` returns the bodies whose boxes overlap a rect, and `collision_cast(ctx, box, delta, mask)` the first body a box would hit on a straight move, both over the bodies as they stand when called. They read the columns through the context, so a system that calls them declares `*.Transform2, *.Collider2` in its reads. The hammer's reach is a query.

**The fence posts (R10).** Tests, as for tilemap_move: touching is not overlapping; a stopped box ends exactly on the blocker's edge; a box slides along a row of boxes without stopping at the seams, both axis orders, both directions; nothing tunnels at any speed against bodies or the world; a body that starts inside another may only move out of it; two movers walking into each other resolve by order and the order is the documented one; a push one deep moves the pushed body by exactly what the mover gained; a pushed body blocked by the world stops the mover; the same world resolves the same on any thread count.

## Animation as data

**Sheets (R11).** A `.sheet.json` names a texture and how it is cut, and the clips in it:

```json
{ "texture": "example_arcanum/actors/NinjaBlue.png", "cell": [16, 16],
  "clips": {
    "walk.down":  { "column": 0, "rows": [0, 1, 2, 3], "fps": 6.67, "loop": "loop" },
    "idle.down":  { "cells": [[0, 0]] },
    "attack.down": { "cells": [[0, 4]], "duration": 0.3, "events": [[0.1, "hit"]] } },
  "sets": { "walk": { "down": "walk.down", "up": "walk.up", "left": "walk.left", "right": "walk.right" } } }
```

`anim_sheet_load(e, path, scope)` reads it as a blob, loads the texture and its material (`asset_material_for_texture`, alpha, nearest) into the same scope, builds each clip as a uv track (plus events), names each `path#clip`, and turns `Ready` when the texture is. `anim_sheet_clip(e, sheet, "walk.down")` and `anim_sheet_variant(e, sheet, "walk", "down")` find clips; `anim_sheet_get(e, sheet)->material` is what a sprite uses and `->cell_size` its size. The sheet is an external asset kind (slice 5's `asset_kind_register`), so it lives and dies with its scope.

**Clips in scopes (R12).** `anim_load` and `anim_create` take a scope like every other load; the old signatures use the global scope. A clip that a sheet or a map makes is held by its maker.

**Writing clips (R13).** `anim_write(clip, SaveWriter&)` writes a clip as `.anim.json` (uv tracks as keys, not as a grid); `anim_parse` of that text builds a clip equal to the first, key for key, which is a test over every channel and loop mode. An `AnimPlayer` saves and loads exactly: its clip by name, its time, rate, state and loop count, so a run continued from a save hashes as the saving run did.

**Tile animations (R14).** The tilemap module makes one clip per animated tile from Tiled's frames (named `map#tile<gid>`) and spawns animated cells into a kind of their own, `tilemap.animtile` (`Transform2, Sprite, AnimPlayer`), so the many still tiles carry no player.

## Attachments and property bags

A kind is a fixed set of components, and that stays the engine's model: it is what makes iteration a walk over columns. Some data belongs to one entity of many: the quest one villager gives, the item one chest holds, the dialogue a map's designer typed on one object.

**Attachments (R15).** A kind may declare `attachable = "Talk, Chest"`. An attachable component on an entity of that kind is stored sparsely in its table: an array of slots in ascending order beside the rows, a lookup by binary search. `ctx.attach(h, Talk{...})`, `ctx.detach<Talk>(h)`, `ctx.attached<const Talk>(h)` (null if the entity has none) under the resource `kind.@Talk` (or `*.@Talk`) in a system's sets; `for (Attached<Talk> a : ctx.attached_all<Talk>("kind"))` walks them in slot order. A write on the whole kind covers its attachments. They are hashed with the table (for a sim kind), written by `save_kind` and restored by `load_kind`, and destroyed with the entity.

Engine systems never see attachments: views are built from a kind's columns, and an attached `Sprite` or `Collider2` is data and nothing more. Attaching a component the kind did not declare attachable is refused (the resource does not exist, so a system cannot name it). A component that is a column of the kind cannot also be attached to it.

**Property bags (R16).** A kind may declare `props = true`. Each entity then has a bag of named values: int (i64), float (f64), bool, string (interned) and handle. `ctx.prop_set(h, "dialogue", "old_man_1")`, `ctx.prop_i64(h, "gold", 0)` and the rest, under `kind.@props`. A bag is slower than a column and untyped by design: it is for what designers type into a map and scripts read a few times a tick. Bags are hashed, saved, restored and destroyed like attachments. `tilemap_spawn_props(ctx, h, map, object)` copies a Tiled object's custom properties into an entity's bag.

## The UI module

**A tree the module owns (R17).** The `ui` module keeps its own retained tree of nodes, in its own state, and turns it into cosmetic sprite rows (`ui.sprite`, screen space) every tick something visible changed; the rows are output, visible to the CLI and the renderer for convenience, never input. Every node has a name and is found by path from its screen (`pause/items/save`). Node types:

- `panel`: a 9-slice frame from a texture and its border insets, or a flat color.
- `image`: a texture rect, scaled to fit, fill or not at all.
- `label`: rich text (R19) with wrapping at the node's width, alignment, line spacing, a drop shadow and a reveal (typewriter).
- `stack`: children in a row or a column with a gap and alignment.
- `list`: a stack whose children are items with focus: navigation with repeat, wrap-around, a cursor glyph or highlight, a sound per move, select and cancel.
- `button`: a focusable item with a label.
- `scroll`: one child larger than itself, clipped to its rect (quads cropped on the CPU with their uv, so the renderer needs nothing new), scrolled by focus, by input or by code.
- `field`: a one-line text input with a caret, a maximum length and an allowed set (R21).
- `meter`: a bar or a row of icons (`pips`) showing a value out of a maximum, bound to the model; the HUD's hearts and the minigame's needle.

**Layout (R18).** Each node has an anchor and a pivot (the nine points), an offset, a size (fixed, fit to content, or fill the parent along an axis), padding and margins. The screen's root is the window in UI units: the window's height divided by the document's design height, rounded down to a whole number when the document says `pixel: true` (arcanum's pixel art), continuous otherwise. Layout runs when the tree or the window changes, not every tick.

**Rich text (R19).** Labels take inline tags: `{c=gold}…{/c}` (named or `#rrggbb` colors from the style), `{wave}…{/wave}`, `{shake}…{/shake}`, `{pause=0.4}`, `{=hp}` (a model value), and `{{` for a brace. The UI lays out glyphs itself from the font's table, so wrapping, clipping and per-glyph effects need nothing from the text module. A reveal shows `rate` characters per second, plays the style's tick sound every few characters, honours pauses, completes at once on `confirm`, and emits `revealed` when done.

**Documents and styles (R20).** A `.ui.json` is a style block (fonts by name and size, colors, frames, sounds) and screens of nodes; `ui_load(e, path, scope)` reads one as an asset, and `ui_build` makes the same tree in code from descriptors. Both give the same nodes, which a test compares. `ui reload` (CLI) reads every loaded document again and rebuilds its open screens, keeping focus by name.

**Input and events (R21).** The UI reads the action map: the document names its `move`, `confirm` and `cancel` actions. The top screen that takes input gets it; the rest see none. Activating a node emits `UiEvent { screen, node, event, value }`, names as interned hashes (`select`, `cancel`, `changed`, `revealed`, `submit`, and any event a document names on a node), in `ui.update` (PreSim, main thread, every tick), so handlers run before that tick's Sim. A field reads typed text from the platform: `InputState` gains the UTF-8 text typed since the last sample (SDL3's text input events, Win32's `WM_CHAR`), started and stopped by the field taking and losing focus; Backspace and Enter edit and submit. The demo injects text with `input_text_override`. UI state is presentation, outside the hash and the save; what the sim needs leaves it as events or a game's own reads of a field.

**The model (R22).** `ui_model_set(e, "hp", 3)` and the string, float and bool forms set named values; labels and meters bound to them update when they change, so a HUD needs no rebuilding code.

**Screens and modes (R23).** `ui_push(e, doc, "pause", UiTransition)`, `ui_pop(e)`, `ui_replace(e, ...)` and `ui_top(e)`. A screen may hold the sim (a new `HoldUi` reason) and may take input (modal). Transitions: cut, fade (a color), slide (from an edge) and iris (a circle closing on a point, an alpha mask texture the module makes at startup). A replace runs out then in, with an `UiCovered` event at the midpoint, when the screen is fully covered, which is where a game switches modes: from the world to the minigame, from the world to the game-over screen, from the title to the village. The level module's own fade stays as it is.

## Persistence and paths

**Per-level state (R24).** `level_persist(e, kinds)` names the kinds whose named entities a level remembers. On `LevelLeaving` the module writes each named entity of those kinds in the level (by `save_fields`, attachments and bag included) into the level's record, and marks as destroyed the names the level had built and no longer has; after the game's `LevelBuild` handlers have spawned from the map, the module destroys what the record says was destroyed and loads the rest over what was spawned, by name. `level_save(w)` and `level_load(doc)` carry every level's record in a save. A level reload by the CLI keeps the record; a game can clear one (`level_forget`).

**Paths (R25).** `tilemap_path(m, origin, from, to, PathQuery{ box, diagonal, max_nodes }, out)` runs A* over the map's cells, a cell walkable when a box of the given size centred in it overlaps no solid (whole cells and solid rectangles), 8-connected with no corner cutting, with octile costs and ties broken by cell order so the path is the same on any run. It returns cell centres in world units, start excluded, goal included, or nothing when there is no path within `max_nodes`.

## The example

`arcanum` gains what shows the slice working; it loses its menu code, its uv arithmetic, its blocking loop and its save remap.

- **Content.** From the Ninja Adventure pack, kept at `~/code/asset-packs/ninja-adventure-assets` for later slices: the hammer, the villagers' facesets, a dialogue frame, a UI frame for menus, hit and break sounds and an alert sound, vendored and credited as in slice 5.
- **Title and name entry.** The game starts on a title screen (`title.ui.json`: the game's name with a wave, New game, Continue when a save exists, Credits, Quit). New game asks the hero's name in a field; the name shows in dialogue and on the game-over screen. An iris opens on the village.
- **Walking.** The player, villagers and rocks are bodies. The player and the villagers are kinematic, the rocks pushable solids at half pace; the walk reads its intent's `MoveResult2` for the bump that costs a heart. Walk cycles come from sheets.
- **Talking.** `confirm` facing a villager within reach (a query) opens a dialogue box: the villager's faceset, name and lines from a `dialogue` property on the map object (the bag), paged, typewritten, with colored and waving words. The old man carries a `Quest` attachment that changes his lines once the player has broken a rock.
- **The hammer.** A new action, `hammer` (key H or X, pad West). Facing a rock within reach, it replaces the world with the aiming minigame through an iris: a needle sweeps a bar with a sweet spot; `confirm` stops it; the closer to the middle, the better the chance (drawn from the sim's random stream, so the demo hashes) that the rock breaks, with a sound and debris. Back in the world either way. Facing a villager, it makes that villager hostile: it shouts (a `{shake}` line), turns red and chases the player along `tilemap_path` at twice the walk speed, a heart per touch, until the player is dead or leaves the map. Hostility is level-local and is not remembered.
- **Game over.** The last heart replaces the world with a game-over screen naming the hero; Continue loads the save (or restarts the village when there is none), Title goes back to the title.
- **Rocks remember.** Rocks are persisted per level (R24): pushed and left, or broken, they stay so across exits and across a save and load.
- **The menu, the HUD and the credits** are UI documents; the credits scroll in a clipped region.
- **Demo.** The script names the hero by typing, talks to the old man, pushes a rock, leaves and returns, hammers a rock (and breaks it with a well-timed stop), hammers a villager and is chased down, continues from the game-over screen, and quits. `--hash` checks it as before.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Handle issuers name handles and find names; `kv_handle_name`, `kv_handle_find` | P0 |
| R2 | Names for renderer textures and materials, asset records, scopes; derived names by content; `asset_material_for_texture` | P0 |
| R3 | Saves write `kind@name` and read it back by name | P0 |
| R4 | Entity names: unique, deferred set, lookup from anywhere, hashed, saved, CLI `@name` and `/names`; `level.root` named | P0 |
| R5 | Enum name traits in saves and the CLI | P1 |
| R6 | Bodies: sensor, solid, kinematic; offset; pushable and pusher flags; pace | P0 |
| R7 | `Move2` intents resolved in order in the same tick; `MoveResult2`; `collision.moved`; overlaps after the resolve | P0 |
| R8 | One static world, installed by the tilemap module | P0 |
| R9 | `collision_query`, `collision_cast` | P0 |
| R10 | The fence posts, as tests | P0 |
| R11 | `.sheet.json`: grid, named clips, sets, the material | P0 |
| R12 | Clips in scopes | P0 |
| R13 | `anim_write` round trip; `AnimPlayer` saves by clip name | P0 |
| R14 | Tile animations play | P1 |
| R15 | Attachable components: sparse, scheduled, hashed, saved, invisible to engine systems | P0 |
| R16 | Property bags; Tiled properties into them | P0 |
| R17 | UI tree owned by the module, emitted as cosmetic sprites; panel, image, label, stack, list, button, scroll, field, meter | P0 |
| R18 | Anchored layout with fit and fill, design height, pixel scaling | P0 |
| R19 | Rich text: wrap, color, wave, shake, pause, model values, reveal | P0 |
| R20 | `.ui.json` documents and styles, `ui_build` in code, `ui reload` | P0 |
| R21 | Input through actions, focus, `UiEvent`; platform text input on SDL3 and Win32 | P0 |
| R22 | The model and bindings | P0 |
| R23 | Screen stack, `HoldUi`, fade, slide and iris transitions, `UiCovered` | P0 |
| R24 | Per-level persistence of named entities; in saves | P0 |
| R25 | `tilemap_path` | P0 |

## Milestones

Each leaves arcanum building, running and hashing.

1. **Names** (R1–R5): the handle registry, renderer and asset names, entity names, saves by name, `level.root`.
2. **Animation data** (R11–R14): sheets, scoped clips, `anim_write`, tile animations; arcanum's walk cycles from sheets.
3. **Collision** (R6–R10): bodies, intents, the resolve, the static world, queries; arcanum's player, villagers and rocks as bodies, its blocking loop gone.
4. **Attachments and bags** (R15, R16).
5. **UI core** (R17–R20, R22): the tree, layout, rich text, documents; arcanum's menu, credits and HUD.
6. **UI screens** (R21, R23): input and events, text entry, the screen stack and transitions; the title, name entry and dialogue.
7. **The hammer** (R24, R25): persistence, paths, the minigame, the pursuer, game over, the demo.
