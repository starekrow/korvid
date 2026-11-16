// Krome's shapes (docs/PRDs/Krome-Slice-5-PRD.md, "Krome"): an image node whose asset is a shape
// is rasterized by patherizer at the node's pixel size and kept in pages, as glyphs are, by
// shape, size and colour. A one-colour shape (every paint currentColor) is rasterized once in
// white and tinted by the box, so a hover, a theme or a transition changes the tint and not the
// pixels; a shape with colours of its own is rasterized with currentColor resolved, and that
// colour is part of its key. A page's changed rectangle goes to the renderer after the paint.
#include "internal.h"
#include "korvid/asset/shape.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/render/renderer.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kPage = 1024;      // a page's side, pixels
constexpr u32 kOwnPast = 512;    // a shape wider or taller than this gets a texture of its own
constexpr u32 kMaxPages = 8;     // past it, the page used longest ago is cleared and reused

u32 pack(color c) {
    auto b = [](f32 v) { return (u32)(v <= 0.0f ? 0 : v >= 1.0f ? 255 : (u32)(v * 255.0f + 0.5f)); };
    return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// Room for w x h on a page: its place, or false when the page is full.
bool place(KromeShapePage& p, u32 w, u32 h, u32* x, u32* y) {
    if (p.x + w > kPage) { p.x = 0; p.shelf_y += p.shelf_h + 1; p.shelf_h = 0; }
    if (p.shelf_y + h > kPage) return false;
    *x = p.x;
    *y = p.shelf_y;
    p.x += w + 1;
    if (h > p.shelf_h) p.shelf_h = h;
    return true;
}

void dirty(KromeShapePage& p, u32 x, u32 y, u32 w, u32 h) {
    if (!p.dirty) { p.d0x = x; p.d0y = y; p.d1x = x + w; p.d1y = y + h; p.dirty = true; return; }
    p.d0x = x < p.d0x ? x : p.d0x; p.d0y = y < p.d0y ? y : p.d0y;
    p.d1x = x + w > p.d1x ? x + w : p.d1x; p.d1y = y + h > p.d1y ? y + h : p.d1y;
}

bool new_page(KromeModule& s, KromeShapePage& p, u32 w, u32 h) {
    usize bytes = (usize)w * h * 4;
    p.rgba = (u8*)alloc_in(s.general, bytes, 16);
    if (!p.rgba) return false;
    memset(p.rgba, 0, bytes);
    p.w = w;
    p.h = h;
    return true;
}

// A page's texture, made the first time it is drawn with a renderer: the whole page once, so
// the texture has its size; then only what changes.
bool page_texture(KromeModule& s, Renderer* r, KromeShapePage& p) {
    if (!p.texture.is_null()) return true;
    char name[48];
    format(name, sizeof name, "krome:shapes%u", s.shape_pages_made++);
    TextureDesc td;
    td.format = TextureFormat::RGBA8_SRGB; // patherizer writes sRGB, straight alpha
    td.name = name;
    p.texture = renderer_texture_create(r, td);
    if (p.texture.is_null()) return false;
    renderer_texture_update(r, p.texture, p.rgba, p.w, p.h, 0, 0, p.w, p.h);
    p.dirty = false;
    return true;
}

// The box drawing entry e: false without a renderer (the cache is kept all the same).
bool boxed(KromeModule& s, Renderer* r, KromeShapePage& p, const KromeShapeEntry& e, UiBox& box) {
    if (!r || !page_texture(s, r, p)) return false;
    box.textured = true;
    box.texture = renderer_texture_slot(r, p.texture);
    box.uv = rect2{(f32)e.x / (f32)p.w, (f32)e.y / (f32)p.h, (f32)e.w / (f32)p.w, (f32)e.h / (f32)p.h};
    return true;
}

void free_page(KromeModule& s, Renderer* r, KromeShapePage& p) {
    if (r && !p.texture.is_null()) renderer_texture_destroy(r, p.texture);
    if (p.rgba) free_in(s.general, p.rgba, (usize)p.w * p.h * 4);
    p = KromeShapePage{};
}

} // namespace

