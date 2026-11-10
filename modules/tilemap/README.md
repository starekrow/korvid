# tilemap

Tiled maps as assets, drawn as sprites. A game loads a `.tmj` into an asset scope like any
asset, spawns an instance of it when it is Ready, and asks it questions: which tile is at
a cell, what a layer, an object or a tile says in its properties, where the player
starts, which cells are solid.

## How it works

**Loading.** `tilemap_load(e, path, scope)` returns a `tilemap.map` handle at once and
registers nothing new with the asset module but a kind: tile maps live in the asset
module's scopes (`asset_kind_register`), so a level's scope holds its map, readiness
waits for it, and releasing the scope retires it. `tilemap.pump` (PreSim, main thread,
after the asset pumps) moves each pending map through its stages:

1. the map file arrives as a blob and is parsed (`tilemap_parse_map`) into the record's
   own arena: layers, cells, objects, tilesets, properties;
2. each external tileset (`.tsj`) arrives as a blob and is parsed into the same record;
3. each tileset's image loads as a texture; when they have all settled, a material per
   tileset (alpha blend) is made and the solids are built;
4. the blobs are let go (their bytes are no longer needed) and the record turns Ready
   with `AssetReady`.

The map holds references on its textures (the asset module's dependency counts), so two
maps sharing a tileset image share one texture. Retiring a map destroys its materials,
drops its textures, frees its arena, and despawns its instances.

**Instances.** `tilemap_spawn(ctx, map, TilemapPlace{origin, base_layer, owner, scale})` makes a
sprite row in the cosmetic, still kind `tilemap.tile` for every non-empty cell of every visible
tile layer and every tile object, in Tiled's layer order: layer `i` draws at sprite layer
`base_layer + i`, so the index of an object layer is where a game puts its characters to
draw between the layers below and above. A tile larger than the grid sits on its cell's
bottom-left, as Tiled draws it; a tile object's `(x, y)` is its bottom-left. Flips are uv
flips plus, for the diagonal flag, a quarter turn. A layer with the bool property `ysort`
sorts its rows by their feet (`SpriteSort::Feet`): a tile's bottom edge, raised by the
tile's `foot` property in pixels, so a character drawn in the same sprite layer passes in
front of and behind trees, fences and houses as it walks. Opacity and tint (through groups) go
into the sprite's tint. `scale` is world units a map pixel (2 draws a 16 px tileset as
32-unit tiles); it applies to the spawned rows only, and the collision, path and cell
helpers stay in the map's own pixels. Tiles never move (the kind is `still`: a write to
one is not drawn), so after their first frame they cost the extract nothing at all; a map
that scrolls does it by layer (`sprite/layers.h`), one offset per layer a tick. An instance goes when its owner entity dies (a level's root)
or its map retires, at the next pump, before anything is drawn.

**Solids.** Built once per map: a cell is solid if any tile in it has the bool property
`solid`, or it is non-empty in a layer whose bool property `collision` is true; the
collision rectangles drawn in Tiled's tile collision editor are placed in every cell that
uses the tile, flips applied; objects with `solid` add their rectangles. Outside the map
is solid. `tilemap_solid(m, x, y)` answers for whole cells; the rectangles are in
`solid_rects` (map pixels, y down).

**Moving a box.** `tilemap_move(m, origin, box, delta)` sweeps an axis-aligned box (world
units, min corner) through the solids, x first, then y, so a box sliding along a wall it
meets at an angle keeps going, and nothing tunnels at any speed. Boxes are half-open:
touching a wall is not overlapping it, and a box may rest against one forever. A stopped
box ends exactly on the wall's edge (the wall's coordinate minus the box's size), so the
caller sets its position from the returned box; pushing into a wall for a thousand ticks
drifts nothing. Moving along a wall made of many tiles never catches on their seams. A box
that starts overlapping a solid (spawned in a wall) is not pushed out: it may move along or
away, not deeper, and `started_inside` says so. Each of these is a test.

**Coordinates.** A Tiled pixel `(x, y)` (y down from the map's top-left) is the world
point `origin + (x, -y)`: one world unit per pixel, +y up as everywhere in the engine.

## Using it

```cpp
void on_loading(TickContext& ctx, Span<const LevelLoading> ls) { ... }       // the level module loads the map

void on_build(TickContext& ctx, Span<const LevelBuild> bs) {                  // declare writes "tilemap.tile"
    const TilemapAsset* m = tilemap_get(*ctx.engine, bs[0].map);
    TilemapPlace place;
    place.base_layer = 0;
    place.owner      = bs[0].root;                                            // goes with the level
    tilemap_spawn(ctx, bs[0].map, place);
    const MapObject* start = tilemap_object_find(*m, "player_start");
    point2 p = tilemap_to_world(place.origin, start->x, start->y);
    const MapProperty* music = tilemap_map_property(*m, "music");
}
```

Save maps from Tiled as JSON (`.tmj`) with external tilesets as `.tsj`, or embedded. Tile
layer format: CSV, Base64 (uncompressed), Base64 (zlib) or Base64 (gzip); not zstd.

## Quick reference

Header `korvid/tilemap/tilemap.h`:

| name | kind | what |
|---|---|---|
| `TilemapAsset` | record | size, tile size; `layers`, `objects`, `tilesets`, `tiles`, `props`, `rects`, `frames` (flat arrays, `MapRange` into them); `map_props`; `solid`, `solid_rects`; `error_text` |
| `MapLayer`, `MapLayerKind` | record, enum | `Tiles` (`width`, `height`, `gids` with flip bits), `Objects` (`objects`), `Image`; `visible`, `opacity`, `tint`, `offset` after groups; `props` |
| `MapObject` | record | `id`, `name`, `cls`, `x`, `y`, `width`, `height`, `rotation`, `gid`, `point`, `ellipse`, `layer`, `props` |
| `MapTileset`, `MapTile`, `MapFrame` | records | tileset geometry, image path, `texture`, `material`; per-tile properties, collision rects, animation frames |
| `MapProperty`, `MapPropType` | record, enum | `String`, `Int`, `Float`, `Bool`, `Color` (linear), `File`, `Object` |
| `KV_TILE_FLIP_H`, `_V`, `_D`, `KV_TILE_GID_MASK` | constants | Tiled's flag bits |
| `tilemap_load(e, path, scope)`, `tilemap_get(e, h)` | functions | the asset |
| `TilemapPlace`, `tilemap_spawn(ctx, map, place)`, `tilemap_despawn`, `tilemap_instance_rows` | record, functions | instances |
| `tilemap_layer_find`, `tilemap_object_find`, `tilemap_gid_at`, `tilemap_tileset_of`, `tilemap_tile_info` | functions | lookups |
| `tilemap_property`, `tilemap_map_property`, `tilemap_tile_property` | functions | properties by name |
| `tilemap_cell_of`, `tilemap_cell_center`, `tilemap_to_world`, `tilemap_solid` | functions | cells and the world |
| `TileMove`, `tilemap_move(m, origin, box, delta)` | record, function | a box swept through the solids: `box`, `moved`, `hit_x`, `hit_y`, `started_inside` |
| `tilemap_parse_map`, `tilemap_parse_tileset`, `tilemap_resolve` | functions | the parser and its path rule, for tools and tests |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `tilemap.map`, `tilemap.instance` | handle kinds | the map is also an asset kind of the asset module's |
| `tilemap.tile` | kind, cosmetic, still | `Transform2`, `Sprite`; 262,144 rows of address space |
| `tilemap.pump` | system, PreSim, main thread, `Always`, after the asset pumps | writes `tilemap.tile` |

## Animated tiles

A tile with a Tiled animation gets a clip when the map turns Ready (`<map>#tile<gid>`, its
frames as uv keys, looping), held by the map. `tilemap_spawn` puts its cells in a kind of
their own, `tilemap.animtile` (`Transform2, Sprite, AnimPlayer`), so the many still tiles
carry no player; declare `tilemap.animtile` in the writes of whatever spawns a map that has
animated tiles. A flipped animated tile plays its frames unflipped.

## What a designer typed

`tilemap_spawn_props(ctx, entity, map, object)` copies the object's custom properties into
the entity's property bag (core's per-entity bags; its kind declares `props`), so a
designer's `dialogue` or `gold` on a map object is `ctx.prop_str(h, "dialogue")` at run
time.

## Naming what you spawn

`tilemap_object_name(map, object, out, cap)` gives an object a name for the entity a game
spawns from it: its Tiled name when it is the only object with that name, otherwise
`<map stem>#<object id>` (`village#12`), which Tiled keeps for the object's life. Pass it
to `engine_entity_name_set` and saves, the CLI (`print @village#12`) and per-level state
can find the entity again.

## Paths

`tilemap_path(map, origin, from, to, PathQuery{box, diagonal, max_nodes}, out)` is A* over the
map's cells for a box that must fit where it stands (whole solid cells and solid rectangles
count), 8-connected without cutting corners (or 4), octile costs, ties broken by cell order
so the same question gets the same path in every run. It appends cell centres in world
units, the goal's last. Arcanum's pursuer asks for one every 0.3 s and walks to its first
waypoint.

## Behaviour worth knowing

- **Paths resolve against the file that names them.** A map's tileset `"../tiles/a.tsj"`
  and a tileset's image `"a.png"` become asset paths; one that climbs out of the asset
  root fails the map.
- **A tileset image that fails** leaves its tiles showing the placeholder; the map is
  still Ready, with a warning.
- **Tile animations are parsed, not played.** The frames are in the record; a game (or a
  later version of this module) drives them with clips.
- **Object rotation is not applied** to tile objects' sprites or solid rectangles.
- **Row per tile.** A 128x128 map with three full layers is 49,152 rows. Fine for the
  maps a JRPG has, and since the kind is still they cost nothing a tick once drawn
  (arcfire's Shoreline: 15,000 tiles, an extract of 0.002 ms a frame); the renderer still
  culls and draws every row, and a GPU tile layer is the upgrade for much larger ones.
- **Moving a map is moving its layers.** The rows cannot move; `SpriteLayers::set` on
  `base_layer + i` scrolls layer `i`.

## Not in this module

TMX (XML), infinite maps, isometric and hexagonal maps, image-collection tilesets,
templates, Wang sets, parallax and image layers drawn, zstd layers, polygon collision,
and collision response beyond `tilemap_move` (pushing, sliding off corners, slopes).
