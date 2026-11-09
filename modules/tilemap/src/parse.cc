// Tiled's JSON, into a TilemapAsset. Everything the record points at comes from the
// allocator the caller gives (the map's arena): strings, layers, cells, objects,
// tilesets, tiles, properties, rectangles, frames.
#include "korvid/tilemap/tilemap.h"
#include "korvid/core/sort.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/json.h"
#include "korvid/core/memory.h"
#include "korvid/inflate/inflate.h"
#include "korvid/pack/zip.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct Parse {
    Allocator*  a;
    char*       error;
    usize       cap;
    const char* base;
    const JsonDoc* doc;
    bool fail(const char* fmt, ...) KV_PRINTF(2, 3) {
        va_list args;
        va_start(args, fmt);
        if (cap) vsnprintf(error, cap, fmt, args);
        va_end(args);
        return false;
    }
};

const char* dup(Allocator* a, const char* s) {
    if (!s) s = "";
    usize n = strlen(s);
    char* d = (char*)alloc_in(a, n + 1);
    memcpy(d, s, n + 1);
    return d;
}

// Grows a record array by one (the old storage stays in the arena: it is never freed).
template <class T>
T& append(Allocator* a, T*& arr, u32& count) {
    // Capacity is the next power of two at or above count: no separate field to keep.
    u32 cap = 1;
    while (cap < count) cap <<= 1;
    if (count == 0 || count == cap) {
        u32 ncap = count ? cap * 2 : 4;
        T* n = (T*)alloc_in(a, sizeof(T) * ncap, alignof(T) > 8 ? alignof(T) : 8);
        if (count) memcpy((void*)n, arr, sizeof(T) * count);
        arr = n;
    }
    T& t = arr[count++];
    memset((void*)&t, 0, sizeof t);
    return t;
}

f32 num(const JsonValue* v, f32 dflt = 0.0f) { return (f32)json_number(v, dflt); }

f32 srgb_to_linear(u32 c) {
    f32 v = (f32)c / 255.0f;
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

// "#AARRGGBB" or "#RRGGBB", Tiled's sRGB, as a linear color.
bool parse_color(const char* s, color* out) {
    if (!s || s[0] != '#') return false;
    usize n = strlen(s + 1);
    if (n != 6 && n != 8) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (*end) return false;
    u32 a = n == 8 ? (u32)(v >> 24) & 255 : 255;
    *out = color_rgba(srgb_to_linear((u32)(v >> 16) & 255), srgb_to_linear((u32)(v >> 8) & 255), srgb_to_linear((u32)v & 255), (f32)a / 255.0f);
    return true;
}

bool parse_props(Parse& p, TilemapAsset& m, const JsonValue* list, MapRange* out) {
    out->first = m.prop_count;
    out->count = 0;
    if (!list) return true;
    if (!list->is_array()) return p.fail("properties must be an array");
    for (const JsonValue* it = json_first(*p.doc, *list); it; it = json_next(*p.doc, *it)) {
        const char* type = json_string(json_get(*p.doc, *it, "type"), "string");
        const JsonValue* value = json_get(*p.doc, *it, "value");
        MapProperty& pr = append(p.a, m.props, m.prop_count);
        pr.name = dup(p.a, json_string(json_get(*p.doc, *it, "name"), ""));
        pr.str = "";
        if (!strcmp(type, "string") || !strcmp(type, "file")) { pr.type = !strcmp(type, "file") ? MapPropType::File : MapPropType::String; pr.str = dup(p.a, json_string(value, "")); }
        else if (!strcmp(type, "int"))    { pr.type = MapPropType::Int; pr.num = json_number(value, 0); }
        else if (!strcmp(type, "float"))  { pr.type = MapPropType::Float; pr.num = json_number(value, 0); }
        else if (!strcmp(type, "bool"))   { pr.type = MapPropType::Bool; pr.num = json_bool(value, false) ? 1 : 0; }
        else if (!strcmp(type, "object")) { pr.type = MapPropType::Object; pr.num = json_number(value, 0); }
        else if (!strcmp(type, "color"))  { pr.type = MapPropType::Color; if (!parse_color(json_string(value, ""), &pr.col)) pr.col = color_rgba(0, 0, 0, 0); }
        else { pr.type = MapPropType::String; } // a class-typed property: its members are not read
        ++out->count;
    }
    return true;
}

bool prop_bool(const TilemapAsset& m, MapRange r, const char* name) {
    const MapProperty* p = tilemap_property(m, r, name);
    return p && p->type == MapPropType::Bool && p->num != 0;
}

i32 b64(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes base64 (whitespace skipped) into a new buffer; false on a bad character.
bool base64_decode(Allocator* a, const char* s, usize n, u8** out, usize* out_len) {
    u8* d = (u8*)alloc_in(a, n / 4 * 3 + 3);
    usize at = 0;
    u32 acc = 0, bits = 0;
    for (usize i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '=' ) break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        i32 v = b64(c);
        if (v < 0) return false;
        acc = acc << 6 | (u32)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; d[at++] = (u8)(acc >> bits); }
    }
    *out = d;
    *out_len = at;
    return true;
}

