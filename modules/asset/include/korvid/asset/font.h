// Korvid asset: fonts as assets (docs/PRDs/Krome-Slice-3-PRD.md, "Glyphs on demand").
// asset_load_font(e, "sprite_text/DejaVuSans.ttf", 32) returns a handle at once: a strike, the
// font at one pixel size. The .ttf (and each fallback's) reads through the I/O queue, once
// per file however many sizes use it; "asset.pump_fonts" (phase PreSim, main thread) makes
// the strike Ready when its first face is in, with its metrics and kerning, and
// AssetReady follows. Before that the metrics are zero and font_glyph returns null for
// everything, so text lays out as nothing and appears when the font does.
//
// Glyphs are made when they are first asked for: rasterized with stb_truetype into the
// strike's pages (512x512, RGBA8 white with coverage in alpha, packed by shelves, a new
// page when one fills), and kept. A Glyph pointer stays valid for the strike's life. Asking
// is safe from any thread. The pages changed in a frame are uploaded by "asset.fonts_flush"
// (the Frame cycle, after the systems that draw text), by the rectangles that changed where
// the backend can, so a glyph made while a frame is laid out is drawn in that frame. The
// pages are drawn through asset_render (AssetFontPages, below): a program without it, a tool,
// gets glyphs, metrics and the pages' pixels, and null texture and material handles.
//
// A code point the first face lacks is looked for down the fallback list (FontDesc, or the
// engine's, asset_set_font_fallback), drawn from the first face that has it, scaled to
// the same pixel size on the same baseline. Small text is made crisp without hinting: the
// metrics are rounded to whole pixels, a glyph's box sits on whole pixels from the pen, and
// below 14 px the coverage is darkened so a thin stem reads as one pixel. Asked for, a glyph
// is made at quarter-pixel pen offsets as well (font_glyph_at), for subpixel positioning.
//
// Strings are UTF-8. FontDesc::ranges, when given, are made at load rather than on demand.
#pragma once
#include "korvid/asset/asset.h"
#include "korvid/math/rect2.h"

