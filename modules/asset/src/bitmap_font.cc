// Bitmap fonts (font.h, BitmapFontDesc; docs/PRDs/Krome-Slice-7-PRD.md, KR32): a .kbf read,
// then its image; or a description given in code, then its image. The image's cells are
// copied into one page, a clear pixel round each so linear sampling never reaches the next
// cell, and become the font's table of glyphs: the same FontAsset a TrueType strike is,
// with no faces and no kerning.
#include "korvid/asset/font.h"
#include "korvid/asset/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/sort.h"
#include "korvid/core/utf8.h"
#include "korvid/image/png.h"
#include "korvid/math/scalar.h"
#include "korvid/platform/io.h"
#include "asset_internal.h"
#include <string.h>

namespace kv {

constexpr u32 kBitmapPageMax = 4096; // a page's side at most: an image of more cells than fit is refused

struct FontBitmap {
    u8     stage = 0;                    // 0 the .kbf being read, 1 the image, 2 made
    Handle io;
    char   image[KV_ASSET_PATH_MAX] = {};
    u32    cell_w = 0, cell_h = 0, advance = 0;
    u32*   chars = nullptr;              // the cells' code points, in order
    u32    char_count = 0;
    u32    missing = 0;                  // 0: none
    bool   has_missing = false;
    u8*    page = nullptr;               // side x side, RGBA8, linear
    u32    side = 0;
    bool   uploaded = false;
};

namespace {

// ---- the .kbf -----------------------------------------------------------------------------------

bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// A quoted string at p (escapes \" and \\), copied; null when it is not one.
const char* quoted(const char*& p, const char* end, Allocator* a) {
    if (p >= end || *p != '"') return nullptr;
    ++p;
    const char* start = p;
    usize n = 0;
    for (const char* q = p; q < end && *q != '"' && *q != '\n'; ++q, ++n) if (*q == '\\' && q + 1 < end) ++q;
    char* out = (char*)alloc_in(a, n + 1, 1);
    usize k = 0;
    while (p < end && *p != '"' && *p != '\n') {
        if (*p == '\\' && p + 1 < end) ++p;
        out[k++] = *p++;
    }
    out[k] = 0;
    if (p >= end || *p != '"') return nullptr;
    ++p;
    (void)start;
    return out;
}

bool number(const char*& p, const char* end, u32* out) {
    u64 v = 0;
    const char* s = p;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (u64)(*p - '0'); if (v > 1000000) return false; ++p; }
    *out = (u32)v;
    return p > s;
}

u8 linear_of[256]; // sRGB byte to linear byte: a page is linear, a picture's colours are not
bool linear_made = false;
void make_linear() {
    if (linear_made) return;
    for (u32 i = 0; i < 256; ++i) {
        f32 c = (f32)i / 255.0f;
        f32 l = c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
        linear_of[i] = (u8)(l * 255.0f + 0.5f);
    }
    linear_made = true;
}

void fail_bitmap(Engine& e, AssetState_& s, Handle h, FontAsset& f, AssetError err, const char* detail) {
    f.state = AssetState::Failed;
    f.error = err;
    if (f.bitmap) f.bitmap->io = Handle{};
    ++s.fonts_failed;
    KV_WARN("asset", "bitmap font '%s' %gpx: %s%s%s", f.path, f.px, asset_error_name(err), detail ? ": " : "", detail ? detail : "");
    e.events.emit(AssetFailed{h, err});
}

// The description into the record: the image's path (beside `base` when one is given), the
// cells, the characters decoded.
bool describe(AssetState_& s, FontBitmap& b, const BitmapFontDesc& d, const char* base, const char** why) {
    if (!d.image || !*d.image) { *why = "it names no image"; return false; }
    if (!d.cell_w || !d.cell_h) { *why = "its cell has no size"; return false; }
    if (!d.chars || !*d.chars) { *why = "it names no characters"; return false; }
    usize dir = 0;
    if (base) for (usize i = 0; base[i]; ++i) if (base[i] == '/') dir = i + 1;
    if (dir + strlen(d.image) >= sizeof b.image) { *why = "its image's path is too long"; return false; }
    if (dir) memcpy(b.image, base, dir);
    strncpy(b.image + dir, d.image, sizeof b.image - dir - 1);
    b.cell_w = d.cell_w;
    b.cell_h = d.cell_h;
    b.advance = d.advance;
    b.char_count = utf8_count(d.chars);
    b.chars = (u32*)alloc_in(s.general, sizeof(u32) * b.char_count);
    const char* p = d.chars;
    for (u32 i = 0; i < b.char_count; ++i) b.chars[i] = utf8_next(p);
    if (d.missing && *d.missing) {
        const char* m = d.missing;
        b.missing = utf8_next(m);
        b.has_missing = true;
    }
    return true;
}

void submit(Engine& e, AssetState_& s, Handle h, FontBitmap& b, const char* path) {
    IoQueue* io = platform_io(e);
    IoRequest req;
    req.path  = path;
    req.alloc = s.general;
    req.user  = h.bits;
    req.owner = asset_io_owner(s.font_kind);
    b.io = io ? io->read_asset(req) : Handle{};
}

// The image is in: the page made, the table of glyphs, Ready.
bool make(AssetState_& s, FontAsset& f, const u8* data, usize size, const char** why) {
    FontBitmap& b = *f.bitmap;
    PngImage img;
    if (!png_decode(data, size, s.general, img, why)) return false;
    u32 cols = img.width / b.cell_w, rows = img.height / b.cell_h;
    bool ok = true;
    if ((u64)cols * rows < b.char_count) {
        static char msg[128];
        format(msg, sizeof msg, "%u characters, and the %ux%u image holds %u cells of %ux%u", b.char_count, img.width, img.height, cols * rows, b.cell_w, b.cell_h);
        *why = msg;
        ok = false;
    }
    // One page for every cell, each a pixel apart: as many cells across as fit in a square.
    u32 across = 1;
    while (ok && (u64)across * across < b.char_count) ++across;
    u32 down = across ? (b.char_count + across - 1) / across : 0;
    u32 need = across * (b.cell_w + 1) + 1;
    if (down * (b.cell_h + 1) + 1 > need) need = down * (b.cell_h + 1) + 1;
    u32 side = 64;
    while (side < need) side *= 2;
    if (ok && side > kBitmapPageMax) { *why = "its cells do not fit in a 4096-pixel page"; ok = false; }
    if (!ok) { free_in(s.general, img.rgba, (usize)img.width * img.height * 4); return false; }
    make_linear();
    usize bytes = (usize)side * side * 4;
    b.page = (u8*)alloc_in(s.general, bytes, 16);
    b.side = side;
    for (usize i = 0; i < bytes; i += 4) { b.page[i] = b.page[i + 1] = b.page[i + 2] = 255; b.page[i + 3] = 0; }
    // The glyphs, by code point (a character named twice: its first cell).
    f32 k = f.px / (f32)b.cell_h;
    f32 inv = 1.0f / (f32)side;
    f.codepoints = (u32*)alloc_in(s.general, sizeof(u32) * b.char_count);
    f.glyphs = alloc_items<Glyph>(s.general, b.char_count);
    u32 n = 0;
    for (u32 i = 0; i < b.char_count; ++i) {
        u32 cp = b.chars[i];
        bool seen = false;
        for (u32 j = 0; j < i; ++j) if (b.chars[j] == cp) seen = true;
        if (seen) continue;
        u32 sx = (i % cols) * b.cell_w, sy = (i / cols) * b.cell_h;
        u32 dx = 1 + (i % across) * (b.cell_w + 1), dy = 1 + (i / across) * (b.cell_h + 1);
        for (u32 r = 0; r < b.cell_h; ++r) {
            const u8* from = img.rgba + ((usize)(sy + r) * img.width + sx) * 4;
            u8* to = b.page + ((usize)(dy + r) * side + dx) * 4;
            for (u32 c = 0; c < b.cell_w; ++c) {
                to[c * 4 + 0] = linear_of[from[c * 4 + 0]];
                to[c * 4 + 1] = linear_of[from[c * 4 + 1]];
                to[c * 4 + 2] = linear_of[from[c * 4 + 2]];
                to[c * 4 + 3] = from[c * 4 + 3];
            }
        }
        // Inserted in code point order.
        u32 at = n;
        while (at > 0 && f.codepoints[at - 1] > cp) { f.codepoints[at] = f.codepoints[at - 1]; f.glyphs[at] = f.glyphs[at - 1]; --at; }
        Glyph& g = f.glyphs[at];
        g = Glyph{};
        f.codepoints[at] = cp;
        g.uv      = rect2{(f32)dx * inv, (f32)dy * inv, (f32)b.cell_w * inv, (f32)b.cell_h * inv};
        g.size    = vec2{(f32)b.cell_w * k, (f32)b.cell_h * k};
        g.offset  = vec2{g.size.x * 0.5f, g.size.y * 0.5f}; // the cell's bottom on the baseline
        g.advance = (f32)(b.advance ? b.advance : b.cell_w) * k;
        g.texture = f.texture;
        g.material = f.material;
        ++n;
    }
    f.glyph_count = n;
    font_index(f);
    f.replacement = 0;
    if (b.has_missing)
        for (u32 i = 0; i < n; ++i) if (f.codepoints[i] == b.missing) f.replacement = i + 1;
    f.ascent = (f32)b.cell_h * k;
    f.descent = 0.0f;
    f.line_height = (f32)b.cell_h * k;
    f.atlas_width = f.atlas_height = side;
    free_in(s.general, img.rgba, (usize)img.width * img.height * 4);
    b.stage = 2;
    return true;
}

} // namespace