struct GroupState { vec2 offset; f32 opacity; color tint; bool visible; };

bool parse_tile_data(Parse& p, const JsonValue& layer, MapLayer& l) {
    usize cells = (usize)l.width * l.height;
    u32* gids = (u32*)alloc_in(p.a, sizeof(u32) * (cells ? cells : 1));
    l.gids = gids;
    const JsonValue* data = json_get(*p.doc, layer, "data");
    if (json_get(*p.doc, layer, "chunks")) return p.fail("layer '%s': infinite maps (chunks) are not supported", l.name);
    if (!data) return p.fail("layer '%s' has no data", l.name);
    if (data->is_array()) {
        if (data->count != cells) return p.fail("layer '%s': %u cells, the layer is %ux%u", l.name, data->count, l.width, l.height);
        u32 i = 0;
        for (const JsonValue* v = json_first(*p.doc, *data); v; v = json_next(*p.doc, *v)) gids[i++] = (u32)json_number(v, 0);
        return true;
    }
    if (!data->is_string()) return p.fail("layer '%s': data is neither an array nor a string", l.name);
    const char* enc = json_string(json_get(*p.doc, layer, "encoding"), "csv");
    const char* comp = json_string(json_get(*p.doc, layer, "compression"), "");
    if (strcmp(enc, "base64") != 0) return p.fail("layer '%s': encoding '%s' with string data", l.name, enc);
    MemScope scope;
    u8* raw = nullptr;
    usize raw_len = 0;
    if (!base64_decode(mem_scratch(), data->str, data->len, &raw, &raw_len)) return p.fail("layer '%s': bad base64", l.name);
    const u8* bytes = raw;
    usize len = raw_len;
    if (comp[0]) {
        u8* out = (u8*)alloc_in(mem_scratch(), cells * 4 + 1);
        InflateOut r;
        if (!strcmp(comp, "zlib")) r = inflate_zlib(raw, raw_len, out, cells * 4);
        else if (!strcmp(comp, "gzip")) r = inflate_gzip(raw, raw_len, out, cells * 4);
        else return p.fail("layer '%s': compression '%s' is not supported (save as zlib, gzip or uncompressed)", l.name, comp);
        if (r.result != InflateResult::Ok) return p.fail("layer '%s': %s data: %s", l.name, comp, inflate_result_name(r.result));
        bytes = out;
        len = r.produced;
    }
    if (len != cells * 4) return p.fail("layer '%s': %llu bytes of cells, the layer needs %llu", l.name, (unsigned long long)len, (unsigned long long)(cells * 4));
    for (usize i = 0; i < cells; ++i) gids[i] = (u32)bytes[i * 4] | (u32)bytes[i * 4 + 1] << 8 | (u32)bytes[i * 4 + 2] << 16 | (u32)bytes[i * 4 + 3] << 24;
    return true;
}

