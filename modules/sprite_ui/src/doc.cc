// UI documents: the asset, the style, and screens built from their JSON.
#include "sprite_ui_internal.h"
#include "korvid/asset/blob.h"
#include "korvid/core/format.h"
#include "korvid/dejavu/dejavu.h"
#include "korvid/asset/font.h"
#include "korvid/asset_render/material.h"
#include "korvid/asset/scope.h"
#include "korvid/asset/sound.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/utf8.h"
#include "korvid/math/scalar.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

const char* const k_type_names[(u32)SpuiType::Count] = {"box", "panel", "image", "label", "stack", "list", "scroll", "field", "meter"};
const char* const k_point_names[9] = {"center", "topleft", "top", "topright", "left", "right", "bottomleft", "bottom", "bottomright"};

void* doc_resolve(kv_handle h, void* ctx) { return ((SpuiState_*)ctx)->docs.get(Handle{h}); }
const char* doc_name_of(kv_handle h, void* ctx) { SpuiDoc* d = ((SpuiState_*)ctx)->docs.get(Handle{h}); return d ? d->path : nullptr; }
kv_handle doc_find_name(const char* name, void* ctx) {
    const Handle* h = ((SpuiState_*)ctx)->doc_by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int doc_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    SpuiDoc* d = ((SpuiState_*)ctx)->docs.get(Handle{h});
    if (!d) return format(out, cap, "stale");
    return format(out, cap, "'%s' %s, %u frames, %u images, %u sounds", d->path,
                    d->state == AssetState::Ready ? "ready" : d->state == AssetState::Failed ? "failed" : "pending",
                    d->frames.len, d->images.len, d->sounds.len);
}

u32* doc_refs(void* user, Handle h) { SpuiDoc* d = ((SpuiState_*)user)->docs.get(h); return d ? &d->refs : nullptr; }
AssetState doc_state(void* user, Handle h) { SpuiDoc* d = ((SpuiState_*)user)->docs.get(h); return d ? d->state : AssetState::Failed; }
const char* doc_path(void* user, Handle h) { SpuiDoc* d = ((SpuiState_*)user)->docs.get(h); return d ? d->path : ""; }

// What a document holds: its file, its pictures, its sounds and its fonts.
void drop_style(Engine& e, SpuiDoc& d) {
    for (SpuiFrame& f : d.frames) { asset_unref(e, f.texture); asset_unref(e, f.material); }
    for (SpuiImage& i : d.images) { asset_unref(e, i.texture); asset_unref(e, i.material); }
    for (SpuiSound& s : d.sounds) asset_unref(e, s.sound);
    for (SpuiFont& f : d.fonts) asset_unref(e, f.font);
    d.frames.clear(); d.images.clear(); d.colors.clear(); d.sounds.clear(); d.fonts.clear();
    d.parsed = false;
}

void doc_retire(Engine& e, void* user, Handle h) {
    SpuiState_& s = *(SpuiState_*)user;
    SpuiDoc* d = s.docs.get(h);
    if (!d) return;
    drop_style(e, *d);
    asset_unref(e, d->blob);
    d->frames.free(); d->images.free(); d->colors.free(); d->sounds.free(); d->fonts.free();
    s.doc_by_path.remove(name_lookup(d->path));
    s.docs.destroy(h);
}

// A texture and its material (alpha, nearest), both held by the document.
void picture(Engine& e, const char* path, Handle* texture, Handle* material) {
    *texture = asset_load_texture(e, path, Handle{});
    asset_ref(e, *texture);
    *material = asset_material_for_texture(e, *texture, BlendMode::Alpha, TextureFilter::Nearest, Handle{});
    asset_ref(e, *material);
}