bool kbf_parse(const char* text, usize len, Allocator* strings, BitmapFontDesc& out, const char** why, u32* line) {
    out = BitmapFontDesc{};
    *why = nullptr;
    *line = 0;
    const char* p = text;
    const char* end = text + len;
    u32 at = 0;
    while (p < end) {
        ++at;
        const char* eol = p;
        while (eol < end && *eol != '\n') ++eol;
        while (p < eol && space(*p)) ++p;
        if (p == eol || *p == '#') { p = eol + 1; continue; }
        const char* w = p;
        while (p < eol && !space(*p)) ++p;
        usize wn = (usize)(p - w);
        while (p < eol && space(*p)) ++p;
        auto is = [&](const char* k) { return wn == strlen(k) && !memcmp(w, k, wn); };
        bool ok = true;
        if (is("image")) ok = (out.image = quoted(p, eol, strings)) != nullptr;
        else if (is("chars")) ok = (out.chars = quoted(p, eol, strings)) != nullptr && *out.chars;
        else if (is("missing")) ok = (out.missing = quoted(p, eol, strings)) != nullptr && utf8_count(out.missing) == 1;
        else if (is("cell")) {
            ok = number(p, eol, &out.cell_w);
            while (p < eol && space(*p)) ++p;
            ok = ok && number(p, eol, &out.cell_h) && out.cell_w && out.cell_h;
        } else if (is("advance")) ok = number(p, eol, &out.advance);
        else { *why = "not a .kbf word (image, cell, chars, missing, advance)"; *line = at; return false; }
        while (p < eol && space(*p)) ++p;
        if (!ok || p != eol) {
            *why = is("image") ? "image takes a path in quotes" : is("chars") ? "chars takes the characters in quotes" :
                   is("missing") ? "missing takes one character in quotes" : is("cell") ? "cell takes a width and a height above 0" :
                   "advance takes a number of pixels";
            *line = at;
            return false;
        }
        p = eol + 1;
    }
    if (!out.image) *why = "no image line";
    else if (!out.cell_w) *why = "no cell line";
    else if (!out.chars) *why = "no chars line";
    if (*why) { *line = 0; return false; } // the whole file's
    return true;
}