bool parse_object(Parse& p, TilemapAsset& m, const JsonValue& o, u32 layer) {
    if (json_get(*p.doc, o, "template")) return p.fail("object %u uses a template, which is not supported", (u32)json_number(json_get(*p.doc, o, "id"), 0));
    MapObject& ob = append(p.a, m.objects, m.object_count);
    ob.id       = (u32)json_number(json_get(*p.doc, o, "id"), 0);
    ob.name     = dup(p.a, json_string(json_get(*p.doc, o, "name"), ""));
    const char* cls = json_string(json_get(*p.doc, o, "class"), nullptr);
    ob.cls      = dup(p.a, cls ? cls : json_string(json_get(*p.doc, o, "type"), ""));
    ob.x        = num(json_get(*p.doc, o, "x"));
    ob.y        = num(json_get(*p.doc, o, "y"));
    ob.width    = num(json_get(*p.doc, o, "width"));
    ob.height   = num(json_get(*p.doc, o, "height"));
    ob.rotation = num(json_get(*p.doc, o, "rotation"));
    ob.gid      = (u32)json_number(json_get(*p.doc, o, "gid"), 0);
    ob.point    = json_bool(json_get(*p.doc, o, "point"), false);
    ob.ellipse  = json_bool(json_get(*p.doc, o, "ellipse"), false);
    ob.layer    = layer;
    MapRange props;
    if (!parse_props(p, m, json_get(*p.doc, o, "properties"), &props)) return false;
    m.objects[m.object_count - 1].props = props; // the append above may have moved nothing, but be safe
    return true;
}

bool parse_layers(Parse& p, TilemapAsset& m, const JsonValue& list, const GroupState& g) {
    for (const JsonValue* lv = json_first(*p.doc, list); lv; lv = json_next(*p.doc, *lv)) {
        const char* type = json_string(json_get(*p.doc, *lv, "type"), "");
        GroupState here = g;
        here.offset.x += num(json_get(*p.doc, *lv, "offsetx"));
        here.offset.y += num(json_get(*p.doc, *lv, "offsety"));
        here.opacity  *= num(json_get(*p.doc, *lv, "opacity"), 1.0f);
        here.visible   = here.visible && json_bool(json_get(*p.doc, *lv, "visible"), true);
        color tc;
        if (parse_color(json_string(json_get(*p.doc, *lv, "tintcolor"), nullptr), &tc))
            here.tint = color_rgba(here.tint.r * tc.r, here.tint.g * tc.g, here.tint.b * tc.b, here.tint.a * tc.a);
        if (!strcmp(type, "group")) {
            const JsonValue* inner = json_get(*p.doc, *lv, "layers");
            if (inner && inner->is_array() && !parse_layers(p, m, *inner, here)) return false;
            continue;
        }
        u32 index = m.layer_count;
        MapLayer& l = append(p.a, m.layers, m.layer_count);
        l.name    = dup(p.a, json_string(json_get(*p.doc, *lv, "name"), ""));
        l.id      = (u32)json_number(json_get(*p.doc, *lv, "id"), 0);
        l.visible = here.visible;
        l.opacity = here.opacity;
        l.tint    = here.tint;
        l.offset  = here.offset;
        MapRange props;
        if (!parse_props(p, m, json_get(*p.doc, *lv, "properties"), &props)) return false;
        m.layers[index].props = props;
        if (!strcmp(type, "tilelayer")) {
            m.layers[index].kind   = MapLayerKind::Tiles;
            m.layers[index].width  = (u32)json_number(json_get(*p.doc, *lv, "width"), 0);
            m.layers[index].height = (u32)json_number(json_get(*p.doc, *lv, "height"), 0);
            if (!parse_tile_data(p, *lv, m.layers[index])) return false;
        } else if (!strcmp(type, "objectgroup")) {
            m.layers[index].kind = MapLayerKind::Objects;
            u32 first = m.object_count;
            const JsonValue* objs = json_get(*p.doc, *lv, "objects");
            for (const JsonValue* o = objs && objs->is_array() ? json_first(*p.doc, *objs) : nullptr; o; o = json_next(*p.doc, *o))
                if (!parse_object(p, m, *o, index)) return false;
            m.layers[index].objects = MapRange{first, m.object_count - first};
        } else if (!strcmp(type, "imagelayer")) {
            m.layers[index].kind = MapLayerKind::Image; // kept for its name and properties; not drawn
        } else {
            return p.fail("layer '%s' has unknown type '%s'", m.layers[index].name, type);
        }
    }
    return true;
}

int compare_tiles(const void* a, const void* b) {
    u32 x = ((const MapTile*)a)->local, y = ((const MapTile*)b)->local;
    return x < y ? -1 : x > y ? 1 : 0;
}