bool parse_style(Engine& e, SpuiDoc& d, const JsonDoc& doc, const char** why) {
    const JsonValue& root = doc.root();
    f32 v2[2];
    if (json_numbers(doc, json_get(doc, root, "design"), v2, 2) == 2 && v2[0] > 0 && v2[1] > 0) d.design = vec2{v2[0], v2[1]};
    d.pixel = json_bool(json_get(doc, root, "pixel"), true);
    const char* names[3] = {"move", "confirm", "cancel"};
    const JsonValue* actions = json_get(doc, root, "actions");
    for (u32 i = 0; i < 3; ++i) utf8_copy(d.actions[i], sizeof d.actions[i], json_string(actions ? json_get(doc, *actions, names[i]) : nullptr, names[i]));
    const JsonValue* style = json_get(doc, root, "style");
    if (!style || !style->is_object()) { *why = "a document has a style"; return false; }
    const char* font = json_string(json_get(doc, *style, "font"), KV_DEJAVU_SANS);
    utf8_copy(d.font_path, sizeof d.font_path, font);
    d.font_px = (f32)json_number(json_get(doc, *style, "size"), 16.0);
    d.line = (f32)json_number(json_get(doc, *style, "line"), 0.0);
    d.space = (f32)json_number(json_get(doc, *style, "space"), 0.0);
    if (const JsonValue* colors = json_get(doc, *style, "colors"))
        for (const JsonValue* c = json_first(doc, *colors); c; c = json_next(doc, *c)) {
            SpuiColor uc{name_intern(c->key), color_white()};
            if (!spui_color_parse(nullptr, json_string(c, ""), &uc.c)) { *why = "a color is #rrggbb or #rrggbbaa"; return false; }
            d.colors.push(uc);
        }
    if (const char* sh = json_string(json_get(doc, *style, "shadow"), nullptr)) spui_color_parse(&d, sh, &d.shadow);
    d.shadow_offset = (u32)json_u64(json_get(doc, *style, "shadow_offset"), 1);
    if (d.shadow_offset < 1) d.shadow_offset = 1;
    if (d.shadow_offset > 8) d.shadow_offset = 8;
    if (const JsonValue* frames = json_get(doc, *style, "frames"))
        for (const JsonValue* f = json_first(doc, *frames); f; f = json_next(doc, *f)) {
            SpuiFrame uf{};
            uf.name = name_intern(f->key);
            const char* tex = json_string(json_get(doc, *f, "texture"), nullptr);
            if (!tex) { *why = "a frame has a texture"; return false; }
            if (json_numbers(doc, json_get(doc, *f, "insets"), uf.insets, 4) != 4) { *why = "a frame has insets [left, top, right, bottom]"; return false; }
            picture(e, tex, &uf.texture, &uf.material);
            d.frames.push(uf);
        }
    if (const JsonValue* images = json_get(doc, *style, "images"))
        for (const JsonValue* im = json_first(doc, *images); im; im = json_next(doc, *im)) {
            SpuiImage ui{};
            ui.name = name_intern(im->key);
            const char* tex = json_string(json_get(doc, *im, "texture"), nullptr);
            f32 r[4];
            if (!tex || json_numbers(doc, json_get(doc, *im, "rect"), r, 4) != 4) { *why = "an image has a texture and a rect [x, y, w, h]"; return false; }
            ui.rect = rect2{r[0], r[1], r[2], r[3]};
            ui.frames = (u32)json_number(json_get(doc, *im, "frames"), 1.0);
            ui.scale = (f32)json_number(json_get(doc, *im, "scale"), 1.0);
            if (!ui.frames) ui.frames = 1;
            picture(e, tex, &ui.texture, &ui.material);
            d.images.push(ui);
        }
    if (const JsonValue* sounds = json_get(doc, *style, "sounds"))
        for (const JsonValue* sv = json_first(doc, *sounds); sv; sv = json_next(doc, *sv)) {
            const char* path = json_string(sv, nullptr);
            if (!path) { *why = "a sound is a path"; return false; }
            SpuiSound us{name_intern(sv->key), asset_load_sound(e, path, Handle{})};
            asset_ref(e, us.sound);
            d.sounds.push(us);
        }
    d.parsed = true;
    return true;
}

void fail_doc(Engine& e, Handle h, SpuiDoc& d, AssetError err, const char* why) {
    d.state = AssetState::Failed;
    d.error = err;
    utf8_copy(d.error_text, sizeof d.error_text, why ? why : "");
    KV_WARN("sprite_ui", "'%s': %s%s%s", d.path, asset_error_name(err), why ? ": " : "", why ? why : "");
    e.events.emit(AssetFailed{h, err});
}