Handle bitmap_font_load(Engine& e, AssetState_& s, const char* path, const BitmapFontDesc* desc, f32 px, Name key) {
    if (const Handle* existing = s.font_by_key.find(key)) return *existing;
    FontAsset f{};
    strncpy(f.path, path, sizeof f.path - 1);
    f.px = px;
    f.state = AssetState::Pending;
    Handle h = s.fonts.create(f);
    if (h.is_null()) return Handle{};
    FontAsset* rec = s.fonts.get(h);
    FontBitmap* b = alloc_items<FontBitmap>(s.general, 1);
    *b = FontBitmap{};
    rec->bitmap = b;
    // The page's texture now, so the font's is valid from load on, as a strike's is.
    if (s.font_pages.create) {
        char tname[KV_ASSET_PATH_MAX + 48];
        format(tname, sizeof tname, "%s:page0", name_str(key));
        if (!s.font_pages.create(s.font_pages.user, tname, &rec->texture, &rec->material)) rec->texture = rec->material = Handle{};
    }
    s.font_by_key.set(key, h);
    if (desc) {
        const char* why = nullptr;
        if (!describe(s, *b, *desc, nullptr, &why)) { fail_bitmap(e, s, h, *rec, AssetError::Decode, why); return h; }
        b->stage = 1;
        submit(e, s, h, *b, b->image);
    } else {
        submit(e, s, h, *b, path);
    }
    if (b->io.is_null()) fail_bitmap(e, s, h, *rec, AssetError::Read, "could not submit its read");
    return h;
}