// A tileset's own fields (embedded in a map or in a .tsj), into m.tilesets[index].
// `base` is the file the tileset is in, for its image.
bool parse_tileset_body(Parse& p, TilemapAsset& m, u32 index, const JsonValue& ts) {
    const char* image = json_string(json_get(*p.doc, ts, "image"), nullptr);
    if (!image) return p.fail("tileset '%s' has no single image (image collections are not supported)", json_string(json_get(*p.doc, ts, "name"), "?"));
    char resolved[KV_ASSET_PATH_MAX];
    if (!tilemap_resolve(p.base, image, resolved, sizeof resolved)) return p.fail("tileset image '%s' is outside the asset root", image);
    MapRange props;
    if (!parse_props(p, m, json_get(*p.doc, ts, "properties"), &props)) return false;
    u32 tiles_first = m.tile_count;
    const JsonValue* tiles = json_get(*p.doc, ts, "tiles");
    for (const JsonValue* t = tiles && tiles->is_array() ? json_first(*p.doc, *tiles) : nullptr; t; t = json_next(*p.doc, *t)) {
        u32 at = m.tile_count;
        MapTile& tile = append(p.a, m.tiles, m.tile_count);
        tile.local = (u32)json_number(json_get(*p.doc, *t, "id"), 0);
        MapRange tp;
        if (!parse_props(p, m, json_get(*p.doc, *t, "properties"), &tp)) return false;
        m.tiles[at].props = tp;
        // Collision shapes: the rectangles of the tile's object group (points, polygons and
        // ellipses are not shapes a box can collide with here, and are skipped).
        MapRange rects{m.rect_count, 0};
        if (const JsonValue* og = json_get(*p.doc, *t, "objectgroup")) {
            const JsonValue* objs = json_get(*p.doc, *og, "objects");
            for (const JsonValue* o = objs && objs->is_array() ? json_first(*p.doc, *objs) : nullptr; o; o = json_next(*p.doc, *o)) {
                if (json_bool(json_get(*p.doc, *o, "point"), false) || json_bool(json_get(*p.doc, *o, "ellipse"), false) ||
                    json_get(*p.doc, *o, "polygon") || json_get(*p.doc, *o, "polyline")) continue;
                rect2& r = append(p.a, m.rects, m.rect_count);
                r = rect2{num(json_get(*p.doc, *o, "x")), num(json_get(*p.doc, *o, "y")), num(json_get(*p.doc, *o, "width")), num(json_get(*p.doc, *o, "height"))};
                ++rects.count;
            }
        }
        m.tiles[at].rects = rects;
        MapRange frames{m.frame_count, 0};
        if (const JsonValue* anim = json_get(*p.doc, *t, "animation")) {
            for (const JsonValue* f = anim->is_array() ? json_first(*p.doc, *anim) : nullptr; f; f = json_next(*p.doc, *f)) {
                MapFrame& fr = append(p.a, m.frames, m.frame_count);
                fr.local   = (u32)json_number(json_get(*p.doc, *f, "tileid"), 0);
                fr.seconds = num(json_get(*p.doc, *f, "duration"), 100.0f) / 1000.0f;
                ++frames.count;
            }
        }
        m.tiles[at].frames = frames;
    }
    if (m.tile_count - tiles_first > 1) kv::sort(m.tiles + tiles_first, m.tile_count - tiles_first, sizeof(MapTile), compare_tiles);
    MapTileset& t = m.tilesets[index];
    if (!t.name || !t.name[0] || t.image == nullptr) t.name = dup(p.a, json_string(json_get(*p.doc, ts, "name"), ""));
    t.image    = dup(p.a, resolved);
    t.count    = (u32)json_number(json_get(*p.doc, ts, "tilecount"), 0);
    t.columns  = (u32)json_number(json_get(*p.doc, ts, "columns"), 0);
    t.tile_w   = (u32)json_number(json_get(*p.doc, ts, "tilewidth"), 0);
    t.tile_h   = (u32)json_number(json_get(*p.doc, ts, "tileheight"), 0);
    t.margin   = (u32)json_number(json_get(*p.doc, ts, "margin"), 0);
    t.spacing  = (u32)json_number(json_get(*p.doc, ts, "spacing"), 0);
    t.image_w  = (u32)json_number(json_get(*p.doc, ts, "imagewidth"), 0);
    t.image_h  = (u32)json_number(json_get(*p.doc, ts, "imageheight"), 0);
    t.tiles    = MapRange{tiles_first, m.tile_count - tiles_first};
    t.props    = props;
    if (!t.columns || !t.tile_w || !t.tile_h || !t.image_w || !t.image_h) return p.fail("tileset '%s' is missing its tile or image size", t.name);
    return true;
}

