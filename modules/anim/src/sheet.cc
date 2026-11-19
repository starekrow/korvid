// Sprite sheets: the file format, the record, and the pump that builds a sheet's clips once
// its texture has a size.
#include "anim_internal.h"
#include "korvid/asset/blob.h"
#include "korvid/core/format.h"
#include "korvid/asset_render/material.h"
#include "korvid/asset/scope.h"
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

// ---- the format ---------------------------------------------------------------------------

bool anim_sheet_parse_header(const char* text, usize len, char* texture, usize cap, const char** error) {
    MemScope scope;
    JsonDoc doc;
    if (!json_parse(text, len, mem_scratch(), doc)) { *error = doc.error; return false; }
    const char* t = json_string(json_get(doc, doc.root(), "texture"), nullptr);
    if (!t || !t[0]) { *error = "a sheet names its texture"; return false; }
    if (strlen(t) >= cap) { *error = "texture path too long"; return false; }
    utf8_copy(texture, cap, t);
    return true;
}

bool anim_sheet_parse(const char* text, usize len, u32 image_w, u32 image_h, Allocator* alloc, AnimSheetParsed& out, const char** error) {
    // No scope of its own: the output may be on the caller's scratch, which a nested scope
    // would take back. The document and the temporaries go on the scratch too.
    JsonDoc doc;
    out.clips = array_in<AnimClipDesc>(alloc);
    out.sets  = array_in<AnimSheetSet>(alloc);
    if (!json_parse(text, len, mem_scratch(), doc)) { *error = doc.error; return false; }
    const JsonValue& root = doc.root();
    if (!root.is_object()) { *error = "a sheet is one object"; return false; }
    if (!anim_sheet_parse_header(text, len, out.texture, sizeof out.texture, error)) return false;
    if (!image_w || !image_h) { *error = "the texture has no size"; return false; }

    // The cut: a cell size in pixels, or a grid that divides the image.
    f32 dims[2] = {0, 0};
    if (const JsonValue* cell = json_get(doc, root, "cell")) {
        if (json_numbers(doc, cell, dims, 2) != 2 || dims[0] < 1 || dims[1] < 1) { *error = "cell must be [width, height] in pixels"; return false; }
        out.cell_size = vec2{dims[0], dims[1]};
        out.cols = image_w / (u32)dims[0];
        out.rows = image_h / (u32)dims[1];
    } else if (const JsonValue* grid = json_get(doc, root, "grid")) {
        if (json_numbers(doc, grid, dims, 2) != 2 || dims[0] < 1 || dims[1] < 1) { *error = "grid must be [cols, rows]"; return false; }
        out.cols = (u32)dims[0];
        out.rows = (u32)dims[1];
        out.cell_size = vec2{(f32)image_w / (f32)out.cols, (f32)image_h / (f32)out.rows};
    } else { *error = "a sheet has a cell or a grid"; return false; }
    if (!out.cols || !out.rows) { *error = "the cell is larger than the texture"; return false; }
    f32 default_fps = (f32)json_number(json_get(doc, root, "fps"), 8.0);
    if (!(default_fps > 0.0f)) { *error = "fps must be positive"; return false; }
    f32 uw = out.cell_size.x / (f32)image_w, uh = out.cell_size.y / (f32)image_h;

    const JsonValue* clips = json_get(doc, root, "clips");
    if (!clips || !clips->is_object()) { *error = "a sheet has clips, an object of them"; return false; }
    for (const JsonValue* cv = json_first(doc, *clips); cv; cv = json_next(doc, *cv)) {
        if (!cv->is_object()) { *error = "a clip is an object"; return false; }
        // Its cells, as [column, row] pairs, from one of the three spellings.
        Array<u32> cells = array_in<u32>(mem_scratch());
        auto add = [&](f64 c, f64 r) {
            if (c < 0 || r < 0 || c >= out.cols || r >= out.rows) return false;
            cells.push((u32)c); cells.push((u32)r);
            return true;
        };
        if (const JsonValue* list = json_get(doc, *cv, "cells")) {
            if (!list->is_array()) { *error = "cells is an array of [column, row]"; return false; }
            for (const JsonValue* p = json_first(doc, *list); p; p = json_next(doc, *p)) {
                f32 cr[2];
                if (!p->is_array() || json_numbers(doc, p, cr, 2) != 2 || !add(cr[0], cr[1])) { *error = "a cell is [column, row] inside the sheet"; return false; }
            }
        } else if (const JsonValue* col = json_get(doc, *cv, "column")) {
            const JsonValue* rows = json_get(doc, *cv, "rows");
            if (!col->is_number() || !rows || !rows->is_array()) { *error = "a column clip has rows"; return false; }
            for (const JsonValue* r = json_first(doc, *rows); r; r = json_next(doc, *r))
                if (!r->is_number() || !add(col->number(), r->number())) { *error = "a row is outside the sheet"; return false; }
        } else if (const JsonValue* row = json_get(doc, *cv, "row")) {
            const JsonValue* cols = json_get(doc, *cv, "columns");
            if (!row->is_number() || !cols || !cols->is_array()) { *error = "a row clip has columns"; return false; }
            for (const JsonValue* c = json_first(doc, *cols); c; c = json_next(doc, *c))
                if (!c->is_number() || !add(c->number(), row->number())) { *error = "a column is outside the sheet"; return false; }
        } else { *error = "a clip has cells, a column and rows, or a row and columns"; return false; }
        u32 frames = cells.len / 2;
        if (!frames) { *error = "a clip has at least one frame"; return false; }

        f32 fps = (f32)json_number(json_get(doc, *cv, "fps"), default_fps);
        if (!(fps > 0.0f)) { *error = "fps must be positive"; return false; }
        AnimClipDesc d;
        usize nl = strlen(cv->key);
        char* name = (char*)alloc_in(alloc, nl + 1);
        memcpy(name, cv->key, nl + 1);
        d.name = name;
        d.loop = AnimLoop::Loop;
        if (const char* loop = json_string(json_get(doc, *cv, "loop"), nullptr); loop && !anim_loop_parse(loop, &d.loop)) {
            *error = "loop must be once, loop or pingpong"; return false;
        }
        d.duration = (f32)frames / fps;
        if (const JsonValue* dur = json_get(doc, *cv, "duration")) {
            if (!dur->is_number() || dur->number() <= 0) { *error = "duration must be positive"; return false; }
            d.duration = (f32)dur->number();
        }
        // The frames over the duration, as uv step keys; then any event keys.
        const JsonValue* evs = json_get(doc, *cv, "events");
        u32 nev = evs && evs->is_array() ? evs->count() : 0;
        AnimKey* keys = alloc_items<AnimKey>(alloc, frames + nev);
        for (u32 i = 0; i < frames; ++i) {
            AnimKey& k = keys[i];
            k = AnimKey{};
            k.time = d.duration * (f32)i / (f32)frames;
            k.v[0] = (f32)cells[2 * i] * uw; k.v[1] = (f32)cells[2 * i + 1] * uh; k.v[2] = uw; k.v[3] = uh;
        }
        u32 e = 0;
        for (const JsonValue* ev = nev ? json_first(doc, *evs) : nullptr; ev; ev = json_next(doc, *ev), ++e) {
            const JsonValue* t = ev->is_array() ? json_at(doc, *ev, 0) : nullptr;
            const JsonValue* n = ev->is_array() ? json_at(doc, *ev, 1) : nullptr;
            if (!t || !n || !t->is_number() || !n->is_string() || !n->len()) { *error = "an event is [time, name]"; return false; }
            AnimKey& k = keys[frames + e];
            k = AnimKey{};
            k.time = (f32)t->number();
            k.event = name_intern(n->str());
        }
        AnimTrackDesc* tracks = alloc_items<AnimTrackDesc>(alloc, nev ? 2 : 1);
        tracks[0] = AnimTrackDesc{};
        tracks[0].channel = AnimChannel::Uv;
        tracks[0].keys = Span<const AnimKey>(keys, frames);
        if (nev) {
            tracks[1] = AnimTrackDesc{};
            tracks[1].channel = AnimChannel::Event;
            tracks[1].keys = Span<const AnimKey>(keys + frames, nev);
        }
        d.tracks = Span<const AnimTrackDesc>(tracks, nev ? 2 : 1);
        out.clips.push(d);
    }

    if (const JsonValue* sets = json_get(doc, root, "sets")) {
        if (!sets->is_object()) { *error = "sets is an object of sets"; return false; }
        for (const JsonValue* sv = json_first(doc, *sets); sv; sv = json_next(doc, *sv)) {
            if (!sv->is_object()) { *error = "a set is an object of variant: clip"; return false; }
            for (const JsonValue* vv = json_first(doc, *sv); vv; vv = json_next(doc, *vv)) {
                const char* clip = json_string(vv, nullptr);
                u32 at = KV_POOL_NONE;
                for (u32 i = 0; clip && i < out.clips.len; ++i) if (!strcmp(out.clips[i].name, clip)) at = i;
                if (at == KV_POOL_NONE) { *error = "a set names a clip the sheet does not have"; return false; }
                out.sets.push(AnimSheetSet{name_intern(sv->key), name_intern(vv->key), Handle{at}});
            }
        }
    }
    return true;
}