bool bitmap_font_take(Engine& e, AssetState_& s, IoCompletion& c) {
    for (u32 i = 0; i < s.fonts.len; ++i) {
        if (!s.fonts.is_alive(i)) continue;
        FontAsset& f = s.fonts.items[i];
        FontBitmap* b = f.bitmap;
        if (!b || b->io.is_null() || b->io != c.request) continue;
        Handle h = s.fonts.handle_at(i);
        b->io = Handle{};
        if (IoQueue* io = platform_io(e)) io->reap(c.request);
        if (c.status != IoStatus::Done) {
            fail_bitmap(e, s, h, f, c.not_found() ? AssetError::NotFound : AssetError::Read, b->stage == 0 ? "the .kbf" : b->image);
            c.bytes.free();
            return true;
        }
        const char* why = nullptr;
        if (b->stage == 0) {
            // The .kbf: its description, then its image beside it.
            MemScope scope;
            BitmapFontDesc d;
            u32 line = 0;
            bool ok = kbf_parse((const char*)c.bytes.data, c.bytes.size, mem_scratch(), d, &why, &line) && describe(s, *b, d, f.path, &why);
            c.bytes.free();
            if (!ok) {
                char msg[160];
                if (line) format(msg, sizeof msg, "line %u: %s", line, why);
                else format(msg, sizeof msg, "%s", why);
                fail_bitmap(e, s, h, f, AssetError::Parse, msg);
                return true;
            }
            b->stage = 1;
            submit(e, s, h, *b, b->image);
            if (b->io.is_null()) fail_bitmap(e, s, h, f, AssetError::Read, "could not submit its image's read");
            return true;
        }
        bool ok = make(s, f, (const u8*)c.bytes.data, c.bytes.size, &why);
        c.bytes.free();
        if (!ok) { fail_bitmap(e, s, h, f, AssetError::Decode, why); return true; }
        f.state = AssetState::Ready;
        ++s.fonts_loaded;
        KV_INFO("asset", "bitmap font '%s' %gpx ready: %u characters of %ux%u from '%s'", f.path, f.px, f.glyph_count, b->cell_w, b->cell_h, b->image);
        e.events.emit(AssetReady{h});
        return true;
    }
    return false;
}

void bitmap_font_flush(AssetState_& s, FontAsset& f) {
    FontBitmap* b = f.bitmap;
    if (!b || b->uploaded || b->stage != 2 || f.texture.is_null() || !s.font_pages.upload) return;
    b->uploaded = s.font_pages.upload(s.font_pages.user, f.texture, b->page, b->side, true, 0, 0, b->side, b->side);
}

void bitmap_font_free(Engine& e, AssetState_& s, FontAsset& f) {
    FontBitmap* b = f.bitmap;
    if (!b) return;
    IoQueue* io = platform_io(e);
    if (io && !b->io.is_null()) io->cancel(b->io);
    if (s.font_pages.destroy && !f.texture.is_null()) s.font_pages.destroy(s.font_pages.user, f.texture, f.material);
    free_in(s.general, b->chars, sizeof(u32) * b->char_count);
    free_in(s.general, b->page, (usize)b->side * b->side * 4);
    free_in(s.general, f.codepoints, sizeof(u32) * b->char_count);
    if (f.glyphs) free_items(s.general, f.glyphs, b->char_count);
    f.codepoints = nullptr;
    f.glyphs = nullptr;
    f.glyph_count = 0;
    free_items(s.general, b, 1);
    f.bitmap = nullptr;
}

Handle asset_load_bitmap_font(Engine& e, const char* name, const BitmapFontDesc& desc, f32 px) {
    AssetState_* s = asset_state(e);
    if (!s || !name) { KV_ERROR("asset", "cannot load bitmap font '%s': the asset module is not up", name ? name : "?"); return Handle{}; }
    if (strlen(name) >= KV_ASSET_PATH_MAX) { KV_ERROR("asset", "name too long: '%s'", name); return Handle{}; }
    if (!(px >= 1.0f && px <= 1024.0f)) { KV_ERROR("asset", "bitmap font '%s': size %g is outside [1, 1024]", name, px); return Handle{}; }
    char key[KV_ASSET_PATH_MAX + 32];
    format(key, sizeof key, "%s@%g", name, px);
    return asset_hold(*s, asset_scope_global(e), bitmap_font_load(e, *s, name, &desc, px, name_intern(key)));
}

} // namespace kv
