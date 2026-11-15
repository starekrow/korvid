// typeset: paragraphs (docs/PRDs/Krome-Slice-3-PRD.md, "Paragraphs").
//
//   Typeset p;
//   typeset_paragraph(font, text, len, TypesetParams{.width = 300, .align = TypesetAlign::Center}, p);
//   for (const TypesetGlyph& g : p.glyphs) draw(g.glyph, g.x, g.baseline);
//   p.free();
//
// Breaking: after a run of spaces (which hangs past the edge, uncounted), after a hyphen or
// a dash; `\n` is a hard break; a word too long for its line breaks inside it, between code
// points. Left to right, no shaping beyond kerning. Positions are in the font's pixels
// from the paragraph's top-left; a glyph's x is the pen, its baseline the line's.
#pragma once
#include "korvid/asset/font.h"
#include "korvid/core/array.h"

namespace kv {

enum class TypesetAlign : u8 { Left, Center, Right };

struct TypesetParams {
    f32          width = 0.0f;       // where lines break; <= 0: only at `\n`
    TypesetAlign align = TypesetAlign::Left;
    f32          line_height = 0.0f; // a line's height in pixels; <= 0: the font's
    bool         subpixel = false;   // pens kept to a quarter pixel (else whole pixels for each glyph)
};

struct TypesetLine {
    u32 begin, end;   // bytes: the line's text, its hanging spaces and its `\n` not included
    u32 next;         // where the next line starts (after the spaces and the `\n`)
    f32 x;            // where the line starts, after alignment
    f32 width;        // its ink's advance, the hanging spaces not counted
    f32 top, baseline;
};

struct TypesetGlyph {
    const Glyph* glyph;   // null for a code point that draws nothing (nothing in any face)
    f32          x;       // the pen, before this glyph
    f32          advance; // to the next glyph's pen, kerning included
    u32          byte;    // where its code point starts
    u32          line;
    u32          quarter; // the subpixel copy, 0 to 3 (0 unless asked)
};

struct Typeset {
    Array<TypesetLine>  lines{};
    Array<TypesetGlyph> glyphs{};   // every code point but `\n`, in order
    f32 width = 0.0f, height = 0.0f; // the widest line; the lines' heights
    f32 line_height = 0.0f, ascent = 0.0f;
    u32 len = 0;                     // the text's bytes

    void free() { lines.free(); glyphs.free(); }
};

// Lays out [text, text + len). Arrays from their own allocators (the general one unless set).
void typeset_paragraph(const FontAsset& font, const char* text, u32 len, const TypesetParams& p, Typeset& out);

// The measure of a paragraph without keeping its glyphs: the widest line unwrapped (what it
// wants), the narrowest width it can take (its longest unbreakable run), and its height
// broken at `width`.
struct TypesetMeasure { f32 widest = 0.0f, narrowest = 0.0f, height = 0.0f; u32 lines = 0; };
TypesetMeasure typeset_measure(const FontAsset& font, const char* text, u32 len, f32 width, f32 line_height = 0.0f);

// ---- geometry, on a laid-out paragraph --------------------------------------------------------

// The line holding a byte offset (a caret at a line's end is on that line).
u32   typeset_line_of(const Typeset& t, u32 byte);
// Where a caret at a byte offset goes: its x, and its line's top and height.
rect2 typeset_caret(const Typeset& t, u32 byte);
// The byte offset nearest a point: the line under it, then the side of the glyph's middle.
u32   typeset_hit(const Typeset& t, f32 x, f32 y);
// The offset one line up (-1) or down (+1) nearest `x` (the caret's remembered column).
u32   typeset_vertical(const Typeset& t, u32 byte, i32 lines, f32 x);
// The rectangles a selection covers, one per line it touches; how many, up to `cap`.
u32   typeset_selection(const Typeset& t, u32 from, u32 to, rect2* out, u32 cap);
// The bytes of the first `lines_in` lines that fit a height: where a page ends.
u32   typeset_fit(const Typeset& t, f32 height);

// ---- the text itself, by code point and word ----------------------------------------------------

u32  typeset_next_char(const char* text, u32 len, u32 at); // past the code point at `at`
u32  typeset_prev_char(const char* text, u32 len, u32 at); // to the start of the one before
u32  typeset_next_word(const char* text, u32 len, u32 at); // to the end of the next word
u32  typeset_prev_word(const char* text, u32 len, u32 at); // to the start of the word before
// The word around `at` (letters, digits and _; or one run of anything else).
void typeset_word_at(const char* text, u32 len, u32 at, u32* begin, u32* end);

} // namespace kv
