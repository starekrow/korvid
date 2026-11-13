// The parser over Tiled-saved JSON: layers in order with groups flattened, every data
// encoding, objects, properties, embedded and external tilesets, paths resolved; the
// queries; the solids with flips; and every refusal with a reason.
#include "korvid/tilemap/tilemap.h"
#include "korvid/core/arena.h"
#include "korvid/core/test.h"
#include "fixtures.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv { void tilemap_build_solids(TilemapAsset& m, Allocator* a); }

namespace {

using namespace kv;

bool near(f32 a, f32 b) { return kv::fabs(a - b) < 1e-4f; }

bool parse(const char* json, TilemapAsset& m, Arena& ar, char* err, usize cap) {
    m = TilemapAsset{};
    return tilemap_parse_map(json, strlen(json), "game/maps/town.tmj", &ar.a, m, err, cap);
}

bool has_rect(const TilemapAsset& m, rect2 r) {
    for (u32 i = 0; i < m.solid_rect_count; ++i) {
        const rect2& s = m.solid_rects[i];
        if (near(s.x, r.x) && near(s.y, r.y) && near(s.w, r.w) && near(s.h, r.h)) return true;
    }
    return false;
}

} // namespace

void test_tilemap_parse(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tilemap parse");
    Arena ar;
    KV_CHECK(c, arena_create(ar, 16u << 20, "test", MemTag{}));
    char err[200] = {};

    // Paths.
    char path[128];
    KV_CHECK(c, tilemap_resolve("a/b/map.tmj", "../t/x.tsj", path, sizeof path) && strcmp(path, "a/t/x.tsj") == 0);
    KV_CHECK(c, tilemap_resolve("map.tmj", "./x.png", path, sizeof path) && strcmp(path, "x.png") == 0);
    KV_CHECK(c, !tilemap_resolve("a/map.tmj", "../../x.png", path, sizeof path));

    TilemapAsset m;
    KV_CHECK(c, parse(k_map, m, ar, err, sizeof err));
    KV_CHECK(c, m.width == 4 && m.height == 3 && m.tile_w == 16 && m.tile_h == 16);
    // Layers: the group's layer flattened in place with its offset, opacity and tint.
    KV_CHECK(c, m.layer_count == 6);
    u32 li = 99;
    const MapLayer* detail = tilemap_layer_find(m, "detail", &li);
    KV_CHECK(c, detail && li == 1 && near(detail->offset.x, 2) && near(detail->offset.y, 4) && near(detail->opacity, 0.25f));
    KV_CHECK(c, near(detail->tint.r, 1.0f) && near(detail->tint.g, 0.0f));
    KV_CHECK(c, tilemap_gid_at(m, li, 1, 1) == (13 | KV_TILE_FLIP_H) && tilemap_gid_at(m, li, 2, 1) == (13 | KV_TILE_FLIP_D));
    KV_CHECK(c, tilemap_gid_at(m, 0, 3, 2) == 12 && tilemap_gid_at(m, 0, 4, 0) == 0 && tilemap_gid_at(m, 0, -1, 0) == 0);
    const MapLayer* gz = tilemap_layer_find(m, "gz");
    KV_CHECK(c, gz && !gz->visible && gz->gids[11] == 12 && gz->gids[0] == 1);
    const MapLayer* walls = tilemap_layer_find(m, "walls");
    KV_CHECK(c, walls && walls->gids[8] == 21 && walls->gids[10] == 0 && tilemap_property(m, walls->props, "collision")->num == 1);
    KV_CHECK(c, m.layers[4].kind == MapLayerKind::Objects && m.layers[5].kind == MapLayerKind::Image);
    // Objects.
    KV_CHECK(c, m.object_count == 4 && m.layers[4].objects.count == 4);
    const MapObject* start = tilemap_object_find(m, "player_start");
    const MapObject* door = tilemap_object_find(m, "door");
    const MapObject* tree = tilemap_object_find(m, "tree");
    KV_CHECK(c, start && start->point && near(start->x, 24) && near(start->y, 40));
    KV_CHECK(c, door && strcmp(door->cls, "exit") == 0 && strcmp(tilemap_property(m, door->props, "target")->str, "field") == 0);
    KV_CHECK(c, tree && strcmp(tree->cls, "prop") == 0 && tree->gid == (14 | KV_TILE_FLIP_H) && tree->layer == 4);
    KV_CHECK(c, tilemap_object_find(m, "pond")->ellipse && !tilemap_object_find(m, "nobody"));
    // Map properties, every type.
    KV_CHECK(c, tilemap_map_property(m, "music")->type == MapPropType::File && strcmp(tilemap_map_property(m, "music")->str, "../music/town.ogg") == 0);
    KV_CHECK(c, tilemap_map_property(m, "dark")->num == 1 && tilemap_map_property(m, "level")->num == 3 && tilemap_map_property(m, "gravity")->num == 0.5);
    KV_CHECK(c, tilemap_map_property(m, "boss")->type == MapPropType::Object && tilemap_map_property(m, "boss")->num == 4);
    KV_CHECK(c, strcmp(tilemap_map_property(m, "title")->str, "Caf\xc3\xa9 Town") == 0);
    const MapProperty* sky = tilemap_map_property(m, "sky");
    KV_CHECK(c, sky && sky->type == MapPropType::Color && near(sky->col.a, 1.0f) && sky->col.r < sky->col.b); // #ff336699: r 0x33 < b 0x99
    KV_CHECK(c, tilemap_map_property(m, "cfg") && !tilemap_map_property(m, "none"));
    // Tilesets: the external one named by its resolved path, the embedded one complete.
    KV_CHECK(c, m.tileset_count == 2 && m.tilesets[0].image == nullptr && strcmp(m.tilesets[0].name, "game/tiles/town.tsj") == 0);
    KV_CHECK(c, strcmp(m.tilesets[1].image, "game/tiles/walls.png") == 0 && m.tilesets[1].first_gid == 21 && m.tilesets[1].columns == 2);
    KV_CHECK(c, tilemap_parse_tileset(k_tileset, strlen(k_tileset), m.tilesets[0].name, &ar.a, m, 0, err, sizeof err));
    const MapTileset& town = m.tilesets[0];
    KV_CHECK(c, strcmp(town.image, "game/tiles/town.png") == 0 && town.count == 20 && town.columns == 5 && town.tiles.count == 3);
    KV_CHECK(c, strcmp(tilemap_property(m, town.props, "theme")->str, "spring") == 0);
    u32 local = 0;
    KV_CHECK(c, tilemap_tileset_of(m, 13 | KV_TILE_FLIP_H, &local) == &m.tilesets[0] && local == 12);
    KV_CHECK(c, tilemap_tileset_of(m, 22, &local) == &m.tilesets[1] && local == 1 && !tilemap_tileset_of(m, 0, &local));
    KV_CHECK(c, tilemap_tile_property(m, 5, "solid")->num == 1 && tilemap_tile_property(m, 5, "foot")->num == 2 && !tilemap_tile_property(m, 6, "solid"));
    const MapTile* anim = tilemap_tile_info(m, 11);
    KV_CHECK(c, anim && anim->frames.count == 2 && m.frames[anim->frames.first + 1].local == 11 && near(m.frames[anim->frames.first].seconds, 0.25f));
    const MapTile* shaped = tilemap_tile_info(m, 13);
    KV_CHECK(c, shaped && shaped->rects.count == 1 && near(m.rects[shaped->rects.first].y, 8) && near(m.rects[shaped->rects.first].w, 4));

    // Cells and the world: +y up, the map's top-left at the origin.
    point2 origin{100, 50};
    i32 x, y;
    KV_CHECK(c, tilemap_cell_of(m, origin, point2{100 + 17, 50 - 33}, &x, &y) && x == 1 && y == 2);
    KV_CHECK(c, !tilemap_cell_of(m, origin, point2{99, 49}, &x, &y) && x == -1);
    point2 cc = tilemap_cell_center(m, origin, 1, 2);
    KV_CHECK(c, near(cc.x, 124) && near(cc.y, 10));

    // Solids: a whole collision layer, a solid tile, a tile's shape under each flip, a solid
    // tile object; the edge.
    tilemap_build_solids(m, &ar.a);
    KV_CHECK(c, tilemap_solid(m, 0, 2) && tilemap_solid(m, 1, 2) && !tilemap_solid(m, 2, 2) && tilemap_solid(m, 3, 2));
    KV_CHECK(c, tilemap_solid(m, 0, 1) && !tilemap_solid(m, 1, 1) && tilemap_solid(m, -1, 0) && tilemap_solid(m, 4, 0));
    KV_CHECK(c, m.solid_rect_count == 3);
    KV_CHECK(c, has_rect(m, rect2{30, 28, 4, 8}));  // detail (1,1), flipped horizontally, the group's offset
    KV_CHECK(c, has_rect(m, rect2{42, 20, 8, 4}));  // detail (2,1), flipped diagonally
    KV_CHECK(c, has_rect(m, rect2{32, 16, 16, 32})); // the tree: a tile object's y is its bottom

    // Refusals, each with a reason.
    struct Bad { const char* json; const char* says; };
    const Bad bad[] = {
        {R"({"width":2,"height":2,"tilewidth":16,"tileheight":16,"infinite":true,"layers":[]})", "infinite"},
        {R"({"width":2,"height":2,"tilewidth":16,"tileheight":16,"orientation":"isometric","layers":[]})", "isometric"},
        {R"({"width":2,"height":2,"tilewidth":16,"tileheight":16,"tilesets":[{"firstgid":1,"source":"t.tsx"}],"layers":[]})", "TSX"},
        {R"({"width":1,"height":1,"tilewidth":16,"tileheight":16,"layers":[{"type":"tilelayer","name":"z","width":1,"height":1,"encoding":"base64","compression":"zstd","data":"AAAA"}]})", "zstd"},
        {R"({"width":2,"height":1,"tilewidth":16,"tileheight":16,"layers":[{"type":"tilelayer","name":"short","width":2,"height":1,"data":[1]}]})", "1 cells"},
        {R"({"width":2,"height":1,"tilewidth":16,"tileheight":16,"layers":[{"type":"tilelayer","name":"w","width":2,"height":1,"chunks":[]}]})", "chunks"},
        {R"({"width":2,"height":1,"tilewidth":16 "tileheight":16})", "byte"},
        {R"({"width":2,"height":1,"tilewidth":16,"tileheight":16,"layers":[{"type":"objectgroup","name":"o","objects":[{"id":1,"template":"t.tx"}]}]})", "template"},
    };
    for (const Bad& b : bad) {
        err[0] = 0;
        KV_CHECK(c, !parse(b.json, m, ar, err, sizeof err) && strstr(err, b.says));
    }
    arena_destroy(ar);
}