bool parse_json(Parse& p, const char* json, usize len, JsonDoc& doc) {
    if (!json_parse(json, len, mem_scratch(), doc)) return p.fail("JSON: %s at byte %u", doc.error ? doc.error : "?", doc.error_at);
    p.doc = &doc;
    if (!doc.root().is_object()) return p.fail("not a JSON object");
    return true;
}

} // namespace

bool tilemap_resolve(const char* base, const char* rel, char* out, usize cap) {
    char joined[512];
    const char* slash = base ? strrchr(base, '/') : nullptr;
    int n = slash ? snprintf(joined, sizeof joined, "%.*s/%s", (int)(slash - base), base, rel) : snprintf(joined, sizeof joined, "%s", rel);
    if (n < 0 || (usize)n >= sizeof joined) return false;
    // Resolve "..": segments onto a stack.
    char segs[64][128];
    u32 depth = 0;
    for (char* p = joined; *(p += strspn(p, "/\\"));) {
        char* tok = p;           // a segment, ended in place at its separator
        p += strcspn(p, "/\\");
        if (*p) *p++ = 0;
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (!depth) return false; --depth; continue; }
        if (depth >= 64 || strlen(tok) >= sizeof segs[0]) return false;
        strcpy(segs[depth++], tok);
    }
    usize at = 0;
    for (u32 i = 0; i < depth; ++i) {
        usize l = strlen(segs[i]);
        if (at + l + 2 > cap) return false;
        if (i) out[at++] = '/';
        memcpy(out + at, segs[i], l);
        at += l;
    }
    if (!at) return false;
    out[at] = 0;
    return true;
}

bool tilemap_parse_map(const char* json, usize len, const char* base, Allocator* alloc, TilemapAsset& m, char* error, usize cap) {
    MemScope scope;
    Parse p{alloc, error, cap, base, nullptr};
    JsonDoc doc;
    if (!parse_json(p, json, len, doc)) return false;
    const JsonValue& root = doc.root();
    const char* orient = json_string(json_get(doc, root, "orientation"), "orthogonal");
    if (strcmp(orient, "orthogonal") != 0) return p.fail("orientation '%s' is not supported (orthogonal only)", orient);
    if (json_bool(json_get(doc, root, "infinite"), false)) return p.fail("infinite maps are not supported");
    m.width  = (u32)json_number(json_get(doc, root, "width"), 0);
    m.height = (u32)json_number(json_get(doc, root, "height"), 0);
    m.tile_w = (u32)json_number(json_get(doc, root, "tilewidth"), 0);
    m.tile_h = (u32)json_number(json_get(doc, root, "tileheight"), 0);
    if (!m.width || !m.height || !m.tile_w || !m.tile_h) return p.fail("the map has no size or no tile size");
    if (!parse_props(p, m, json_get(doc, root, "properties"), &m.map_props)) return false;
    const JsonValue* tilesets = json_get(doc, root, "tilesets");
    for (const JsonValue* ts = tilesets && tilesets->is_array() ? json_first(doc, *tilesets) : nullptr; ts; ts = json_next(doc, *ts)) {
        u32 index = m.tileset_count;
        MapTileset& t = append(alloc, m.tilesets, m.tileset_count);
        t.first_gid = (u32)json_number(json_get(doc, *ts, "firstgid"), 1);
        if (const char* src = json_string(json_get(doc, *ts, "source"), nullptr)) {
            char resolved[KV_ASSET_PATH_MAX];
            if (!tilemap_resolve(base, src, resolved, sizeof resolved)) return p.fail("tileset '%s' is outside the asset root", src);
            if (strstr(src, ".tsx")) return p.fail("tileset '%s' is TSX (XML); export it as JSON (.tsj)", src);
            m.tilesets[index].name = dup(alloc, resolved); // filled by tilemap_parse_tileset once read
            m.tilesets[index].image = nullptr;
        } else if (!parse_tileset_body(p, m, index, *ts)) {
            return false;
        }
    }
    const JsonValue* layers = json_get(doc, root, "layers");
    if (!layers || !layers->is_array()) return p.fail("the map has no layers");
    GroupState g{vec2{0, 0}, 1.0f, color_white(), true};
    return parse_layers(p, m, *layers, g);
}