bool krome_shape_box(KromeModule& s, Renderer* r, const ShapeAsset& shape, Handle asset, u32 w, u32 h, color current, UiBox& box) {
    if (!w || !h || w > 4096 || h > 4096) return false;
    u32 colour = shape.one_colour ? 0xFFFFFFFFu : pack(current);
    s.shape_entries.alloc = s.general;
    s.shape_pages.alloc = s.general;
    ++s.shape_clock;
    for (KromeShapeEntry& e : s.shape_entries) {
        if (e.asset == asset && e.w == w && e.h == h && e.colour == colour) {
            e.used = s.shape_clock;
            KromeShapePage& p = s.shape_pages[e.page];
            p.used = s.shape_clock;
            return boxed(s, r, p, e, box);
        }
    }
    // A place: a page with room, a new one, or the one used longest ago, cleared.
    u32 page = ~0u, x = 0, y = 0;
    bool own = w > kOwnPast || h > kOwnPast;
    if (!own) {
        for (u32 i = 0; i < s.shape_pages.len && page == ~0u; ++i)
            if (!s.shape_pages[i].own && place(s.shape_pages[i], w, h, &x, &y)) page = i;
        if (page == ~0u && s.shape_pages.len >= kMaxPages) {
            u32 oldest = ~0u;
            for (u32 i = 0; i < s.shape_pages.len; ++i)
                if (!s.shape_pages[i].own && (oldest == ~0u || s.shape_pages[i].used < s.shape_pages[oldest].used)) oldest = i;
            if (oldest != ~0u) {
                KromeShapePage& p = s.shape_pages[oldest];
                for (u32 i = 0; i < s.shape_entries.len;) {
                    if (s.shape_entries[i].page == oldest) { s.shape_entries[i] = s.shape_entries.last(); --s.shape_entries.len; }
                    else ++i;
                }
                memset(p.rgba, 0, (usize)p.w * p.h * 4);
                p.x = p.shelf_y = p.shelf_h = 0;
                dirty(p, 0, 0, p.w, p.h);
                if (place(p, w, h, &x, &y)) page = oldest;
            }
        }
    }
    if (page == ~0u) {
        KromeShapePage p;
        p.own = own;
        if (!new_page(s, p, own ? w : kPage, own ? h : kPage)) { free_page(s, r, p); return false; }
        if (!own && !place(p, w, h, &x, &y)) { free_page(s, r, p); return false; }
        s.shape_pages.push(p);
        page = s.shape_pages.len - 1;
    }
    KromeShapePage& p = s.shape_pages[page];
    p.used = s.shape_clock;
    // Drawn into its place on the page: cleared, then the shape over it.
    for (u32 row = 0; row < h; ++row) memset(p.rgba + ((usize)(y + row) * p.w + x) * 4, 0, (usize)w * 4);
    PathTarget t{p.rgba + ((usize)y * p.w + x) * 4, w, h, p.w * 4, false};
    PathOptions o;
    u32 c = shape.one_colour ? 0xFFFFFFFFu : colour;
    o.current[0] = (u8)c; o.current[1] = (u8)(c >> 8); o.current[2] = (u8)(c >> 16); o.current[3] = (u8)(c >> 24);
    patherizer_draw(shape.shape, t, o);
    dirty(p, x, y, w, h);
    ++s.shapes_drawn;
    KromeShapeEntry e;
    e.asset = asset;
    e.w = w;
    e.h = h;
    e.colour = colour;
    e.page = page;
    e.x = x;
    e.y = y;
    e.used = s.shape_clock;
    s.shape_entries.push(e);
    return boxed(s, r, p, e, box);
}

void krome_shapes_flush(KromeModule& s, Renderer* r) {
    if (!r) return;
    for (KromeShapePage& p : s.shape_pages) {
        if (!p.dirty || p.texture.is_null()) continue;
        renderer_texture_update(r, p.texture, p.rgba, p.w, p.h, p.d0x, p.d0y, p.d1x - p.d0x, p.d1y - p.d0y);
        p.dirty = false;
    }
}

void krome_shapes_free(KromeModule& s, Renderer* r) {
    for (KromeShapePage& p : s.shape_pages) free_page(s, r, p);
    s.shape_pages.free();
    s.shape_entries.free();
}

} // namespace kv