// ---- the records --------------------------------------------------------------------------

namespace {

void* sheet_resolve(kv_handle h, void* ctx) { return ((AnimState_*)ctx)->sheets.get(Handle{h}); }
const char* sheet_name_of(kv_handle h, void* ctx) { AnimSheet* s = ((AnimState_*)ctx)->sheets.get(Handle{h}); return s ? s->path : nullptr; }
kv_handle sheet_find_name(const char* name, void* ctx) {
    const Handle* h = ((AnimState_*)ctx)->sheet_by_path.find(name_lookup(name));
    return h ? h->bits : 0;
}
int sheet_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    AnimSheet* s = ((AnimState_*)ctx)->sheets.get(Handle{h});
    if (!s) return format(out, cap, "stale");
    const char* st = s->state == AssetState::Ready ? "ready" : s->state == AssetState::Failed ? "failed" : "pending";
    return format(out, cap, "'%s' %s, %ux%u cells of %gx%g, %u clips, %u sets", s->path, st, s->cols, s->rows, s->cell_size.x, s->cell_size.y, s->clips.len, s->sets.len);
}

u32* sheet_refs(void* user, Handle h) { AnimSheet* s = ((AnimState_*)user)->sheets.get(h); return s ? &s->refs : nullptr; }
AssetState sheet_state(void* user, Handle h) { AnimSheet* s = ((AnimState_*)user)->sheets.get(h); return s ? s->state : AssetState::Failed; }
const char* sheet_path(void* user, Handle h) { AnimSheet* s = ((AnimState_*)user)->sheets.get(h); return s ? s->path : ""; }

