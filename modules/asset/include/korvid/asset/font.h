// Korvid asset: fonts as assets. asset_load_font(e, "text/DejaVuSans.ttf", 32) returns a
// handle at once; the record already owns a renderer texture (the glyph atlas, the
// placeholder until built) and a material for it (alpha blend, white, so a sprite's tint
// is the text color). The .ttf reads through the I/O queue; "asset.pump_fonts" (phase
// PreSim, main thread, after "asset.pump") rasterizes the font's character set at the
// pixel size with stb_truetype into an 8-bit atlas, uploads it as RGBA8_UNORM white with
// coverage in alpha, and fills the glyph table and the kerning pairs. AssetReady follows
// once the atlas is resident. Before that the metrics are zero and font_glyph returns
// null for everything, so text lays out as nothing and appears when the font does.
//
// Strings are UTF-8. The character set is a list of code point ranges (FontDesc::ranges);
// the default covers printable ASCII, Latin-1 Supplement, Latin Extended-A, the common
// General Punctuation (dashes, curly quotes, the ellipsis, the bullet), the euro and
// trade marks, and U+FFFD. A code point the font file does not have is left out of the
// table, whatever the ranges say. The same path, size and set is the same handle.
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/math/rect2.h"

namespace kv {

constexpr u32 KV_FONT_MAX_GLYPHS = 8192; // code points per font, after the ranges are merged

// One glyph in the atlas, in pixels at the font's size. A glyph quad is `size` placed so
// its center is `offset` from the pen (x right, y up, the pen on the baseline); then the
// pen moves by `advance` (plus the kerning with the next glyph).
struct Glyph {
    rect2 uv;      // in the atlas, texture space
    vec2  size;    // the quad, pixels; zero for a glyph with no ink (a space)
    vec2  offset;  // quad center from the pen
    f32   advance;
};

// Code points first..last, inclusive.
struct FontRange { u32 first, last; };

// The default character set (see the top of this header).
Span<const FontRange> font_default_ranges();

struct FontDesc {
    f32                   px     = 32.0f;                 // pixels per em-height, in [4, 256]
    Span<const FontRange> ranges = font_default_ranges(); // in any order, overlaps allowed; copied
};

struct FontAsset {
    char       path[KV_ASSET_PATH_MAX];      // as given to asset_load_font
    AssetState state = AssetState::Pending;
    AssetError error = AssetError::None;     // Decode for a file stb_truetype rejects
    Handle     texture;                      // render.texture: the atlas slot, valid from load on
    Handle     material;                     // render.material: the atlas with alpha blend and white tint
    Handle     io;                           // the pending read while reading; null otherwise
    f32        px = 0.0f;                    // the pixel size asked for
    f32        ascent = 0.0f, descent = 0.0f, line_height = 0.0f; // pixels, once Ready; descent is negative
    u32        atlas_width = 0, atlas_height = 0;
    // The glyph table, once Ready: glyphs[i] is code point codepoints[i], ascending.
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
    f64        build_seconds = 0.0;          // what the rasterization cost, for the log
};

// Starts loading <exe dir>/assets/<path> as a TrueType font. Null under the same
// conditions as asset_load_texture, for px outside [4, 256], or for an empty set.
Handle           asset_load_font(Engine& e, const char* path, const FontDesc& desc);
// The default character set at `px`.
Handle           asset_load_font(Engine& e, const char* path, f32 px);
// The record for a handle, or null if the handle is stale or not a font asset.
const FontAsset* asset_font(Engine& e, Handle asset);

// The glyph for a code point, or null if the font is not Ready or does not cover it.
const Glyph* font_glyph_lookup(const FontAsset& f, u32 codepoint);
inline const Glyph* font_glyph(const FontAsset& f, u32 codepoint) {
    if (f.state != AssetState::Ready) return nullptr;
    if (codepoint >= 32 && codepoint < 127) { u16 i = f.ascii[codepoint - 32]; return i ? &f.glyphs[i - 1] : nullptr; }
    return font_glyph_lookup(f, codepoint);
}
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
