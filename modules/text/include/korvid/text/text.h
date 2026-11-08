// Korvid text: the Text component. Put it on any kind with a Transform2 (the module's own
// kind "text.label" has exactly those two) and the system "text.layout" (phase PostSim,
// every tick) turns the string into glyph sprites: rows of the kind "text.glyph", children
// of the label through Parent and Local2, one per visible character, each with the font's
// atlas material, the glyph's uv and size times `scale`, and the label's tint, layer and
// space. The scene module moves them with the label; a label that dies loses its glyphs
// as orphans. Changing the string, the look or the font is one row write: the layout
// notices and redoes the glyphs. Strings are Latin-1 bytes, one line, up to KV_TEXT_MAX
// including the NUL.
//
// The label's Transform2 is the pen's start on the baseline: Left puts the first glyph
// there, Center and Right shift the line so that point is its middle or its end. The
// line's box runs from the font's descent (below) to its ascent (above), times scale.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"
#include "korvid/render/sprite.h"

namespace kv {

constexpr u32 KV_TEXT_MAX = 64;

enum class TextAlign : u8 { Left, Center, Right };

struct Text {
    Handle      font;                     // asset.font; null or not Ready: nothing is drawn yet
    char        str[KV_TEXT_MAX] = {};    // NUL-terminated Latin-1
    f32         scale = 1.0f;             // 1: the font's pixel size
    color       tint  = color_white();
    i32         layer = 0;
    SpriteSpace space = SpriteSpace::World;
    TextAlign   align = TextAlign::Left;
};

// Formats into `str`, truncating to fit; returns what snprintf would.
int text_set(Text& t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace kv
