// Korvid text: module record, the layout as a function, and the kinds.
//
//   components  Text
//   kinds       text.label  (Transform2, Text)                       a label and nothing else
//               text.glyph  (Transform2, Sprite, Local2, Parent)     cosmetic: never hashed
//   systems     text.layout  phase PostSim, Always: reads *.Text, writes text.glyph
//
// See the module's README.
#pragma once
#include "korvid/asset/font.h"
#include "korvid/core/module.h"
#include "korvid/text/text.h"

namespace kv {

KV_MODULE_DECLARE(text_module);

// One glyph's place in a laid-out line, relative to the label: the quad's center and size
// in the label's units (pixels times the text's scale), and its uv in the font's atlas.
struct GlyphPlacement {
    vec2   center;
    vec2   size;
    rect2  uv;
    Handle material; // the glyph's page, or the font's for a table built by hand
};

// The layout on its own: writes up to `out.len` placements for the visible glyphs of
// `text` in `font` and returns how many there are (which may exceed out.len; nothing past
// it is written). 0 for a font that is not Ready. `width`, if given, receives the line's
// advance width in the label's units.
u32 text_layout(const FontAsset& font, const Text& text, Span<GlyphPlacement> out, f32* width = nullptr);

// Counters, for tests and the overlay.
struct TextStats {
    u32 labels   = 0; // labels seen last tick
    u32 relaid   = 0; // labels whose glyphs were rebuilt last tick
    u32 glyphs   = 0; // glyph rows spawned last tick
};
const TextStats* text_stats(Engine& e);

} // namespace kv
