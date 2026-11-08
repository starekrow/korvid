#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/hash.h"
#include "korvid/core/time.h"
#include "korvid/core/utf8.h"
#include "korvid/core/array.h"
#include "korvid/core/name.h"
#include "korvid/platform/io.h"
#include "korvid/render/renderer.h"
#include "asset_internal.h"
#include "vendor/stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

void* font_resolve(kv_handle h, void* ctx) { return ((AssetState_*)ctx)->fonts.get(Handle{h}); }
int font_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    FontAsset* f = ((AssetState_*)ctx)->fonts.get(Handle{h});
    if (!f) return snprintf(out, cap, "stale");
    const char* st = f->state == AssetState::Ready ? "ready" : f->state == AssetState::Failed ? "failed" : "pending";
    return snprintf(out, cap, "'%s' %gpx %s atlas %ux%u", f->path, f->px, st, f->atlas_width, f->atlas_height);
}

void fail_font(Engine& e, AssetState_& s, Handle h, FontAsset& f, AssetError err, const char* detail) {
    f.state = AssetState::Failed;
    f.error = err;
    f.io    = Handle{};
    ++s.fonts_failed;
    KV_WARN("asset", "font '%s' %gpx: %s%s%s", f.path, f.px, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// What a font's tables occupy in the module's allocator, for the free.
void free_tables(AssetState_& s, FontAsset& f) {
    free_in(s.general, f.codepoints, sizeof(u32) * f.glyph_count);
    free_in(s.general, f.glyphs, sizeof(Glyph) * f.glyph_count);
    free_in(s.general, f.kern_pairs, sizeof(u64) * f.kern_count);
    free_in(s.general, f.kern_values, sizeof(f32) * f.kern_count);
    f.codepoints = nullptr; f.glyphs = nullptr; f.kern_pairs = nullptr; f.kern_values = nullptr;
    f.glyph_count = f.kern_count = 0;
}

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
// The first entry of a glyph in a sorted GlyphAt list, or n.
u32 first_of(const GlyphAt* list, u32 n, int glyph) {
    u32 lo = 0, hi = n;
    while (lo < hi) { u32 mid = (lo + hi) / 2; if (list[mid].glyph < glyph) lo = mid + 1; else hi = mid; }
    return lo;
}

// Pairwise kerning is quadratic in the set; past this many glyphs only the first ones
// (by code point: ASCII and the Latin blocks, where kerning matters) are paired.
constexpr u32 kPairwiseMax = 512;

// Rasterizes the covered code points of the font's set at f.px into the smallest atlas of
// a fixed ladder that holds them, fills the glyph table and the kerning pairs, and
// uploads the atlas. False with the reason on a file stb_truetype cannot read.
bool build_font(Engine& e, AssetState_& s, FontAsset& f, const u8* data, usize size, const char** why) {
    KV_UNUSED(size);
    f64 t0 = time_now();
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) { *why = "not a TrueType font"; return false; }
    f32 scale = stbtt_ScaleForPixelHeight(&info, f.px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);

    MemScope scope;
    // The code points the font has, ascending (the ranges are merged and sorted).
    u32 wanted = 0;
    for (u32 r = 0; r < f.range_count; ++r) wanted += f.ranges[r].last - f.ranges[r].first + 1;
    int* cps = (int*)alloc_in(mem_scratch(), sizeof(int) * wanted);
    int* gis = (int*)alloc_in(mem_scratch(), sizeof(int) * wanted);
    u32 n = 0;
    for (u32 r = 0; r < f.range_count; ++r)
        for (u32 cp = f.ranges[r].first; cp <= f.ranges[r].last; ++cp)
            if (int gi = stbtt_FindGlyphIndex(&info, (int)cp)) { cps[n] = (int)cp; gis[n] = gi; ++n; }
    if (n == 0) { *why = "the font has none of the character set"; return false; }

    stbtt_packedchar* packed = (stbtt_packedchar*)alloc_in(mem_scratch(), sizeof(stbtt_packedchar) * n);
    static const u32 k_ladder[][2] = {{256, 128}, {256, 256}, {512, 256}, {512, 512}, {1024, 512}, {1024, 1024}, {2048, 1024}, {2048, 2048}};
    u8* pixels = nullptr;
    u32 w = 0, h = 0;
    for (const u32* dims : k_ladder) {
        w = dims[0]; h = dims[1];
        pixels = (u8*)alloc_in(mem_scratch(), (usize)w * h);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, pixels, (int)w, (int)h, 0, 1, nullptr)) { *why = "packer init failed"; return false; }
        stbtt_PackSetOversampling(&pc, 1, 1);
        stbtt_pack_range pr{};
        pr.font_size = f.px;
        pr.array_of_unicode_codepoints = cps;
        pr.num_chars = (int)n;
        pr.chardata_for_range = packed;
        int ok = stbtt_PackFontRanges(&pc, data, 0, &pr, 1);
        stbtt_PackEnd(&pc);
        if (ok) break;
        pixels = nullptr;
    }
    if (!pixels) { *why = "glyphs do not fit a 2048x2048 atlas"; return false; }

    // White with coverage in alpha, so the material's tint (and the sprite's) is the color.
    u8* rgba = (u8*)alloc_in(mem_scratch(), (usize)w * h * 4);
    for (usize i = 0; i < (usize)w * h; ++i) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = pixels[i]; }
    Renderer* r = renderer_get(e);
    if (!r || !renderer_texture_upload(r, f.texture, w, h, rgba, (usize)w * h * 4)) { *why = "upload failed"; return false; }

    f.glyph_count = n;
    f.codepoints  = (u32*)alloc_in(s.general, sizeof(u32) * n);
    f.glyphs      = (Glyph*)alloc_in(s.general, sizeof(Glyph) * n);
    for (u32 i = 0; i < n; ++i) {
        const stbtt_packedchar& p = packed[i];
        Glyph& g = f.glyphs[i];
        f32 gw = (f32)(p.x1 - p.x0), gh = (f32)(p.y1 - p.y0);
        g.uv      = rect2{(f32)p.x0 / (f32)w, (f32)p.y0 / (f32)h, gw / (f32)w, gh / (f32)h};
        g.size    = vec2{gw, gh};
        g.offset  = vec2{p.xoff + gw * 0.5f, -(p.yoff + gh * 0.5f)}; // stb's y is down from the baseline
        g.advance = p.xadvance;
        f.codepoints[i] = (u32)cps[i];
    }
    font_index(f);

    // Kerning: the font's kern table when it has one (every pair it lists, mapped back to
    // the code points that use each glyph); otherwise pairwise through stb, which reads
    // GPOS, over the first kPairwiseMax glyphs. Only nonzero pairs are kept.
    Array<KernPair> pairs;
    pairs.alloc = mem_scratch();
    if (int len = stbtt_GetKerningTableLength(&info)) {
        stbtt_kerningentry* table = (stbtt_kerningentry*)alloc_in(mem_scratch(), sizeof(stbtt_kerningentry) * (usize)len);
        len = stbtt_GetKerningTable(&info, table, len);
        GlyphAt* by_glyph = (GlyphAt*)alloc_in(mem_scratch(), sizeof(GlyphAt) * n);
        for (u32 i = 0; i < n; ++i) by_glyph[i] = GlyphAt{gis[i], i};
        qsort(by_glyph, n, sizeof(GlyphAt), compare_glyph_at);
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
                if (int k = stbtt_GetGlyphKernAdvance(&info, gis[a], gis[b]))
                    pairs.push(KernPair{(u64)cps[a] << 32 | (u32)cps[b], (f32)k * scale});
    }
    if (pairs.len) {
        qsort(pairs.data, pairs.len, sizeof(KernPair), compare_kern);
        f.kern_pairs  = (u64*)alloc_in(s.general, sizeof(u64) * pairs.len);
        f.kern_values = (f32*)alloc_in(s.general, sizeof(f32) * pairs.len);
        u32 m = 0;
        for (u32 i = 0; i < pairs.len; ++i) {
            if (m && f.kern_pairs[m - 1] == pairs[i].key) continue; // a pair listed twice: the first stands
            f.kern_pairs[m] = pairs[i].key; f.kern_values[m] = pairs[i].value; ++m;
        }
        f.kern_count = m;
        if (m != pairs.len) { // shrink to what is kept, so the free matches
            u64* kp = (u64*)alloc_in(s.general, sizeof(u64) * m);
            f32* kv = (f32*)alloc_in(s.general, sizeof(f32) * m);
            memcpy(kp, f.kern_pairs, sizeof(u64) * m); memcpy(kv, f.kern_values, sizeof(f32) * m);
            free_in(s.general, f.kern_pairs, sizeof(u64) * pairs.len); free_in(s.general, f.kern_values, sizeof(f32) * pairs.len);
            f.kern_pairs = kp; f.kern_values = kv;
        }
    }

    f.ascent       = (f32)asc * scale;
    f.descent      = (f32)desc * scale;
    f.line_height  = (f32)(asc - desc + gap) * scale;
    f.atlas_width  = w;
    f.atlas_height = h;
    f.build_seconds = time_now() - t0;
    return true;
}

