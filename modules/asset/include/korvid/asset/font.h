// Korvid asset: fonts as assets. asset_load_font(e, "text/DejaVuSans.ttf", 32) returns a
// handle at once; the record already owns a renderer texture (the glyph atlas, the
// placeholder until built) and a material for it (alpha blend, white, so a sprite's tint
// is the text color). The .ttf reads through the I/O queue; "asset.pump_fonts" (phase
// PreSim, main thread, after "asset.pump") rasterizes printable ASCII and Latin-1 at the
// pixel size with stb_truetype into an 8-bit atlas, uploads it as RGBA8_UNORM white with
// coverage in alpha, and fills the glyph table and the kerning table. AssetReady follows
// once the atlas is resident. Before that the metrics are zero and font_glyph returns
// null for everything, so text lays out as nothing and appears when the font does.
//
// Strings are bytes in Latin-1 (ISO 8859-1): byte 0xE9 is 'é'. UTF-8 is not decoded.
// The same path at the same size is the same handle.
#pragma once
#include "korvid/asset/texture.h"
#include "korvid/math/rect2.h"

namespace kv {

constexpr u32 KV_FONT_FIRST = 32;  // ' '
constexpr u32 KV_FONT_COUNT = 224; // through 0xFF

// One glyph in the atlas, in pixels at the font's size. A glyph quad is `size` placed so
// its center is `offset` from the pen (x right, y up, the pen on the baseline); then the
// pen moves by `advance` (plus the kerning with the next glyph).
struct Glyph {
    rect2 uv;      // in the atlas, texture space
    vec2  size;    // the quad, pixels; zero for a glyph with no ink (a space)
    vec2  offset;  // quad center from the pen
    f32   advance;
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
    Glyph*     glyphs = nullptr;             // KV_FONT_COUNT entries by code point - KV_FONT_FIRST, once Ready
    f32*       kern   = nullptr;             // KV_FONT_COUNT^2 pair adjustments in pixels, or null if the font has none
    f64        build_seconds = 0.0;          // what the rasterization cost, for the log
};

// Starts loading <exe dir>/assets/<path> as a TrueType font at `px` pixels per em-height.
// Null under the same conditions as asset_load_texture, or for px outside [4, 256].
Handle           asset_load_font(Engine& e, const char* path, f32 px);
// The record for a handle, or null if the handle is stale or not a font asset.
const FontAsset* asset_font(Engine& e, Handle asset);

// The glyph for a code point, or null if the font is not Ready or the code point is not
// covered (treat as no ink, no advance).
inline const Glyph* font_glyph(const FontAsset& f, u32 codepoint) {
    if (f.state != AssetState::Ready || codepoint < KV_FONT_FIRST || codepoint >= KV_FONT_FIRST + KV_FONT_COUNT) return nullptr;
    return &f.glyphs[codepoint - KV_FONT_FIRST];
}
// The pen adjustment between two code points, pixels; 0 for anything uncovered.
inline f32 font_kern(const FontAsset& f, u32 a, u32 b) {
    if (!f.kern || a < KV_FONT_FIRST || b < KV_FONT_FIRST || a >= KV_FONT_FIRST + KV_FONT_COUNT || b >= KV_FONT_FIRST + KV_FONT_COUNT) return 0.0f;
    return f.kern[(a - KV_FONT_FIRST) * KV_FONT_COUNT + (b - KV_FONT_FIRST)];
}
// The width of a string (bytes, Latin-1, up to the NUL) in pixels times `scale`, kerning
// included. 0 before the font is Ready.
f32 font_measure(const FontAsset& f, const char* str, f32 scale = 1.0f);

} // namespace kv