void advance(Engine& e, Handle h, SpuiDoc& d) {
    if (d.blob.is_null()) { // reloading: the old bytes retired at this tick's first pump
        d.blob = asset_load_blob(e, d.path, Handle{});
        asset_ref(e, d.blob);
    }
    const BlobAsset* b = asset_blob(e, d.blob);
    if (!b || b->state == AssetState::Pending) return;
    if (b->state == AssetState::Failed) { fail_doc(e, h, d, b->error, "the file"); return; }
    if (!d.parsed) {
        MemScope scope;
        JsonDoc doc;
        const char* why = nullptr;
        if (!json_parse((const char*)b->data, b->size, mem_scratch(), doc)) { fail_doc(e, h, d, AssetError::Parse, doc.error); return; }
        if (!parse_style(e, d, doc, &why)) { drop_style(e, d); fail_doc(e, h, d, AssetError::Parse, why); return; }
    }
    // Ready when every picture and sound has settled (a failed one shows nothing).
    for (const SpuiFrame& f : d.frames) if (const TextureAsset* t = asset_texture(e, f.texture); t && t->state == AssetState::Pending) return;
    for (const SpuiImage& i : d.images) if (const TextureAsset* t = asset_texture(e, i.texture); t && t->state == AssetState::Pending) return;
    for (const SpuiSound& u : d.sounds) if (const SoundAsset* a = asset_sound(e, u.sound); a && a->state == AssetState::Pending) return;
    d.state = AssetState::Ready;
    KV_INFO("sprite_ui", "'%s' ready: %u frames, %u images, %u colors, %u sounds", d.path, d.frames.len, d.images.len, d.colors.len, d.sounds.len);
    e.events.emit(AssetReady{h});
}

f32 to_linear(u32 c) {
    f32 v = (f32)c / 255.0f;
    return v <= 0.04045f ? v / 12.92f : kv::pow((v + 0.055f) / 1.055f, 2.4f);
}

u32 hex(char c) { return c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 99; }

// ---- nodes from JSON -----------------------------------------------------------------------------

bool point_parse(const char* s, SpuiPoint* out) {
    for (u32 i = 0; i < 9; ++i) if (!strcmp(s, k_point_names[i])) { *out = (SpuiPoint)i; return true; }
    return false;
}

// One size: a number (fixed), "fit" or "fill".
void size_parse(const JsonValue* v, SpuiSize* mode, f32* size) {
    if (!v) return;
    if (v->is_number()) { *mode = SpuiSize::Fixed; *size = (f32)v->number(); }
    else if (v->is_string() && !strcmp(v->str(), "fill")) *mode = SpuiSize::Fill;
    else if (v->is_string() && !strcmp(v->str(), "fit")) *mode = SpuiSize::Fit;
}

const char* str_or(const JsonDoc& doc, const JsonValue& n, const char* key, const char* dflt) {
    const JsonValue* v = json_get(doc, n, key);
    if (!v) return dflt;
    if (v->is_string()) return v->str();
    return dflt;
}

