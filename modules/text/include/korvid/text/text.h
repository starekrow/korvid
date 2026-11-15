// Korvid text: the Text component. Put it on any kind with a Transform2 (the module's own
// kind "text.label" has exactly those two) and the system "text.layout" (phase PostSim,
// every tick) turns the string into glyph sprites: rows of the kind "text.glyph", children
// of the label through Parent and Local2, one per visible character, each with the font's
// atlas material, the glyph's uv and size times `scale`, and the label's tint, layer and
// space. The scene module moves them with the label; a label that dies loses its glyphs
// as orphans. Changing the string, the look or the font is one row write: the layout
// notices and redoes the glyphs. Strings are UTF-8, one line, up to KV_TEXT_MAX bytes
// (not characters) including the NUL. A code point the font lacks draws as U+FFFD if the
// font has that, and as nothing otherwise; a malformed byte is U+FFFD too.
//
// The label's Transform2 is the pen's start on the baseline: Left puts the first glyph
// there, Center and Right shift the line so that point is its middle or its end. The
// line's box runs from the font's descent (below) to its ascent (above), times scale.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"
#include "korvid/render/sprite.h"

namespace kv {

constexpr u32 KV_TEXT_MAX = 64; // bytes of UTF-8, the NUL included

enum class TextAlign : u8 { Left, Center, Right };

struct Text {
    Handle      font;                     // asset.font; null or not Ready: nothing is drawn yet
    char        str[KV_TEXT_MAX] = {};    // NUL-terminated UTF-8; write it with text_set or utf8_copy
    f32         scale = 1.0f;             // 1: the font's pixel size
    color       tint  = color_white();
    i32         layer = 0;
    SpriteSpace space = SpriteSpace::World;
    ScreenAnchor anchor = ScreenAnchor::Center; // Screen: the window point the position is measured from
    TextAlign   align = TextAlign::Left;
    color       shadow = color{0, 0, 0, 0};   // a drop shadow's color; alpha 0: none
    vec2        shadow_offset{2.0f, -2.0f};   // where it falls, in the label's units (pixels for screen text), +y up
    f32         wrap = 0.0f;                  // lines break at this width, the label's units; 0: one line (and `\n`)
    f32         line_height = 0.0f;           // lines this far apart, the label's units; 0: the font's
    u32         subpixel = 0;                 // 1: pens kept to a quarter pixel (typeset), not whole pixels
};

// A label's text past 64 bytes: a string the text module keeps (text_paragraph_set), drawn
// in place of Text::str with the Text's font, look and wrap. `shown` code points of it are
// drawn (all by default), so it can be revealed a character at a time without reflowing.
struct Paragraph {
    u32 string = 0;   // the text module's; 0: none yet
    u32 shown  = ~0u;
};

// Formats into `str`, truncating to fit at a code point boundary; returns what snprintf
// would.
int text_set(Text& t, const char* fmt, ...) KV_PRINTF(2, 3);

} // namespace kv
