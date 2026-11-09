// Korvid tilemap: Tiled maps as assets, drawn as sprites. tilemap_load(e, "game/town.tmj",
// scope) returns a tilemap.map handle at once; the map file and any external tilesets
// read as blobs, the tileset images as textures, and "tilemap.pump" turns the record Ready
// (AssetReady) when all of them have settled. The record lives in the asset module's
// scopes like any asset. tilemap_spawn then makes an instance: one cosmetic sprite row
// per tile and per tile object, in Tiled's layer order.
//
// The format is Tiled's JSON (.tmj maps, .tsj tilesets), what Tiled 1.8 and later save:
// orthogonal, finite, right-down. Tile layers as csv arrays or base64 (uncompressed, zlib
// or gzip); global tile ids with the horizontal, vertical and diagonal flip bits; group
// layers flattened (their offsets, opacity and visibility applied to what they hold);
// object layers with points, rectangles, ellipses and tile objects; tilesets embedded or
// external, one image each, with per-tile properties, collision rectangles and
// animations; custom properties of every simple type on the map, layers, tilesets,
// tiles and objects. Coordinates: a Tiled pixel (x, y) with y down is the world point
// origin + (x, -y), one world unit per pixel.
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/math/color.h"
#include "korvid/core/memory.h"
#include "korvid/math/rect2.h"