bool build(Engine& e, SpuiState_& s, SpuiScreen& sc, const JsonDoc& doc, const JsonValue& n, u32 parent) {
    SpuiNodeDesc d;
    const char* type = str_or(doc, n, "type", "box");
    bool known = false;
    for (u32 i = 0; i < (u32)SpuiType::Count; ++i) if (!strcmp(type, k_type_names[i])) { d.type = (SpuiType)i; known = true; }
    if (!known) { KV_WARN("sprite_ui", "screen '%s': no node type '%s'", sc.name, type); return false; }
    d.name = str_or(doc, n, "name", "");
    if (const char* a = str_or(doc, n, "anchor", nullptr)) point_parse(a, &d.anchor);
    d.pivot = d.anchor;
    if (const char* p = str_or(doc, n, "pivot", nullptr)) point_parse(p, &d.pivot);
    f32 v[4];
    if (json_numbers(doc, json_get(doc, n, "offset"), v, 2) == 2) d.offset = vec2{v[0], v[1]};
    if (const JsonValue* sz = json_get(doc, n, "size")) {
        size_parse(json_at(doc, *sz, 0), &d.w_mode, &d.size.x);
        size_parse(json_at(doc, *sz, 1), &d.h_mode, &d.size.y);
    }
    if (const JsonValue* p = json_get(doc, n, "padding")) {
        if (p->is_number()) for (f32& x : d.padding) x = (f32)p->number();
        else if (json_numbers(doc, p, v, 4) == 4) memcpy(d.padding, v, sizeof d.padding);
        else if (json_numbers(doc, p, v, 2) == 2) { d.padding[0] = d.padding[2] = v[0]; d.padding[1] = d.padding[3] = v[1]; }
    }
    d.gap = (f32)json_number(json_get(doc, n, "gap"), 0.0);
    d.dir = !strcmp(str_or(doc, n, "dir", "column"), "row") ? SpuiDir::Row : SpuiDir::Column;
    const char* al = str_or(doc, n, "align", "start");
    d.align = !strcmp(al, "center") ? SpuiAlign::Center : !strcmp(al, "end") || !strcmp(al, "right") ? SpuiAlign::End : SpuiAlign::Start;
    d.visible = json_bool(json_get(doc, n, "visible"), true);
    d.text = str_or(doc, n, "text", "");
    d.color = str_or(doc, n, "color", "");
    d.shadow = str_or(doc, n, "shadow", "");
    d.font_scale = (f32)json_number(json_get(doc, n, "font_scale"), 1.0);
    d.wrap = json_bool(json_get(doc, n, "wrap"), false);
    d.reveal = (f32)json_number(json_get(doc, n, "reveal"), 0.0);
    d.frame = str_or(doc, n, "frame", "");
    d.fill = str_or(doc, n, "fill", "");
    d.image = str_or(doc, n, "image", "");
    d.frame_index = (u32)json_number(json_get(doc, n, "frame_index"), 0.0);
    char value[24] = {}, max[24] = {};
    auto num_or_key = [&](const char* key, char* out) {
        const JsonValue* x = json_get(doc, n, key);
        if (x && x->is_string()) utf8_copy(out, 24, x->str());
        else if (x && x->is_number()) format(out, 24, "%g", x->number());
    };
    num_or_key("value", value);
    num_or_key("max", max);
    d.value = value; d.max = max;
    d.bar = !strcmp(str_or(doc, n, "style", "pips"), "bar");
    d.event = str_or(doc, n, "event", "");
    d.max_length = (u32)json_number(json_get(doc, n, "max_length"), 24.0);
    d.input = json_bool(json_get(doc, n, "input"), false);
    u32 me = spui_node_add(s, sc, parent, d);
    // A list's items, written short: ["Resume", {"name": "save", "text": "Save"}].
    if (const JsonValue* items = json_get(doc, n, "items")) {
        for (const JsonValue* it = json_first(doc, *items); it; it = json_next(doc, *it)) {
            SpuiNodeDesc item;
            item.type = SpuiType::Label;
            char lower[32];
            if (it->is_string()) {
                item.text = it->str();
                u32 k = 0;
                for (const char* c = it->str(); *c && k + 1 < sizeof lower; ++c) lower[k++] = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c == ' ' ? '_' : *c;
                lower[k] = 0;
                item.name = lower;
                spui_node_add(s, sc, me, item);
            } else if (it->is_object()) {
                if (!build(e, s, sc, doc, *it, me)) return false;
            }
        }
    }
    if (const JsonValue* kids = json_get(doc, n, "children"))
        for (const JsonValue* k = json_first(doc, *kids); k; k = json_next(doc, *k))
            if (!build(e, s, sc, doc, *k, me)) return false;
    return true;
}

} // namespace

const char* spui_type_name(SpuiType t) { return (u32)t < (u32)SpuiType::Count ? k_type_names[(u32)t] : "?"; }

bool spui_color_parse(const SpuiDoc* d, const char* text, color* out) {
    if (!text || !text[0]) return false;
    if (text[0] != '#') {
        Name n = name_lookup(text);
        return d && !n.is_null() && spui_doc_color(*d, n, out);
    }
    usize len = strlen(text);
    if (len != 7 && len != 9) return false;
    u32 v[4] = {0, 0, 0, 255};
    for (u32 i = 0; i < (len - 1) / 2; ++i) {
        u32 hi = hex(text[1 + 2 * i]), lo = hex(text[2 + 2 * i]);
        if (hi > 15 || lo > 15) return false;
        v[i] = hi * 16 + lo;
    }
    *out = color_rgba(to_linear(v[0]), to_linear(v[1]), to_linear(v[2]), (f32)v[3] / 255.0f); // colors are linear; files say sRGB
    return true;
}