namespace kv {

constexpr u32 KV_FONT_MAX_GLYPHS   = 8192; // code points made at load (FontDesc::ranges), after merging
constexpr u32 KV_FONT_PAGE         = 512;  // a page's width and height, pixels
constexpr u32 KV_FONT_MAX_FALLBACK = 4;    // faces after the first

// One glyph in the atlas, in pixels at the font's size. A glyph quad is `size` placed so
// its center is `offset` from the pen (x right, y up, the pen on the baseline); then the
// pen moves by `advance` (plus the kerning with the next glyph).
struct Glyph {
    rect2  uv;       // in its page, texture space
    vec2   size;     // the quad, pixels; zero for a glyph with no ink (a space)
    vec2   offset;   // quad center from the pen
    f32    advance;
    Handle texture;  // render.texture: its page (null for a table built by hand)
    Handle material; // render.material: its page with alpha blend and white tint
};

// Code points first..last, inclusive.
struct FontRange { u32 first, last; };

// The Latin set kerning is read for (printable ASCII, Latin-1, Latin Extended-A, common
// punctuation, the euro, the trade mark, U+FFFD). A pair outside it is not kerned.
Span<const FontRange> font_default_ranges();

struct FontDesc {
    f32                      px       = 32.0f; // pixels per em-height, in [4, 256]
    Span<const FontRange>    ranges;           // made at load rather than when asked; copied
    Span<const char* const>  fallback;         // faces after this one, by asset path; empty: the engine's list
};

// The fallback list for fonts loaded from now on with none of their own: asset paths,
// copied. Fonts already loaded keep theirs.
void asset_set_font_fallback(Engine& e, Span<const char* const> paths);

// How a font's pages are drawn, set by a module that draws (asset_render). From then on each
// new page gets a texture and a material (Glyph::texture, ::material, FontAsset's), and
// "asset.fonts_flush" sends what changed in a page through `upload`. Without it the handles
// are null and nothing is uploaded; glyphs, metrics and layout are the same. Every callback
// is optional; `create` may run on any thread that asks for a glyph, under the strike's lock.
struct AssetFontPages {
    // A new page called `name`: its texture and its material, alpha-blended and linearly
    // sampled. False for none (no renderer up), and the page stays CPU-only.
    bool  (*create)(void* user, const char* name, Handle* texture, Handle* material) = nullptr;
    // A page's pixels (size x size, RGBA8) to its texture: all of it when `first`, else the
    // rectangle x, y, w, h that changed. False if the first upload did not take.
    bool  (*upload)(void* user, Handle texture, const u8* rgba, u32 size, bool first, u32 x, u32 y, u32 w, u32 h) = nullptr;
    // The page's texture and material, at the strike's retirement.
    void  (*destroy)(void* user, Handle texture, Handle material) = nullptr;
    void* user = nullptr;
};
// One hook, copied; a second call replaces the first, null removes it (pages made since keep
// their handles, and the renderer's go with the renderer).
void asset_font_pages(Engine& e, const AssetFontPages* pages);

struct FontStrike; // the glyph cache of a font read from a file (the asset module's)

struct FontAsset {
    char       path[KV_ASSET_PATH_MAX];      // as given to asset_load_font
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;     // Decode for a file stb_truetype rejects
    Handle     texture;                      // render.texture: the first page, valid from load on (null without AssetFontPages)
    Handle     material;                     // render.material: the first page with alpha blend and white tint
    Handle     io;                           // the pending read while reading; null otherwise
    f32        px = 0.0f;                    // the pixel size asked for
    f32        ascent = 0.0f, descent = 0.0f, line_height = 0.0f; // pixels, once Ready; descent is negative
    u32        atlas_width = 0, atlas_height = 0; // a page's
    FontStrike* strike = nullptr;            // a font from a file: glyphs made as asked. Null: the table below
    // A table built by hand (tests, a font made in code): glyphs[i] is code point
    // codepoints[i], ascending.
    u32        glyph_count = 0;
    u32*       codepoints  = nullptr;
    Glyph*     glyphs      = nullptr;
    u16        ascii[95]   = {};             // ' '..'~': index + 1 into glyphs, 0 if absent
    u32        replacement = 0;              // U+FFFD's index + 1, 0 if the font has none
    // Kerning, once Ready: pair (a << 32 | b) ascending, with its adjustment in pixels.
    u32        kern_count  = 0;
    u64*       kern_pairs  = nullptr;
    f32*       kern_values = nullptr;
    FontRange* ranges      = nullptr;        // the set asked for, merged and sorted
    u32        range_count = 0;
    mutable u32 misses     = 0;              // code points asked for that the table lacks (logged once)
    u32        refs        = 0;              // scopes and assets holding it
    f64        build_seconds = 0.0;          // what the rasterization cost, for the log
};

// Starts loading <exe dir>/assets/<path> as a TrueType font. Null if the module or the
// platform is not up, the path is too long, the tables are full, for px outside [4, 256], or
// for an empty set. The
// scope takes a reference; without one, the global scope does.
Handle           asset_load_font(Engine& e, const char* path, const FontDesc& desc, Handle scope);
Handle           asset_load_font(Engine& e, const char* path, const FontDesc& desc);
// The default character set at `px`, in the global scope.
Handle           asset_load_font(Engine& e, const char* path, f32 px);
// The record for a handle, or null if the handle is stale or not a font asset.
const FontAsset* asset_font(Engine& e, Handle asset);

// The glyph for a code point, made if this is the first ask, or null if the font is not
// Ready or no face of it has the code point.
const Glyph* font_glyph_lookup(const FontAsset& f, u32 codepoint);
const Glyph* font_strike_glyph(const FontAsset& f, u32 codepoint, u32 quarter, bool replace);
inline const Glyph* font_glyph(const FontAsset& f, u32 codepoint) {
    if (f.state != AssetState::Ready) return nullptr;
    if (f.strike) return font_strike_glyph(f, codepoint, 0, false);
    if (codepoint >= 32 && codepoint < 127) { u16 i = f.ascii[codepoint - 32]; return i ? &f.glyphs[i - 1] : nullptr; }
    return font_glyph_lookup(f, codepoint);
}
// For subpixel positioning: the glyph made with the pen `quarter` quarters of a pixel
// right of a whole pixel (0 to 3), its replacement when the font lacks it. A table built
// by hand has one copy, whatever the quarter.
const Glyph* font_glyph_at(const FontAsset& f, u32 codepoint, u32 quarter);
// Grows when what a code point draws changes (a fallback face landed): text laid out
// before then is laid out again.
u32 font_generation(const FontAsset& f);
// Glyphs made and pages in use, for tools.
void font_counts(const FontAsset& f, u32* glyphs, u32* pages);
// What to draw for a code point: its glyph, else U+FFFD's, else null (no ink, no
// advance). A miss is counted on the font and the first one logged.
const Glyph* font_glyph_or_replacement(const FontAsset& f, u32 codepoint);
// The pen adjustment between two code points, pixels; 0 for a pair the font does not kern.
f32 font_kern(const FontAsset& f, u32 a, u32 b);
// The width of a UTF-8 string (up to the NUL) in pixels times `scale`, kerning included,
// drawn as font_glyph_or_replacement would. 0 before the font is Ready.
f32 font_measure(const FontAsset& f, const char* str, f32 scale = 1.0f);
// Fills f.ascii and f.replacement from f.codepoints, for code that builds a table itself
// (the pump, tests).
void font_index(FontAsset& f);

} // namespace kv