namespace kv {

struct TickContext;

enum class MapLayerKind : u8 { Tiles, Objects, Image };
enum class MapPropType : u8 { String, Int, Float, Bool, Color, File, Object };

constexpr u32 KV_TILE_FLIP_H = 0x80000000u, KV_TILE_FLIP_V = 0x40000000u, KV_TILE_FLIP_D = 0x20000000u;
constexpr u32 KV_TILE_GID_MASK = 0x0FFFFFFFu; // (Tiled's hexagonal rotation bit, 0x10000000, is masked off)

struct MapProperty {
    const char* name;
    MapPropType type;
    const char* str;   // String and File: the value; the rest: ""
    f64         num;   // Int, Float, Object (its id), Bool (0 or 1)
    color       col;   // Color, linear from Tiled's #AARRGGBB
};

// A range of a map's flat arrays (properties, objects, rectangles, frames).
struct MapRange { u32 first = 0, count = 0; };

struct MapObject {
    u32         id;
    const char* name;
    const char* cls;      // Tiled's class (or the older "type")
    f32         x, y, width, height, rotation; // Tiled pixels; a tile object's (x, y) is its bottom-left
    u32         gid;      // a tile object's tile, flip bits included; 0 for shapes
    bool        point, ellipse;
    u32         layer;    // the index of the object layer it is in
    MapRange    props;
};

struct MapLayer {
    MapLayerKind kind;
    const char*  name;
    u32          id;
    bool         visible;
    f32          opacity;    // after any groups it was in
    color        tint;       // likewise
    vec2         offset;     // Tiled pixels, likewise
    u32          width, height; // tile layers: cells
    const u32*   gids;       // tile layers: width * height, row-major from the top, flip bits included
    MapRange     objects;    // object layers
    MapRange     props;
};

struct MapTile {           // a tileset's tile that has something (properties, shapes, frames)
    u32      local;          // its id within the tileset
    MapRange props;
    MapRange rects;          // collision rectangles, tile pixels, y down from the tile's top-left
    MapRange frames;         // its animation
};

struct MapFrame { u32 local; f32 seconds; };

struct MapTileset {
    const char* name;
    const char* image;       // the image's asset path, resolved against the tileset's file
    u32         first_gid, count, columns;
    u32         tile_w, tile_h, margin, spacing, image_w, image_h;
    MapRange    tiles;       // into the map's tiles, ascending by local id
    MapRange    props;
    Handle      texture;     // asset.texture
    Handle      material;    // render.material: the image, alpha blend; made when the texture settles
};

struct TilemapAsset {
    char        path[KV_ASSET_PATH_MAX];
    AssetState  state = AssetState::Pending;
    AssetError  error = AssetError::None;
    u32         refs  = 0;
    u32         width = 0, height = 0, tile_w = 0, tile_h = 0;
    MapLayer*   layers   = nullptr;  u32 layer_count   = 0;
    MapObject*  objects  = nullptr;  u32 object_count  = 0;
    MapTileset* tilesets = nullptr;  u32 tileset_count = 0;
    MapTile*    tiles    = nullptr;  u32 tile_count    = 0;
    MapProperty* props   = nullptr;  u32 prop_count    = 0;
    rect2*      rects    = nullptr;  u32 rect_count    = 0;
    MapFrame*   frames   = nullptr;  u32 frame_count   = 0;
    MapRange    map_props;
    // What the load is waiting on, and the arena everything above lives in.
    Handle      blob;                // the map file, until parsed
    u32         stage = 0;
    void*       mem   = nullptr;
    // Solids, built when Ready (see tilemap_solid).
    u8*         solid = nullptr;     // width * height: a whole cell is solid
    rect2*      solid_rects = nullptr; u32 solid_rect_count = 0; // map pixels, y down: tile shapes placed, solid objects
    char        error_text[160] = {}; // a parse error, with the JSON position
};

// Loads a Tiled map into a scope (null: the caller holds it with asset_ref). The same
// path is the same handle.
Handle              tilemap_load(Engine& e, const char* path, Handle scope);
const TilemapAsset* tilemap_get(Engine& e, Handle map);

// Placement of an instance: the map's top-left corner in world units, the sprite layer of
// its first layer (Tiled's layer i draws at base_layer + i), and an entity whose death
// despawns the instance (a level's root; null: only tilemap_despawn does).
struct TilemapPlace {
    point2 origin;
    i32    base_layer = 0;
    Handle owner;
};

// Spawns the tile and tile-object rows of a Ready map into "tilemap.tile" (declare it in
// your writes). Returns the instance (tilemap.instance), or null if the map is not Ready.
Handle tilemap_spawn(TickContext& ctx, Handle map, const TilemapPlace& place);
bool   tilemap_despawn(TickContext& ctx, Handle instance);
u32    tilemap_instance_rows(Engine& e, Handle instance);

// Queries over a Ready map. Cells are (x, y) from the top-left; -1 outside.
const MapLayer*    tilemap_layer_find(const TilemapAsset& m, const char* name, u32* index = nullptr);
const MapObject*   tilemap_object_find(const TilemapAsset& m, const char* name);
u32                tilemap_gid_at(const TilemapAsset& m, u32 layer, i32 x, i32 y);
const MapTileset*  tilemap_tileset_of(const TilemapAsset& m, u32 gid, u32* local);
const MapTile*     tilemap_tile_info(const TilemapAsset& m, u32 gid);
const MapProperty* tilemap_property(const TilemapAsset& m, MapRange props, const char* name);
const MapProperty* tilemap_map_property(const TilemapAsset& m, const char* name);
const MapProperty* tilemap_tile_property(const TilemapAsset& m, u32 gid, const char* name);
// A name for what a game spawns from an object: its Tiled name when no other object of the
// map has that name, otherwise "<map stem>#<object id>" ("village#12"), which Tiled keeps
// for the object's life. Stable across runs and edits that leave the object alone.
// Returns what snprintf would.
int                tilemap_object_name(const TilemapAsset& m, const MapObject& o, char* out, usize cap);
bool               tilemap_cell_of(const TilemapAsset& m, point2 origin, point2 world, i32* x, i32* y);
point2             tilemap_cell_center(const TilemapAsset& m, point2 origin, i32 x, i32 y);
point2             tilemap_to_world(point2 origin, f32 map_x, f32 map_y);
// A whole cell is solid (any tile in it has bool `solid`, or it is non-empty in a layer
// whose bool `collision` is true). Outside the map is solid.
bool               tilemap_solid(const TilemapAsset& m, i32 x, i32 y);

// Moving a box through the solids. `box` is world units (min corner, +y up) for a map
// placed at `origin`; `delta` the move this tick. x first, then y, each axis swept against
// the solid cells, the solid rectangles and the map's edges in the box's path, so a box
// slides along a wall it meets at an angle and nothing tunnels at any speed. Boxes are
// half-open: touching a wall is not overlapping it. A stopped box ends exactly on the
// wall's edge (the wall's coordinate minus the box's size, nothing added), so set your
// position from `box`, not by adding `moved`. A box that starts overlapping a solid is
// not pushed out: it may move along or away from it, not deeper, and `started_inside`
// says so.
struct TileMove {
    rect2 box;              // where the box ended, world units
    vec2  moved;            // box - the box given, for information
    bool  hit_x = false, hit_y = false;
    bool  started_inside = false;
};
TileMove tilemap_move(const TilemapAsset& m, point2 origin, rect2 box, vec2 delta);

// The parser on its own, for tools and tests: a map's JSON into `out` (allocating from
// `alloc`, which must outlive it); external tilesets named, not read (`tilesets[i].image`
// is null and `name` holds the resolved .tsj path until tilemap_parse_tileset fills it).
// `base` is the map's own asset path, for resolving what it names.
bool tilemap_parse_map(const char* json, usize len, const char* base, Allocator* alloc, TilemapAsset& out, char* error, usize cap);
bool tilemap_parse_tileset(const char* json, usize len, const char* base, Allocator* alloc, TilemapAsset& map, u32 index, char* error, usize cap);
// Joins a path relative to the file `base` names, resolving "." and "..": ("a/b/map.tmj",
// "../t/x.tsj") is "a/t/x.tsj". False if it climbs out of the root.
bool tilemap_resolve(const char* base, const char* rel, char* out, usize cap);

} // namespace kv