bool spui_doc_color(const SpuiDoc& d, Name n, color* out) {
    for (const SpuiColor& c : d.colors) if (c.name == n) { *out = c.c; return true; }
    return false;
}
const SpuiFrame* spui_doc_frame(const SpuiDoc& d, Name n) { for (const SpuiFrame& f : d.frames) if (f.name == n) return &f; return nullptr; }
const SpuiImage* spui_doc_image(const SpuiDoc& d, Name n) { for (const SpuiImage& i : d.images) if (i.name == n) return &i; return nullptr; }
const SpuiSound* spui_doc_sound(const SpuiDoc& d, const char* name) {
    Name n = name_lookup(name);
    for (const SpuiSound& u : d.sounds) if (u.name == n) return &u;
    return nullptr;
}

const FontAsset* spui_doc_font(Engine& e, SpuiState_& s, SpuiDoc& d, u32 px) {
    KV_UNUSED(s);
    if (px < 4) px = 4;
    if (px > 256) px = 256;
    for (const SpuiFont& f : d.fonts) if (f.px == px) return asset_font(e, f.font);
    FontDesc fd;
    fd.px = (f32)px;
    SpuiFont f{px, asset_load_font(e, d.font_path, fd, Handle{})};
    asset_ref(e, f.font);
    d.fonts.push(f);
    return asset_font(e, f.font);
}

void spui_node_set_text(SpuiState_& s, SpuiNode& n, const char* text) {
    usize len = text ? strlen(text) : 0;
    if (len + 1 > n.text_cap) {
        if (n.text) free_in(s.general, n.text, n.text_cap);
        n.text_cap = (u32)(len + 1 < 16 ? 16 : len + 1);
        n.text = (char*)alloc_in(s.general, n.text_cap);
    }
    if (len) memcpy(n.text, text, len);
    n.text[len] = 0;
}

void spui_node_free(SpuiState_& s, SpuiNode& n) {
    if (n.text) free_in(s.general, n.text, n.text_cap);
    n.text = nullptr;
    n.text_cap = 0;
}

u32 spui_node_add(SpuiState_& s, SpuiScreen& sc, u32 parent, const SpuiNodeDesc& d) {
    if (parent == KV_SPUI_NONE && sc.nodes.len) parent = 0; // a second root goes under the first
    SpuiNode n{};
    n.name = d.name && d.name[0] ? name_intern(d.name) : Name{};
    n.type = d.type;
    n.parent = parent;
    n.anchor = d.anchor; n.pivot = d.pivot; n.offset = d.offset;
    n.w_mode = d.w_mode; n.h_mode = d.h_mode; n.size = d.size;
    memcpy(n.padding, d.padding, sizeof n.padding);
    n.gap = d.gap; n.dir = d.dir; n.align = d.align;
    n.visible = d.visible; n.wrap = d.wrap; n.bar = d.bar; n.input = d.input;
    n.color = d.color && d.color[0] ? name_intern(d.color) : Name{};
    n.shadow = d.shadow && d.shadow[0] ? name_intern(d.shadow) : Name{};
    n.frame = d.frame && d.frame[0] ? name_intern(d.frame) : Name{};
    n.fill = d.fill && d.fill[0] ? name_intern(d.fill) : Name{};
    n.image = d.image && d.image[0] ? name_intern(d.image) : Name{};
    n.event = d.event && d.event[0] ? name_intern(d.event) : Name{};
    n.font_scale = d.font_scale > 0.0f ? d.font_scale : 1.0f;
    n.reveal_rate = d.reveal;
    n.frame_index = d.frame_index;
    utf8_copy(n.value, sizeof n.value, d.value ? d.value : "");
    utf8_copy(n.max, sizeof n.max, d.max ? d.max : "");
    n.max_length = d.max_length;
    n.revealed = d.reveal <= 0.0f;
    u32 me = sc.nodes.len;
    sc.nodes.push(n);
    spui_node_set_text(s, sc.nodes[me], d.text);
    if (parent != KV_SPUI_NONE) {
        SpuiNode& p = sc.nodes[parent];
        if (p.last == KV_SPUI_NONE) p.first = me; else sc.nodes[p.last].next = me;
        p.last = me;
    }
    return me;
}