int compare_range(const void* a, const void* b) {
    const FontRange& x = *(const FontRange*)a; const FontRange& y = *(const FontRange*)b;
    return x.first != y.first ? (x.first < y.first ? -1 : 1) : (x.last < y.last ? -1 : x.last > y.last ? 1 : 0);
}

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

} // namespace

bool asset_fonts_init(Engine&, AssetState_& s, ModuleInstance& self) {
    s.font_by_key.init(&self.general);
    kv_handle_kind_desc desc{};
    desc.name = "asset.font"; desc.type_key = type_key<FontAsset>(); desc.ctx = &s;
    desc.resolve = font_resolve; desc.describe = font_describe; desc.flags = KV_HANDLE_GENERATIONAL;
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
        free_tables(s, f);
        free_in(s.general, f.ranges, sizeof(FontRange) * f.range_count);
    }
    KV_INFO("asset", "%u fonts loaded, %u failed", s.fonts_loaded, s.fonts_failed);
    kv_handle_kind_unregister(s.font_kind);
    s.font_by_key.free();
    s.fonts.free();
}

// Phase PreSim, main thread, after the texture pump: a font file that arrived is built.
void asset_fonts_pump(TickContext& ctx) {
    AssetState_& s = ctx.state_as<AssetState_>();
    Engine& e = *ctx.engine;
    IoQueue* io = platform_io(e);
    if (!io) return;
    asset_reap_orphans(e, s, s.font_kind);
    for (u32 i = 0; i < io->completions().len;) {
        IoCompletion c = io->completions()[i];
        if (!asset_is_kind(s, c.user, s.font_kind)) { ++i; continue; }
        Handle h{c.user};
        FontAsset* f = s.fonts.get(h);
        if (!f || f->io != c.request) { ++i; continue; }
        io->reap(c.request);
        f->io = Handle{};
        if (c.status != IoStatus::Done) {
            fail_font(e, s, h, *f, c.status == IoStatus::Failed && c.error == 2 ? AssetError::NotFound : AssetError::Read, nullptr);
            continue;
        }
        const char* why = nullptr;
        bool ok = build_font(e, s, *f, (const u8*)c.data, c.size, &why);
        free_in(s.general, c.data, c.size);
        if (!ok) { free_tables(s, *f); fail_font(e, s, h, *f, AssetError::Decode, why); continue; }
        f->state = AssetState::Ready;
        ++s.fonts_loaded;
        KV_INFO("asset", "font '%s' %gpx ready: %u glyphs, atlas %ux%u, line %.1f, %u kerning pairs, built in %.1f ms", f->path,
                f->px, f->glyph_count, f->atlas_width, f->atlas_height, f->line_height, f->kern_count, f->build_seconds * 1e3);
        e.events.emit(AssetReady{h});
    }
}

