// Fonts (font.h): faces read once per file, strikes per pixel size, glyphs made when first
// asked for into pages that are uploaded where they changed.
#include "korvid/asset/font.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/sort.h"
#include "korvid/asset/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/hash.h"
#include "korvid/core/time.h"
#include "korvid/core/utf8.h"
#include "korvid/core/array.h"
#include "korvid/core/name.h"
#include "korvid/math/scalar.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include "korvid/truetype/truetype.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kMissing = ~0u;     // in a strike's index: no face has it
constexpr u32 kBlock   = 256;     // glyphs to a block

void* font_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->fonts.get(Handle{h}); }
// A font's name is its key, "path@px" (and the fallback list's or the set's hash when given).
const char* font_name_of(kv_handle h, void* ctx) {
    AssetState_& s = *(AssetState_*)ctx;
    for (u32 i = 0; i < s.font_by_key.len(); ++i)
        if (s.font_by_key.values[i].bits == h) return name_str(s.font_by_key.keys[i]);
    return nullptr;
}
kv_handle font_find_name(const char* name, void* ctx) {
    const Handle* h = ((AssetState_*)ctx)->font_by_key.find(name_lookup(name));
    return h ? h->bits : 0;
}
int font_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    FontAsset* f = ((AssetState_*)ctx)->fonts.get(Handle{h});
    if (!f) return format(out, cap, "stale");
    const char* st = f->state == AssetState::Ready ? "ready" : f->state == AssetState::Failed ? "failed" : "pending";
    u32 glyphs = 0, pages = 0;
    font_counts(*f, &glyphs, &pages);
    return format(out, cap, "'%s' %gpx %s, %u glyphs in %u pages", f->path, f->px, st, glyphs, pages);
}