bool spui_build_from_doc(Engine& e, SpuiState_& s, SpuiScreen& sc) {
    SpuiDoc* d = s.docs.get(sc.doc);
    const BlobAsset* b = d ? asset_blob(e, d->blob) : nullptr;
    if (!b || b->state != AssetState::Ready) return false;
    MemScope scope;
    JsonDoc doc;
    if (!json_parse((const char*)b->data, b->size, mem_scratch(), doc)) return false;
    const JsonValue* screens = json_get(doc, doc.root(), "screens");
    const JsonValue* root = screens ? json_get(doc, *screens, sc.name) : nullptr;
    if (!root || !root->is_object()) { KV_WARN("sprite_ui", "'%s' has no screen '%s'", d->path, sc.name); return false; }
    return build(e, s, sc, doc, *root, KV_SPUI_NONE);
}

bool spui_docs_init(Engine& e, SpuiState_& s, Allocator* general, MemTag tag) {
    s.doc_by_path.init(general);
    kv_handle_kind_desc desc{};
    desc.name = "sprite_ui.doc"; desc.type_key = type_key<SpuiDoc>(); desc.ctx = &s;
    desc.resolve = doc_resolve; desc.describe = doc_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    desc.name_of = doc_name_of; desc.find_name = doc_find_name;
    s.doc_kind = handle_kind_register(desc);
    if (!s.doc_kind) return false;
    s.docs.use_kind(s.doc_kind);
    if (!s.docs.init(256, tag)) return false;
    AssetKindDesc k;
    k.kind = s.doc_kind; k.refs = doc_refs; k.state = doc_state; k.path = doc_path; k.retire = doc_retire; k.user = &s;
    return asset_kind_register(e, k);
}

void spui_docs_shutdown(Engine& e, SpuiState_& s) {
    asset_kind_unregister(e, s.doc_kind);
    for (u32 i = 0; i < s.docs.len; ++i) {
        if (!s.docs.is_alive(i)) continue;
        SpuiDoc& d = s.docs.items[i];
        d.frames.free(); d.images.free(); d.colors.free(); d.sounds.free(); d.fonts.free();
    }
    kv_handle_kind_unregister(s.doc_kind);
    s.doc_by_path.free();
    s.docs.free();
}

void spui_docs_pump(Engine& e, SpuiState_& s) {
    for (u32 i = 0; i < s.docs.len; ++i)
        if (s.docs.is_alive(i) && s.docs.items[i].state == AssetState::Pending) advance(e, s.docs.handle_at(i), s.docs.items[i]);
}

Handle spui_load(Engine& e, const char* path, Handle scope) {
    SpuiState_* s = spui_state(e);
    if (!s || !path || strlen(path) >= KV_SPUI_PATH_MAX) { KV_ERROR("sprite_ui", "cannot load '%s'", path ? path : "?"); return Handle{}; }
    Name name = name_intern(path);
    Handle h;
    if (const Handle* existing = s->doc_by_path.find(name)) h = *existing;
    else {
        SpuiDoc d{};
        utf8_copy(d.path, sizeof d.path, path);
        d.frames.alloc = d.images.alloc = d.colors.alloc = d.sounds.alloc = d.fonts.alloc = s->general;
        h = s->docs.create(d);
        if (h.is_null()) return h;
        s->doc_by_path.set(name, h);
        SpuiDoc* rec = s->docs.get(h);
        rec->blob = asset_load_blob(e, path, Handle{});
        asset_ref(e, rec->blob);
    }
    if (!scope.is_null()) asset_scope_add(e, scope, h);
    return h;
}

bool spui_ready(Engine& e, Handle doc) {
    SpuiState_* s = spui_state(e);
    SpuiDoc* d = s ? s->docs.get(doc) : nullptr;
    return d && d->state == AssetState::Ready;
}

u32 spui_reload(Engine& e) {
    SpuiState_* s = spui_state(e);
    if (!s) return 0;
    u32 n = 0;
    for (u32 i = 0; i < s->docs.len; ++i) {
        if (!s->docs.is_alive(i)) continue;
        SpuiDoc& d = s->docs.items[i];
        drop_style(e, d);
        asset_unref(e, d.blob);
        // The bytes again: the blob retires at the next pump and the path is read anew.
        d.blob = Handle{};
        d.state = AssetState::Pending;
        ++n;
    }
    for (u32 i = 0; i < s->screens.len; ++i)
        if (s->screens.is_alive(i) && !s->screens.items[i].code) s->screens.items[i].built = false;
    return n;
}

} // namespace kv