Span<const FontRange> font_default_ranges() { return Span<const FontRange>(k_default_ranges, KV_COUNTOF(k_default_ranges)); }

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
    if (Renderer* r = renderer_get(e)) { renderer_material_destroy(r, f->material); renderer_texture_destroy(r, f->texture); }
    free_tables(s, *f);
    free_in(s.general, f->ranges, sizeof(FontRange) * f->range_count);
    for (u32 i = 0; i < s.font_by_key.len(); ++i)
        if (s.font_by_key.values[i] == h) { s.font_by_key.remove(s.font_by_key.keys[i]); break; }
    KV_INFO("asset", "font '%s' %gpx retired", f->path, f->px);
    s.fonts.destroy(h);
}

Handle font_load(Engine& e, AssetState_& st, const char* path, const FontDesc& desc) {
    AssetState_* s = &st;
    Renderer* r = renderer_get(e);
    IoQueue* io = platform_io(e);
    f32 px = desc.px;
    if (!r || !io || !path) { KV_ERROR("asset", "cannot load font '%s': asset, renderer and platform must be up", path ? path : "?"); return Handle{}; }
    if (strlen(path) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "path too long: '%s'", path); return Handle{}; }
    if (!(px >= 4.0f && px <= 256.0f)) { KV_ERROR("asset", "font '%s': size %g is outside [4, 256]", path, px); return Handle{}; }

    // The set, merged: sorted by first, overlapping and adjacent ranges joined.
    MemScope scope;
    FontRange* merged = (FontRange*)alloc_in(mem_scratch(), sizeof(FontRange) * (desc.ranges.len ? desc.ranges.len : 1));
    u32 nr = 0;
    for (const FontRange& rg : desc.ranges)
        if (rg.first <= rg.last && rg.last <= 0x10FFFF) merged[nr++] = rg;
    qsort(merged, nr, sizeof(FontRange), compare_range);
    u32 m = 0;
    u64 total = 0;
    for (u32 i = 0; i < nr; ++i) {
        if (m && merged[i].first <= merged[m - 1].last + 1) { if (merged[i].last > merged[m - 1].last) merged[m - 1].last = merged[i].last; }
        else merged[m++] = merged[i];
    }
    for (u32 i = 0; i < m; ++i) total += merged[i].last - merged[i].first + 1;
    if (m == 0) { KV_ERROR("asset", "font '%s': the character set is empty", path); return Handle{}; }
    if (total > KV_FONT_MAX_GLYPHS) { KV_ERROR("asset", "font '%s': %llu code points, more than %u", path, (unsigned long long)total, KV_FONT_MAX_GLYPHS); return Handle{}; }

    // The key: path, size, and the set unless it is the default.
    u64 set_hash = hash_bytes(merged, sizeof(FontRange) * m);
    static u64 default_hash = 0;
    if (!default_hash) {
        Span<const FontRange> d = font_default_ranges(); // already sorted and disjoint
        default_hash = hash_bytes(d.data, sizeof(FontRange) * d.len);
    }
    char key[KV_ASSET_PATH_MAX + 40];
    if (set_hash == default_hash) snprintf(key, sizeof key, "%s@%g", path, px);
    else snprintf(key, sizeof key, "%s@%g#%016llx", path, px, (unsigned long long)set_hash);
    Name name = name_intern(key);
    if (const Handle* existing = s->font_by_key.find(name)) return *existing;

    FontAsset f{};
    strncpy(f.path, path, sizeof f.path - 1);
    f.px    = px;
    f.state = AssetState::Pending;
    TextureDesc td;
    td.format = TextureFormat::RGBA8_UNORM;
    td.name   = key;
    f.texture = renderer_texture_create(r, td);
    if (f.texture.is_null()) return Handle{};
    MaterialDesc md;
    md.texture = f.texture;
    md.blend   = BlendMode::Alpha;
    f.material = renderer_material_create(r, md);
    if (f.material.is_null()) { renderer_texture_destroy(r, f.texture); return Handle{}; }
    f.ranges = (FontRange*)alloc_in(s->general, sizeof(FontRange) * m);
    memcpy(f.ranges, merged, sizeof(FontRange) * m);
    f.range_count = m;
    Handle h = s->fonts.create(f);
    if (h.is_null()) {
        free_in(s->general, f.ranges, sizeof(FontRange) * m);
        renderer_material_destroy(r, f.material); renderer_texture_destroy(r, f.texture);
        return Handle{};
    }
    s->font_by_key.set(name, h);

    IoRequest req;
    req.path  = path;
    req.alloc = s->general;
    req.user  = h.bits;
    Handle iq = io->read_asset(req);
    FontAsset* rec = s->fonts.get(h);
    if (iq.is_null()) { fail_font(e, *s, h, *rec, AssetError::Read, "could not submit"); return h; }
    rec->io = iq;
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
    u32 lo = 0, hi = f.glyph_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (f.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < f.glyph_count && f.codepoints[lo] == cp ? &f.glyphs[lo] : nullptr;
}

const Glyph* font_glyph_or_replacement(const FontAsset& f, u32 cp) {
    if (f.state != AssetState::Ready) return nullptr;
    if (const Glyph* g = font_glyph(f, cp)) return g;
    if (f.misses++ == 0) KV_WARN("asset", "font '%s' %gpx has no glyph for U+%04X%s", f.path, f.px, cp, f.replacement ? "; drawing U+FFFD" : "");
    return f.replacement ? &f.glyphs[f.replacement - 1] : nullptr;
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