// Lets go of everything the sheet holds.
void release(Engine& e, AnimSheet& sh) {
    asset_unref(e, sh.blob);
    asset_unref(e, sh.texture);
    asset_unref(e, sh.material_asset);
    for (const AnimSheetClip& c : sh.clips) asset_unref(e, c.clip);
    sh.blob = sh.texture = sh.material_asset = Handle{};
    sh.clips.free();
    sh.sets.free();
}

void sheet_retire(Engine& e, void* user, Handle h) {
    AnimState_& s = *(AnimState_*)user;
    AnimSheet* sh = s.sheets.get(h);
    if (!sh) return;
    release(e, *sh);
    s.sheet_by_path.remove(name_lookup(sh->path));
    s.sheets.destroy(h);
}

void fail_sheet(Engine& e, AnimState_& s, Handle h, AnimSheet& sh, AssetError err, const char* why) {
    sh.state = AssetState::Failed;
    sh.error = err;
    utf8_copy(sh.error_text, sizeof sh.error_text, why ? why : "");
    ++s.sheets_failed;
    KV_WARN("anim", "sheet '%s': %s%s%s", sh.path, asset_error_name(err), why ? ": " : "", why ? why : "");
    e.events.emit(AssetFailed{h, err});
}

// A sheet whose file has arrived names its texture; once that has a size, the clips.
void advance(Engine& e, AnimState_& s, Handle h, AnimSheet& sh) {
    const BlobAsset* b = asset_blob(e, sh.blob);
    if (!b || b->state == AssetState::Pending) return;
    if (b->state == AssetState::Failed) { fail_sheet(e, s, h, sh, b->error, "the file"); return; }
    if (sh.texture.is_null()) {
        char texture[KV_ASSET_PATH_MAX];
        const char* why = nullptr;
        if (!anim_sheet_parse_header((const char*)b->data, b->size, texture, sizeof texture, &why)) { fail_sheet(e, s, h, sh, AssetError::Parse, why); return; }
        sh.texture = asset_load_texture(e, texture, Handle{});
        asset_ref(e, sh.texture);
        sh.material_asset = asset_material_for_texture(e, sh.texture, BlendMode::Alpha, TextureFilter::Nearest, Handle{});
        asset_ref(e, sh.material_asset);
        if (const MaterialAsset* m = asset_material(e, sh.material_asset)) sh.material = m->material;
    }
    const TextureAsset* t = asset_texture(e, sh.texture);
    if (!t || t->state == AssetState::Pending) return;
    if (t->state == AssetState::Failed) { fail_sheet(e, s, h, sh, t->error, "the texture"); return; }

    AnimSheetParsed parsed;
    const char* why = nullptr;
    MemScope scope;
    if (!anim_sheet_parse((const char*)b->data, b->size, t->width, t->height, mem_scratch(), parsed, &why)) { fail_sheet(e, s, h, sh, AssetError::Parse, why); return; }
    sh.cell_size = parsed.cell_size;
    sh.cols = parsed.cols;
    sh.rows = parsed.rows;
    for (AnimClipDesc& d : parsed.clips) {
        char name[2 * KV_ASSET_PATH_MAX];
        Name local = name_intern(d.name);
        int n = format(name, sizeof name, "%s#%s", sh.path, d.name);
        if (n < 0 || n >= (int)KV_ASSET_PATH_MAX) { fail_sheet(e, s, h, sh, AssetError::Parse, "a clip's full name is too long"); return; }
        d.name = name;
        Handle clip = anim_create(e, d, Handle{}); // held by the sheet
        asset_ref(e, clip);
        sh.clips.push(AnimSheetClip{local, clip});
    }
    for (const AnimSheetSet& set : parsed.sets) sh.sets.push(AnimSheetSet{set.set, set.variant, sh.clips[(u32)set.clip.bits].clip});
    asset_unref(e, sh.blob); // the clips are made: the text can go
    sh.blob = Handle{};
    sh.state = AssetState::Ready;
    ++s.sheets_loaded;
    KV_INFO("anim", "sheet '%s' ready: %ux%u cells of %gx%g, %u clips, %u sets", sh.path, sh.cols, sh.rows, sh.cell_size.x, sh.cell_size.y, sh.clips.len, sh.sets.len);
    e.events.emit(AssetReady{h});
}

} // namespace