bool tilemap_parse_tileset(const char* json, usize len, const char* base, Allocator* alloc, TilemapAsset& m, u32 index, char* error, usize cap) {
    MemScope scope;
    Parse p{alloc, error, cap, base, nullptr};
    JsonDoc doc;
    if (!parse_json(p, json, len, doc)) return false;
    if (index >= m.tileset_count) return p.fail("no tileset %u", index);
    return parse_tileset_body(p, m, index, doc.root());
}

// ---- queries over the record ----------------------------------------------------------------------

const MapProperty* tilemap_property(const TilemapAsset& m, MapRange r, const char* name) {
    for (u32 i = 0; i < r.count; ++i)
        if (r.first + i < m.prop_count && !strcmp(m.props[r.first + i].name, name)) return &m.props[r.first + i];
    return nullptr;
}

const MapProperty* tilemap_map_property(const TilemapAsset& m, const char* name) { return tilemap_property(m, m.map_props, name); }

const MapLayer* tilemap_layer_find(const TilemapAsset& m, const char* name, u32* index) {
    for (u32 i = 0; i < m.layer_count; ++i)
        if (!strcmp(m.layers[i].name, name)) { if (index) *index = i; return &m.layers[i]; }
    return nullptr;
}

const MapObject* tilemap_object_find(const TilemapAsset& m, const char* name) {
    for (u32 i = 0; i < m.object_count; ++i)
        if (!strcmp(m.objects[i].name, name)) return &m.objects[i];
    return nullptr;
}

u32 tilemap_gid_at(const TilemapAsset& m, u32 layer, i32 x, i32 y) {
    if (layer >= m.layer_count) return 0;
    const MapLayer& l = m.layers[layer];
    if (l.kind != MapLayerKind::Tiles || x < 0 || y < 0 || (u32)x >= l.width || (u32)y >= l.height) return 0;
    return l.gids[(usize)y * l.width + (u32)x];
}

const MapTileset* tilemap_tileset_of(const TilemapAsset& m, u32 gid, u32* local) {
    u32 id = gid & KV_TILE_GID_MASK;
    if (!id) return nullptr;
    const MapTileset* best = nullptr;
    for (u32 i = 0; i < m.tileset_count; ++i)
        if (m.tilesets[i].first_gid <= id && (!best || m.tilesets[i].first_gid > best->first_gid)) best = &m.tilesets[i];
    if (best && local) *local = id - best->first_gid;
    return best;
}

const MapTile* tilemap_tile_info(const TilemapAsset& m, u32 gid) {
    u32 local = 0;
    const MapTileset* ts = tilemap_tileset_of(m, gid, &local);
    if (!ts) return nullptr;
    u32 lo = 0, hi = ts->tiles.count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (m.tiles[ts->tiles.first + mid].local < local) lo = mid + 1; else hi = mid;
    }
    return lo < ts->tiles.count && m.tiles[ts->tiles.first + lo].local == local ? &m.tiles[ts->tiles.first + lo] : nullptr;
}

const MapProperty* tilemap_tile_property(const TilemapAsset& m, u32 gid, const char* name) {
    const MapTile* t = tilemap_tile_info(m, gid);
    return t ? tilemap_property(m, t->props, name) : nullptr;
}

point2 tilemap_to_world(point2 origin, f32 map_x, f32 map_y) { return point2{origin.x + map_x, origin.y - map_y}; }

bool tilemap_cell_of(const TilemapAsset& m, point2 origin, point2 world, i32* x, i32* y) {
    f32 mx = world.x - origin.x, my = origin.y - world.y;
    *x = (i32)floorf(mx / (f32)m.tile_w);
    *y = (i32)floorf(my / (f32)m.tile_h);
    return *x >= 0 && *y >= 0 && (u32)*x < m.width && (u32)*y < m.height;
}

point2 tilemap_cell_center(const TilemapAsset& m, point2 origin, i32 x, i32 y) {
    return tilemap_to_world(origin, ((f32)x + 0.5f) * (f32)m.tile_w, ((f32)y + 0.5f) * (f32)m.tile_h);
}