void fail_font(Engine& e, AssetState_& s, Handle h, FontAsset& f, AssetError err, const char* detail) {
    f.state = AssetState::Failed;
    f.error = err;
    f.io    = Handle{};
    ++s.fonts_failed;
    KV_WARN("asset", "font '%s' %gpx: %s%s%s", f.path, f.px, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// ---- faces -------------------------------------------------------------------------------------

FontFace* face_for(Engine& e, AssetState_& s, const char* path, u64 user) {
    for (FontFace* f : s.faces) if (!strcmp(f->path, path)) { ++f->refs; return f; }
    FontFace* f = alloc_items<FontFace>(s.general, 1);
    *f = FontFace{};
    strncpy(f->path, path, sizeof f->path - 1);
    f->refs = 1;
    s.faces.push(f);
    IoQueue* io = platform_io(e);
    IoRequest req;
    req.path  = path;
    req.alloc = s.general;
    req.user  = user;
    req.owner = asset_io_owner(Handle{user}.kind());
    f->io = io ? io->read_asset(req) : Handle{};
    if (f->io.is_null()) { f->failed = true; KV_WARN("asset", "font file '%s': could not submit its read", path); }
    return f;
}

void face_release(Engine& e, AssetState_& s, FontFace* f) {
    if (!f || --f->refs) return;
    IoQueue* io = platform_io(e);
    if (io && !f->io.is_null()) io->cancel(f->io);
    free_in(s.general, f->data, f->size + 1); // a read's bytes: the file and its NUL
    free_in(s.general, f->info, sizeof(stbtt_fontinfo));
    for (u32 i = 0; i < s.faces.len; ++i)
        if (s.faces[i] == f) { s.faces[i] = s.faces[s.faces.len - 1]; --s.faces.len; break; }
    free_items(s.general, f, 1);
}

// The file is in: parsed, or failed. True when it can be drawn from.
bool face_take(AssetState_& s, FontFace& f, u8* data, usize size) {
    f.io = Handle{};
    f.data = data;
    f.size = size;
    stbtt_fontinfo* info = alloc_items<stbtt_fontinfo>(s.general, 1);
    *info = stbtt_fontinfo{};
    if (!stbtt_InitFont(info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        free_items(s.general, info, 1);
        f.failed = true;
        KV_WARN("asset", "font file '%s': not a TrueType font", f.path);
        return false;
    }
    f.info = info;
    f.ready = true;
    return true;
}

// ---- strikes and pages --------------------------------------------------------------------------

FontPage* new_page(FontStrike& st, const char* name) {
    FontPage p{};
    usize bytes = (usize)KV_FONT_PAGE * KV_FONT_PAGE * 4;
    p.rgba = (u8*)alloc_in(st.alloc, bytes, 16);
    if (!p.rgba) return nullptr;
    // White everywhere and clear: a glyph's edge blends toward white, never black.
    for (usize i = 0; i < bytes; i += 4) { p.rgba[i] = p.rgba[i + 1] = p.rgba[i + 2] = 255; p.rgba[i + 3] = 0; }
    if (st.drawn && st.drawn->create) {
        char tname[KV_ASSET_PATH_MAX + 48];
        format(tname, sizeof tname, "%s:page%u", name, st.pages.len);
        if (!st.drawn->create(st.drawn->user, tname, &p.texture, &p.material)) p.texture = p.material = Handle{};
    }
    p.dirty_x0 = p.dirty_y0 = 0;
    p.dirty_x1 = p.dirty_y1 = KV_FONT_PAGE; // the first upload is the whole page
    st.pages.push(p);
    return &st.pages.last();
}

// Room for a w x h box: a page, and where in it. Shelves left to right, a pixel between.
FontPage* place(FontStrike& st, const char* name, u32 w, u32 h, u32* x, u32* y) {
    if (w > KV_FONT_PAGE || h > KV_FONT_PAGE) return nullptr;
    FontPage* p = st.pages.len ? &st.pages.last() : new_page(st, name);
    if (!p) return nullptr;
    if (p->x + w > KV_FONT_PAGE) { p->x = 0; p->shelf_y += p->shelf_h + 1; p->shelf_h = 0; }
    if (p->shelf_y + h > KV_FONT_PAGE) {
        p = new_page(st, name);
        if (!p) return nullptr;
    }
    *x = p->x;
    *y = p->shelf_y;
    p->x += w + 1;
    if (h > p->shelf_h) p->shelf_h = h;
    return p;
}

Glyph* new_glyph(FontStrike& st, u32* number) {
    if (st.count % kBlock == 0) {
        Glyph* b = alloc_items<Glyph>(st.alloc, kBlock);
        if (!b) return nullptr;
        st.blocks.push(b);
    }
    *number = st.count++;
    Glyph* g = &st.blocks[*number / kBlock][*number % kBlock];
    *g = Glyph{};
    return g;
}

// Coverage as stb_truetype gives it. How heavy it reads is the drawing's to correct, by
// the ink's luminance (the UI shader): a curve here would be the same for light ink and dark.
u8 crisp(u8 c, f32) { return c; }

// Makes the glyph for (cp, quarter), the lock held. The glyph's number + 1, kMissing when no
// face has it, or 0 when a face that might is still loading (nothing cached).
u32 make_locked(const FontAsset& f, FontStrike& st, u32 cp, u32 quarter) {
    u64 key = (u64)cp << 2 | quarter;
    if (const u32* at = st.index.find(key)) return *at;
    bool pending = false;
    for (u32 fi = 0; fi < st.face_count; ++fi) {
        FontFace* face = st.faces[fi];
        if (!face->ready) { if (!face->failed) pending = true; continue; }
        stbtt_fontinfo* info = (stbtt_fontinfo*)face->info;
        int gi = stbtt_FindGlyphIndex(info, (int)cp);
        if (!gi) continue;
        f32 scale = stbtt_ScaleForPixelHeight(info, st.px);
        f32 shift = (f32)quarter * 0.25f;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(info, gi, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
        u32 number = 0;
        Glyph* g = new_glyph(st, &number);
        if (!g) return 0;
        g->advance = (f32)adv * scale;
        u32 w = (u32)(x1 - x0), h = (u32)(y1 - y0);
        if (x1 > x0 && y1 > y0) {
            u32 px = 0, py = 0;
            FontPage* page = place(st, f.path, w, h, &px, &py);
            if (page) {
                MemScope scope;
                u8* cov = (u8*)alloc_in(mem_scratch(), (usize)w * h, 1);
                stbtt_MakeGlyphBitmapSubpixel(info, cov, (int)w, (int)h, (int)w, scale, scale, shift, 0.0f, gi);
                for (u32 r = 0; r < h; ++r) {
                    u8* row = page->rgba + ((usize)(py + r) * KV_FONT_PAGE + px) * 4;
                    for (u32 c = 0; c < w; ++c) row[c * 4 + 3] = crisp(cov[r * w + c], st.px);
                }
                if (px < page->dirty_x0) page->dirty_x0 = px;
                if (py < page->dirty_y0) page->dirty_y0 = py;
                if (px + w > page->dirty_x1) page->dirty_x1 = px + w;
                if (py + h > page->dirty_y1) page->dirty_y1 = py + h;
                f32 inv = 1.0f / (f32)KV_FONT_PAGE;
                g->uv       = rect2{(f32)px * inv, (f32)py * inv, (f32)w * inv, (f32)h * inv};
                g->size     = vec2{(f32)w, (f32)h};
                g->offset   = vec2{(f32)x0 + (f32)w * 0.5f, -((f32)y0 + (f32)h * 0.5f)}; // stb's y is down from the baseline
                g->texture  = page->texture;
                g->material = page->material;
            }
        } else if (st.pages.len) {
            g->texture  = st.pages[0].texture;
            g->material = st.pages[0].material;
        }
        st.index.set(key, number + 1);
        if (quarter == 0 && cp >= 32 && cp < 127) atomic_store(&st.ascii[cp - 32], (const Glyph*)g, MO_RELEASE);
        return number + 1;
    }
    if (pending) return 0;
    st.index.set(key, kMissing);
    if (f.misses++ == 0) KV_WARN("asset", "font '%s' %gpx: no face has U+%04X", f.path, f.px, cp);
    return kMissing;
}

const Glyph* glyph_of(FontStrike& st, u32 at) { return at && at != kMissing ? &st.blocks[(at - 1) / kBlock][(at - 1) % kBlock] : nullptr; }

struct KernPair { u64 key; f32 value; };
int compare_kern(const void* a, const void* b) {
    u64 x = ((const KernPair*)a)->key, y = ((const KernPair*)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}
struct GlyphAt { int glyph; u32 index; };
int compare_glyph_at(const void* a, const void* b) {
    const GlyphAt& x = *(const GlyphAt*)a; const GlyphAt& y = *(const GlyphAt*)b;
    return x.glyph != y.glyph ? (x.glyph < y.glyph ? -1 : 1) : (x.index < y.index ? -1 : x.index > y.index ? 1 : 0);
}
u32 first_of(const GlyphAt* list, u32 n, int glyph) {
    u32 lo = 0, hi = n;
    while (lo < hi) { u32 mid = (lo + hi) / 2; if (list[mid].glyph < glyph) lo = mid + 1; else hi = mid; }
    return lo;
}

// Pairwise kerning is quadratic in the set; past this many glyphs only the first ones
// (by code point: ASCII and the Latin blocks, where kerning matters) are paired.
constexpr u32 kPairwiseMax = 512;

constinit const FontRange k_default_ranges[] = {
    {0x0020, 0x007E}, // printable ASCII
    {0x00A0, 0x00FF}, // Latin-1 Supplement
    {0x0100, 0x017F}, // Latin Extended-A
    {0x2010, 0x2027}, // dashes, quotes, the bullet, the ellipsis
    {0x2030, 0x203A}, // per mille, primes, single angle quotes
    {0x20AC, 0x20AC}, // euro
    {0x2122, 0x2122}, // trade mark
    {0xFFFD, 0xFFFD}, // the replacement character
};

// The first face is in: the strike's metrics (rounded to whole pixels), its kerning over the
// Latin set, and the glyphs asked for at load.
bool build_strike(AssetState_& s, FontAsset& f, const char** why) {
    f64 t0 = time_now();
    FontStrike& st = *f.strike;
    stbtt_fontinfo* info = (stbtt_fontinfo*)st.faces[0]->info;
    f32 scale = stbtt_ScaleForPixelHeight(info, f.px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    f.ascent      = (f32)(i32)((f32)asc * scale + 0.5f);
    f.descent     = -(f32)(i32)(-(f32)desc * scale + 0.5f);
    f.line_height = (f32)(i32)((f32)(asc - desc + gap) * scale + 0.5f);

    MemScope scope;
    u32 wanted = 0;
    for (const FontRange& r : k_default_ranges) wanted += r.last - r.first + 1;
    int* cps = (int*)alloc_in(mem_scratch(), sizeof(int) * wanted);
    int* gis = (int*)alloc_in(mem_scratch(), sizeof(int) * wanted);
    u32 n = 0;
    for (const FontRange& r : k_default_ranges)
        for (u32 cp = r.first; cp <= r.last; ++cp)
            if (int gi = stbtt_FindGlyphIndex(info, (int)cp)) { cps[n] = (int)cp; gis[n] = gi; ++n; }
    if (n == 0 && !f.range_count) { *why = "the font has none of the Latin set"; }
    Array<KernPair> pairs;
    pairs.alloc = mem_scratch();
    if (int len = stbtt_GetKerningTableLength(info)) {
        stbtt_kerningentry* table = (stbtt_kerningentry*)alloc_in(mem_scratch(), sizeof(stbtt_kerningentry) * (usize)len);
        len = stbtt_GetKerningTable(info, table, len);
        GlyphAt* by_glyph = (GlyphAt*)alloc_in(mem_scratch(), sizeof(GlyphAt) * (n ? n : 1));
        for (u32 i = 0; i < n; ++i) by_glyph[i] = GlyphAt{gis[i], i};
        kv::sort(by_glyph, n, sizeof(GlyphAt), compare_glyph_at);
        for (int k = 0; k < len; ++k) {
            if (table[k].advance == 0) continue;
            f32 v = (f32)table[k].advance * scale;
            for (u32 a = first_of(by_glyph, n, table[k].glyph1); a < n && by_glyph[a].glyph == table[k].glyph1; ++a)
                for (u32 b = first_of(by_glyph, n, table[k].glyph2); b < n && by_glyph[b].glyph == table[k].glyph2; ++b)
                    pairs.push(KernPair{(u64)cps[by_glyph[a].index] << 32 | (u32)cps[by_glyph[b].index], v});
        }
    } else {
        u32 m = n < kPairwiseMax ? n : kPairwiseMax;
        for (u32 a = 0; a < m; ++a)
            for (u32 b = 0; b < m; ++b)
                if (int k = stbtt_GetGlyphKernAdvance(info, gis[a], gis[b]))
                    pairs.push(KernPair{(u64)cps[a] << 32 | (u32)cps[b], (f32)k * scale});
    }
    if (pairs.len) {
        kv::sort(pairs.data, pairs.len, sizeof(KernPair), compare_kern);
        u32 m = 0;
        for (u32 i = 0; i < pairs.len; ++i) if (!m || pairs[m - 1].key != pairs[i].key) pairs[m++] = pairs[i];
        f.kern_pairs  = (u64*)alloc_in(s.general, sizeof(u64) * m);
        f.kern_values = (f32*)alloc_in(s.general, sizeof(f32) * m);
        for (u32 i = 0; i < m; ++i) { f.kern_pairs[i] = pairs[i].key; f.kern_values[i] = pairs[i].value; }
        f.kern_count = m;
    }
    // What was asked for at load.
    os_mutex_lock(&st.lock);
    for (u32 r = 0; r < f.range_count; ++r)
        for (u32 cp = f.ranges[r].first; cp <= f.ranges[r].last; ++cp) make_locked(f, st, cp, 0);
    os_mutex_unlock(&st.lock);
    f.atlas_width = f.atlas_height = KV_FONT_PAGE;
    f.build_seconds = time_now() - t0;
    return true;
}

void free_strike(Engine& e, AssetState_& s, FontAsset& f) {
    FontStrike* st = f.strike;
    if (!st) return;
    for (FontPage& p : st->pages) {
        if (s.font_pages.destroy && !p.texture.is_null()) s.font_pages.destroy(s.font_pages.user, p.texture, p.material);
        free_in(st->alloc, p.rgba, (usize)KV_FONT_PAGE * KV_FONT_PAGE * 4);
    }
    st->pages.free();
    for (Glyph* b : st->blocks) free_items(st->alloc, b, kBlock);
    st->blocks.free();
    st->index.free();
    for (u32 i = 0; i < st->face_count; ++i) face_release(e, s, st->faces[i]);
    os_mutex_destroy(&st->lock);
    free_in(s.general, f.kern_pairs, sizeof(u64) * f.kern_count);
    free_in(s.general, f.kern_values, sizeof(f32) * f.kern_count);
    f.kern_pairs = nullptr; f.kern_values = nullptr; f.kern_count = 0;
    free_items(s.general, st, 1);
    f.strike = nullptr;
}

int compare_range(const void* a, const void* b) {
    const FontRange& x = *(const FontRange*)a; const FontRange& y = *(const FontRange*)b;
    return x.first != y.first ? (x.first < y.first ? -1 : 1) : (x.last < y.last ? -1 : x.last > y.last ? 1 : 0);
}

} // namespace

bool asset_fonts_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.font_by_key.init(&self.general);
    s.faces.alloc = &self.general;
    s.fallback.alloc = &self.general;
    kv_handle_kind_desc desc{};
    desc.name = "asset.font"; desc.type_key = type_key<FontAsset>(); desc.ctx = &s;
    desc.resolve = font_resolve; desc.name_of = font_name_of; desc.find_name = font_find_name; desc.describe = font_describe; desc.flags = KV_HANDLE_GENERATIONAL;
    s.font_kind = handle_kind_register(desc);
    if (!s.font_kind) return false;
    s.fonts.use_kind(s.font_kind);
    return s.fonts.init(256, self.general.tag);
}

void asset_fonts_shutdown(Engine& e, AssetState_& s) {
    IoQueue* io = platform_io(e);
    for (u32 i = 0; i < s.fonts.len; ++i) {
        if (!s.fonts.is_alive(i)) continue;
        FontAsset& f = s.fonts.items[i];
        if (io && !f.io.is_null()) io->cancel(f.io);
        free_strike(e, s, f);
        free_in(s.general, f.ranges, sizeof(FontRange) * f.range_count);
    }
    while (s.faces.len) { s.faces[0]->refs = 1; face_release(e, s, s.faces[0]); }
    s.faces.free();
    for (char* p : s.fallback) free_in(s.general, p, strlen(p) + 1);
    s.fallback.free();
    KV_INFO("asset", "%u fonts loaded, %u failed", s.fonts_loaded, s.fonts_failed);
    kv_handle_kind_unregister(s.font_kind);
    s.font_by_key.free();
    s.fonts.free();
}

// Phase PreSim, main thread, after the texture pump: font files that arrived are taken;
// strikes whose first face is in become Ready; strikes a fallback landed for draw anew.
void asset_fonts_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    bool landed = false;
    // Faces first, by their request: a face outlives the font whose handle it was read under.
    for (u32 i = 0; i < io->completions(asset_io_owner(s.font_kind)).len;) {
        IoCompletion c = io->completions(asset_io_owner(s.font_kind))[i];
        FontFace* face = nullptr;
        for (FontFace* f : s.faces) if (!f->io.is_null() && f->io == c.request) face = f;
        if (!face) { ++i; continue; }
        io->reap(c.request);
        if (c.status != IoStatus::Done) {
            face->io = Handle{};
            face->failed = true;
            face->missing = c.not_found();
            KV_WARN("asset", "font file '%s': %s", face->path, face->missing ? "not found" : "could not be read");
            c.bytes.free();
        } else if (!face_take(s, *face, (u8*)c.bytes.data, c.bytes.size)) {
            c.bytes.free();
            face->data = nullptr;
            face->size = 0;
        }
        landed = true;
    }
    asset_reap_orphans(e, s, s.font_kind);
    // Every pending strike is looked at (its face may have been read for another size);
    // ready ones only when a file landed.
    for (u32 i = 0; i < s.fonts.len; ++i) {
        if (!s.fonts.is_alive(i)) continue;
        FontAsset& f = s.fonts.items[i];
        FontStrike* st = f.strike;
        if (!st) continue;
        Handle h = s.fonts.handle_at(i);
        if (f.state == AssetState::Pending) {
            FontFace* first = st->faces[0];
            if (first->failed) {
                if (first->missing) fail_font(e, s, h, f, AssetError::NotFound, nullptr);
                else fail_font(e, s, h, f, AssetError::Decode, first->data ? "not a TrueType font" : "its file could not be read");
                continue;
            }
            if (!first->ready) continue;
            const char* why = nullptr;
            if (!build_strike(s, f, &why)) { fail_font(e, s, h, f, AssetError::Decode, why); continue; }
            f.state = AssetState::Ready;
            ++s.fonts_loaded;
            KV_INFO("asset", "font '%s' %gpx ready: %u faces, line %.0f, %u kerning pairs, in %.1f ms", f.path, f.px, st->face_count,
                    f.line_height, f.kern_count, f.build_seconds * 1e3);
            e.events.emit(AssetReady{h});
        } else if (f.state == AssetState::Ready && landed) {
            // A fallback that landed may have what was missing: forget the misses, and say so.
            os_mutex_lock(&st->lock);
            bool forget = false;
            for (u32 k = 0; k < st->index.len(); ++k) if (st->index.values[k] == kMissing) forget = true;
            if (forget) {
                Map<u64, u32> kept;
                kept.init(st->alloc);
                for (u32 k = 0; k < st->index.len(); ++k) if (st->index.values[k] != kMissing) kept.set(st->index.keys[k], st->index.values[k]);
                st->index.free();
                st->index = kept;
            }
            os_mutex_unlock(&st->lock);
            atomic_add_fetch(&st->generation, 1u, MO_RELEASE);
        }
    }
}

// Phase Frame, main thread, after the systems that draw text: the pages that changed go to
// the renderer (AssetFontPages::upload), by the rectangle that changed.
void asset_fonts_flush(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    const AssetFontPages& up = s.font_pages;
    if (!up.upload) return;
    for (u32 i = 0; i < s.fonts.len; ++i) {
        if (!s.fonts.is_alive(i)) continue;
        FontStrike* st = s.fonts.items[i].strike;
        if (!st) continue;
        os_mutex_lock(&st->lock);
        for (FontPage& p : st->pages) {
            if (p.dirty_x1 <= p.dirty_x0 || p.dirty_y1 <= p.dirty_y0 || p.texture.is_null()) continue;
            if (!p.uploaded) p.uploaded = up.upload(up.user, p.texture, p.rgba, KV_FONT_PAGE, true, 0, 0, KV_FONT_PAGE, KV_FONT_PAGE);
            else up.upload(up.user, p.texture, p.rgba, KV_FONT_PAGE, false, p.dirty_x0, p.dirty_y0, p.dirty_x1 - p.dirty_x0, p.dirty_y1 - p.dirty_y0);
            p.dirty_x0 = p.dirty_y0 = ~0u;
            p.dirty_x1 = p.dirty_y1 = 0;
        }
        os_mutex_unlock(&st->lock);
    }
}

Span<const FontRange> font_default_ranges() { return Span<const FontRange>(k_default_ranges, KV_COUNTOF(k_default_ranges)); }

void asset_font_pages(Engine& e, const AssetFontPages* pages) {
    AssetState_* s = asset_state(e);
    if (!s) return;
    s->font_pages = pages ? *pages : AssetFontPages{};
}

void asset_set_font_fallback(Engine& e, Span<const char* const> paths) {
    AssetState_* s = asset_state(e);
    if (!s) return;
    for (char* p : s->fallback) free_in(s->general, p, strlen(p) + 1);
    s->fallback.clear();
    for (const char* p : paths) {
        if (!p || s->fallback.len == KV_FONT_MAX_FALLBACK) continue;
        usize n = strlen(p) + 1;
        char* c = (char*)alloc_in(s->general, n, 1);
        memcpy(c, p, n);
        s->fallback.push(c);
    }
}

Handle asset_load_font(Engine& e, const char* path, f32 px) {
    FontDesc d;
    d.px = px;
    return asset_load_font(e, path, d);
}

Handle asset_load_font(Engine& e, const char* path, const FontDesc& desc) { return asset_load_font(e, path, desc, asset_scope_global(e)); }

Handle asset_load_font(Engine& e, const char* path, const FontDesc& desc, Handle scope) {
    AssetState_* s = asset_state(e);
    if (!s) { KV_ERROR("asset", "cannot load font '%s': the asset module is not up", path ? path : "?"); return Handle{}; }
    return asset_hold(*s, scope, font_load(e, *s, path, desc));
}

void font_retire(Engine& e, AssetState_& s, Handle h) {
    FontAsset* f = s.fonts.get(h);
    if (!f) return;
    IoQueue* io = platform_io(e);
    if (io && !f->io.is_null()) io->cancel(f->io);
    free_strike(e, s, *f);
    free_in(s.general, f->ranges, sizeof(FontRange) * f->range_count);
    for (u32 i = 0; i < s.font_by_key.len(); ++i)
        if (s.font_by_key.values[i] == h) { s.font_by_key.remove(s.font_by_key.keys[i]); break; }
    KV_INFO("asset", "font '%s' %gpx retired", f->path, f->px);
    s.fonts.destroy(h);
}

Handle font_load(Engine& e, AssetState_& st, const char* path, const FontDesc& desc) {
    AssetState_* s = &st;
    IoQueue* io = platform_io(e);
    f32 px = desc.px;
    if (!io || !path) { KV_ERROR("asset", "cannot load font '%s': asset and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    if (!(px >= 4.0f && px <= 256.0f)) { KV_ERROR("asset", "font '%s': size %g is outside [4, 256]", path, px); return Handle{}; }

    // What is made at load, merged: sorted by first, overlapping and adjacent ranges joined.
    MemScope scope;
    FontRange* merged = (FontRange*)alloc_in(mem_scratch(), sizeof(FontRange) * (desc.ranges.len ? desc.ranges.len : 1));
    u32 nr = 0;
    for (const FontRange& rg : desc.ranges)
        if (rg.first <= rg.last && rg.last <= 0x10FFFF) merged[nr++] = rg;
    kv::sort(merged, nr, sizeof(FontRange), compare_range);
    u32 m = 0;
    u64 total = 0;
    for (u32 i = 0; i < nr; ++i) {
        if (m && merged[i].first <= merged[m - 1].last + 1) { if (merged[i].last > merged[m - 1].last) merged[m - 1].last = merged[i].last; }
        else merged[m++] = merged[i];
    }
    for (u32 i = 0; i < m; ++i) total += merged[i].last - merged[i].first + 1;
    if (total > KV_FONT_MAX_GLYPHS) { KV_ERROR("asset", "font '%s': %llu code points at load, more than %u", path, (unsigned long long)total, KV_FONT_MAX_GLYPHS); return Handle{}; }

    // The faces: the font, then its fallback list (its own, or the engine's).
    const char* fallback[KV_FONT_MAX_FALLBACK];
    u32 nf = 0;
    if (desc.fallback.len) { for (const char* p : desc.fallback) if (p && nf < KV_FONT_MAX_FALLBACK && strcmp(p, path)) fallback[nf++] = p; }
    else for (const char* p : s->fallback) if (nf < KV_FONT_MAX_FALLBACK && strcmp(p, path)) fallback[nf++] = p;

    // The key: path and size, then the fallback list and the set when there are any.
    char key[KV_ASSET_PATH_MAX + 64];
    i32 kn = format(key, sizeof key, "%s@%g", path, px);
    if (nf) {
        u64 h = 0;
        for (u32 i = 0; i < nf; ++i) h = hash_mix(h ^ hash_bytes(fallback[i], strlen(fallback[i])));
        kn += format(key + kn, sizeof key - (usize)kn, "+%08llx", (unsigned long long)(h & 0xffffffffull));
    }
    if (m) format(key + kn, sizeof key - (usize)kn, "#%016llx", (unsigned long long)hash_bytes(merged, sizeof(FontRange) * m));
    Name name = name_intern(key);
    if (const Handle* existing = s->font_by_key.find(name)) return *existing;

    FontAsset f{};
    strncpy(f.path, path, sizeof f.path - 1);
    f.px    = px;
    f.state = AssetState::Pending;
    if (m) {
        f.ranges = (FontRange*)alloc_in(s->general, sizeof(FontRange) * m);
        memcpy(f.ranges, merged, sizeof(FontRange) * m);
        f.range_count = m;
    }
    Handle h = s->fonts.create(f);
    if (h.is_null()) { free_in(s->general, f.ranges, sizeof(FontRange) * m); return Handle{}; }
    FontAsset* rec = s->fonts.get(h);
    FontStrike* strike = alloc_items<FontStrike>(s->general, 1);
    *strike = FontStrike{};
    strike->alloc = s->general;
    strike->drawn = &s->font_pages;
    strike->px = px;
    strike->index.init(s->general);
    strike->blocks.alloc = s->general;
    strike->pages.alloc = s->general;
    os_mutex_init(&strike->lock);
    strike->faces[strike->face_count++] = face_for(e, *s, path, h.bits);
    for (u32 i = 0; i < nf; ++i) strike->faces[strike->face_count++] = face_for(e, *s, fallback[i], h.bits);
    rec->strike = strike;
    // The first page now, so the font's texture is valid from load on.
    if (FontPage* p = new_page(*strike, key)) { rec->texture = p->texture; rec->material = p->material; }
    rec->atlas_width = rec->atlas_height = KV_FONT_PAGE;
    s->font_by_key.set(name, h);
    // A face read before (another size of the same file) is already in: ready next pump.
    return h;
}

const FontAsset* asset_font(Engine& e, Handle asset) {
    AssetState_* s = asset_state(e);
    return s ? s->fonts.get(asset) : nullptr;
}

void font_index(FontAsset& f) {
    for (u16& a : f.ascii) a = 0;
    f.replacement = 0;
    for (u32 i = 0; i < f.glyph_count; ++i) {
        u32 cp = f.codepoints[i];
        if (cp >= 32 && cp < 127) f.ascii[cp - 32] = (u16)(i + 1);
        if (cp == KV_UTF8_REPLACEMENT) f.replacement = i + 1;
    }
}

const Glyph* font_glyph_lookup(const FontAsset& f, u32 cp) {
    if (f.state != AssetState::Ready) return nullptr;
    if (f.strike) return font_strike_glyph(f, cp, 0, false);
    u32 lo = 0, hi = f.glyph_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (f.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < f.glyph_count && f.codepoints[lo] == cp ? &f.glyphs[lo] : nullptr;
}

const Glyph* font_strike_glyph(const FontAsset& f, u32 cp, u32 quarter, bool replace) {
    FontStrike& st = *f.strike;
    quarter &= 3;
    if (quarter == 0 && cp >= 32 && cp < 127)
        if (const Glyph* g = atomic_load(&st.ascii[cp - 32], MO_ACQUIRE)) return g;
    os_mutex_lock(&st.lock);
    u32 at = make_locked(f, st, cp, quarter);
    if ((at == kMissing || at == 0) && replace && cp != KV_UTF8_REPLACEMENT) at = make_locked(f, st, KV_UTF8_REPLACEMENT, quarter);
    const Glyph* g = glyph_of(st, at);
    os_mutex_unlock(&st.lock);
    return g;
}

const Glyph* font_glyph_or_replacement(const FontAsset& f, u32 cp) {
    if (f.state != AssetState::Ready) return nullptr;
    if (f.strike) return font_strike_glyph(f, cp, 0, true);
    if (const Glyph* g = font_glyph(f, cp)) return g;
    if (f.misses++ == 0) KV_WARN("asset", "font '%s' %gpx has no glyph for U+%04X%s", f.path, f.px, cp, f.replacement ? "; drawing U+FFFD" : "");
    return f.replacement ? &f.glyphs[f.replacement - 1] : nullptr;
}

const Glyph* font_glyph_at(const FontAsset& f, u32 cp, u32 quarter) {
    if (f.state != AssetState::Ready) return nullptr;
    if (f.strike) return font_strike_glyph(f, cp, quarter, true);
    return font_glyph_or_replacement(f, cp);
}

u32 font_generation(const FontAsset& f) { return f.strike ? atomic_load(&f.strike->generation, MO_ACQUIRE) : 0; }

void font_counts(const FontAsset& f, u32* glyphs, u32* pages) {
    if (!f.strike) { *glyphs = f.glyph_count; *pages = f.glyph_count ? 1 : 0; return; }
    os_mutex_lock(&f.strike->lock);
    *glyphs = f.strike->count;
    *pages = f.strike->pages.len;
    os_mutex_unlock(&f.strike->lock);
}

f32 font_kern(const FontAsset& f, u32 a, u32 b) {
    if (!f.kern_count) return 0.0f;
    u64 key = (u64)a << 32 | b;
    u32 lo = 0, hi = f.kern_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (f.kern_pairs[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < f.kern_count && f.kern_pairs[lo] == key ? f.kern_values[lo] : 0.0f;
}

f32 font_measure(const FontAsset& f, const char* str, f32 scale) {
    if (f.state != AssetState::Ready || !str) return 0.0f;
    f32 x = 0.0f;
    const char* p = str;
    u32 prev = 0;
    while (u32 cp = utf8_next(p)) {
        if (prev) x += font_kern(f, prev, cp);
        if (const Glyph* g = font_glyph_or_replacement(f, cp)) x += g->advance;
        prev = cp;
    }
    return x * scale;
}

} // namespace kv