bool anim_sheets_init(Engine& e, AnimState_& s, Allocator* general, MemTag tag) {
    s.sheet_by_path.init(general);
    kv_handle_kind_desc desc{};
    desc.name = "anim.sheet"; desc.type_key = type_key<AnimSheet>(); desc.ctx = &s;
    desc.resolve = sheet_resolve; desc.describe = sheet_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    desc.name_of = sheet_name_of; desc.find_name = sheet_find_name;
    s.sheet_kind = handle_kind_register(desc);
    if (!s.sheet_kind) return false;
    s.sheets.use_kind(s.sheet_kind);
    if (!s.sheets.init(1024, tag)) return false;
    AssetKindDesc k;
    k.kind = s.sheet_kind; k.refs = sheet_refs; k.state = sheet_state; k.path = sheet_path; k.retire = sheet_retire; k.user = &s;
    return asset_kind_register(e, k);
}

void anim_sheets_shutdown(Engine& e, AnimState_& s) {
    asset_kind_unregister(e, s.sheet_kind);
    for (u32 i = 0; i < s.sheets.len; ++i)
        if (s.sheets.is_alive(i)) { s.sheets.items[i].clips.free(); s.sheets.items[i].sets.free(); }
    KV_INFO("anim", "%u sheets loaded, %u failed", s.sheets_loaded, s.sheets_failed);
    kv_handle_kind_unregister(s.sheet_kind);
    s.sheet_by_path.free();
    s.sheets.free();
}

void anim_sheets_pump(Engine& e, AnimState_& s) {
    for (u32 i = 0; i < s.sheets.len; ++i) {
        if (!s.sheets.is_alive(i) || s.sheets.items[i].state != AssetState::Pending) continue;
        advance(e, s, s.sheets.handle_at(i), s.sheets.items[i]);
    }
}

Handle anim_sheet_load(Engine& e, const char* path, Handle scope) {
    AnimState_* s = anim_state(e);
    if (!s || !path) { KV_ERROR("anim", "cannot load sheet '%s': anim is not up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("anim", "path too long: '%s'", path); return Handle{}; }
    Name name = name_intern(path);
    Handle h;
    if (const Handle* existing = s->sheet_by_path.find(name)) h = *existing;
    else {
        AnimSheet sh{};
        utf8_copy(sh.path, sizeof sh.path, path);
        sh.clips.alloc = sh.sets.alloc = s->general;
        h = s->sheets.create(sh);
        if (h.is_null()) return h;
        s->sheet_by_path.set(name, h);
        AnimSheet* rec = s->sheets.get(h);
        rec->blob = asset_load_blob(e, path, Handle{});
        asset_ref(e, rec->blob);
    }
    if (!scope.is_null()) asset_scope_add(e, scope, h);
    return h;
}

const AnimSheet* anim_sheet(Engine& e, Handle sheet) {
    AnimState_* s = anim_state(e);
    return s ? s->sheets.get(sheet) : nullptr;
}

Handle anim_sheet_clip(Engine& e, Handle sheet, const char* name) {
    const AnimSheet* sh = anim_sheet(e, sheet);
    Name n = name ? name_lookup(name) : Name{};
    for (u32 i = 0; sh && !n.is_null() && i < sh->clips.len; ++i)
        if (sh->clips[i].name == n) return sh->clips[i].clip;
    return Handle{};
}

Handle anim_sheet_variant(Engine& e, Handle sheet, const char* set, const char* variant) {
    const AnimSheet* sh = anim_sheet(e, sheet);
    Name a = set ? name_lookup(set) : Name{}, b = variant ? name_lookup(variant) : Name{};
    for (u32 i = 0; sh && !a.is_null() && !b.is_null() && i < sh->sets.len; ++i)
        if (sh->sets[i].set == a && sh->sets[i].variant == b) return sh->sets[i].clip;
    return Handle{};
}

} // namespace kv
