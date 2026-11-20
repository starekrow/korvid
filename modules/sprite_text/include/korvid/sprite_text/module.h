// Korvid sprite_text: module record, the layout as a function, and the ktables.
//
//   components  Text
//   ktables     sprite_text.label  (Transform2, Text)                       a label and nothing else
//               sprite_text.glyph  (Transform2, Sprite, Local2, Parent)     cosmetic: never hashed
//   systems     sprite_text.layout  phase PostSim, Always: reads *.Text, writes sprite_text.glyph
//
// See the module's README.
#pragma once
#include "korvid/asset/font.h"
#include "korvid/core/module.h"
#include "korvid/sprite_text/sprite_text.h"

namespace kv {

KV_MODULE_DECLARE(sprite_text_module);

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
u32 sptext_layout(const FontAsset& font, const Text& text, Span<GlyphPlacement> out, f32* width = nullptr);

// Counters, for tests and the overlay.
struct SptextStats {
    u32 labels   = 0; // labels seen last tick
    u32 relaid   = 0; // labels whose glyphs were rebuilt last tick
    u32 glyphs   = 0; // glyph rows spawned last tick
};
const SptextStats* sptext_stats(Engine& e);

// The layout over several lines: a wrap, a `\n`, or a paragraph's string (`str`, of any
// length), its first `shown` glyphs. The pen is on the first line's baseline, lines go down
// (-y), aligned as one line is: Left from the pen, Center and Right about it. Appends to
// `out`; returns how many.
u32 sptext_layout_paragraph(const FontAsset& font, const Text& t, const char* str, u32 shown, Array<GlyphPlacement>& out);

// A paragraph's string: set (made the first time), read, and given back when the label goes.
// Sim code writes them, so a replay writes the same ones; the handle is what is hashed.
void        sptext_paragraph_set(Engine& e, Paragraph& p, const char* utf8);
const char* sptext_paragraph_get(Engine& e, const Paragraph& p);
void        sptext_paragraph_free(Engine& e, Paragraph& p);
// How many code points a paragraph has: what `shown` counts to.
u32         sptext_paragraph_length(Engine& e, const Paragraph& p);
// Where a paragraph's page ends at a height (the label's units): the code points of the lines
// that fit, for a game that shows it a page at a time.
u32         sptext_paragraph_fit(Engine& e, const Text& t, const Paragraph& p, f32 height);

} // namespace kv