bool tilemap_solid(const TilemapAsset& m, i32 x, i32 y) {
    if (x < 0 || y < 0 || (u32)x >= m.width || (u32)y >= m.height) return true;
    return m.solid && m.solid[(usize)y * m.width + (u32)x];
}

// Built once the tilesets are known: whole solid cells, and the rectangles of tile shapes
// and solid objects in map pixels.
void tilemap_build_solids(TilemapAsset& m, Allocator* a) {
    usize cells = (usize)m.width * m.height;
    m.solid = (u8*)alloc_in(a, cells ? cells : 1);
    memset(m.solid, 0, cells ? cells : 1);
    m.solid_rect_count = 0;
    m.solid_rects = nullptr;
    for (u32 li = 0; li < m.layer_count; ++li) {
        const MapLayer& l = m.layers[li];
        if (l.kind == MapLayerKind::Objects) {
            for (u32 k = 0; k < l.objects.count; ++k) {
                const MapObject& o = m.objects[l.objects.first + k];
                if (o.point) continue;
                // A tile object collides by its tile's shapes (scaled to the object's size), or
                // whole with the bool property `solid`. Its (x, y) is its bottom-left; a
                // shape's is its top-left.
                const MapTile* t = o.gid ? tilemap_tile_info(m, o.gid) : nullptr;
                const MapTileset* ts = o.gid ? tilemap_tileset_of(m, o.gid, nullptr) : nullptr;
                if (t && ts && t->rects.count && !prop_bool(m, o.props, "solid")) {
                    f32 sx = o.width / (f32)ts->tile_w, sy = o.height / (f32)ts->tile_h, top = o.y - o.height;
                    for (u32 q = 0; q < t->rects.count; ++q) {
                        rect2 r = m.rects[t->rects.first + q];
                        if (o.gid & KV_TILE_FLIP_H) r.x = (f32)ts->tile_w - r.x - r.w;
                        if (o.gid & KV_TILE_FLIP_V) r.y = (f32)ts->tile_h - r.y - r.h;
                        append(a, m.solid_rects, m.solid_rect_count) = rect2{o.x + r.x * sx, top + r.y * sy, r.w * sx, r.h * sy};
                    }
                    continue;
                }
                if (!prop_bool(m, o.props, "solid")) continue;
                rect2 r = o.gid ? rect2{o.x, o.y - o.height, o.width, o.height} : rect2{o.x, o.y, o.width, o.height};
                append(a, m.solid_rects, m.solid_rect_count) = r;
            }
            continue;
        }
        if (l.kind != MapLayerKind::Tiles) continue;
        bool whole_layer = prop_bool(m, l.props, "collision");
        for (u32 y = 0; y < l.height && y < m.height; ++y)
            for (u32 x = 0; x < l.width && x < m.width; ++x) {
                u32 gid = l.gids[(usize)y * l.width + x];
                if (!(gid & KV_TILE_GID_MASK)) continue;
                if (whole_layer) { m.solid[(usize)y * m.width + x] = 1; continue; }
                const MapTile* t = tilemap_tile_info(m, gid);
                if (!t) continue;
                if (prop_bool(m, t->props, "solid")) { m.solid[(usize)y * m.width + x] = 1; continue; }
                u32 local = 0;
                const MapTileset* ts = tilemap_tileset_of(m, gid, &local);
                f32 tw = (f32)ts->tile_w, th = (f32)ts->tile_h;
                // The tile's image sits on the cell's bottom-left.
                f32 left = (f32)x * (f32)m.tile_w + l.offset.x, top = (f32)(y + 1) * (f32)m.tile_h - th + l.offset.y;
                for (u32 k = 0; k < t->rects.count; ++k) {
                    rect2 r = m.rects[t->rects.first + k];
                    // Flips, in Tiled's order (diagonal, then horizontal, then vertical).
                    if (gid & KV_TILE_FLIP_D) r = rect2{r.y, r.x, r.h, r.w};
                    f32 w = (gid & KV_TILE_FLIP_D) ? th : tw, h = (gid & KV_TILE_FLIP_D) ? tw : th;
                    if (gid & KV_TILE_FLIP_H) r.x = w - r.x - r.w;
                    if (gid & KV_TILE_FLIP_V) r.y = h - r.y - r.h;
                    append(a, m.solid_rects, m.solid_rect_count) = rect2{left + r.x, top + r.y, r.w, r.h};
                }
            }
    }
}

} // namespace kv
