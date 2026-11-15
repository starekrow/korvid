// Paragraphs on a font built by hand: every glyph advances 10, the space too, nothing kerns
// but 'A' before 'V' (-2). Lines 12 high, the baseline 9 down.
#include "korvid/typeset/typeset.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Fixed {
    static constexpr u32 kCount = 96;
    FontAsset f{};
    u32       cps[kCount];
    Glyph     glyphs[kCount];
    u64       kern_pairs[1] = {(u64)'A' << 32 | 'V'};
    f32       kern_values[1] = {-2.0f};
    Fixed() {
        for (u32 i = 0; i < 95; ++i) cps[i] = 32 + i;
        cps[95] = 0xFFFD;
        for (u32 i = 0; i < kCount; ++i) glyphs[i] = Glyph{rect2{0, 0, 0, 0}, vec2{8, 10}, vec2{5, 4}, 10.0f, Handle{}, Handle{}};
        f.state = AssetState::Ready;
        f.glyph_count = kCount;
        f.codepoints = cps;
        f.glyphs = glyphs;
        f.kern_count = 1;
        f.kern_pairs = kern_pairs;
        f.kern_values = kern_values;
        f.ascent = 9; f.descent = -3; f.line_height = 12;
        font_index(f);
    }
};

// "begin-end|..." of each line, as the text between them.
const char* lines_of(const Typeset& t, const char* text) {
    static char out[512];
    out[0] = 0;
    for (const TypesetLine& l : t.lines) {
        usize n = strlen(out);
        if (n) out[n++] = '|';
        memcpy(out + n, text + l.begin, l.end - l.begin);
        out[n + l.end - l.begin] = 0;
    }
    return out;
}

bool same(const char* got, const char* want) {
    if (!strcmp(got, want)) return true;
    KV_ERROR("test", "typeset: got '%s', wanted '%s'", got, want);
    return false;
}

} // namespace

void test_typeset(kv::TestCtx& c) {
    c.begin("typeset");
    using namespace kv;
    Fixed fx;
    const FontAsset& f = fx.f;
    Typeset t;
    TypesetParams p;

    // Unwrapped: one line; `\n` breaks; a `\n` last leaves an empty line.
    const char* s1 = "one two\nthree\n";
    typeset_paragraph(f, s1, (u32)strlen(s1), p, t);
    KV_CHECK(c, same(lines_of(t, s1), "one two|three|") && t.width == 70 && t.height == 36);
    KV_CHECK(c, t.lines[1].baseline == 12 + 9 && t.glyphs.len == 12);

    // At 75 px: after spaces, the spaces hanging and not counted.
    const char* s2 = "aaa bbb cc dddddddddd ee";
    p.width = 75;
    typeset_paragraph(f, s2, (u32)strlen(s2), p, t);
    // A word longer than the width breaks inside it.
    KV_CHECK(c, same(lines_of(t, s2), "aaa bbb|cc|ddddddd|ddd ee"));
    KV_CHECK(c, t.lines[0].width == 70 && t.lines[2].width == 70 && t.width == 70);
    p.width = 55;
    typeset_paragraph(f, s2, (u32)strlen(s2), p, t);
    KV_CHECK(c, same(lines_of(t, s2), "aaa|bbb|cc|ddddd|ddddd|ee"));
    // After a hyphen or a slash.
    const char* s3 = "well-known and/or";
    p.width = 60;
    typeset_paragraph(f, s3, (u32)strlen(s3), p, t);
    KV_CHECK(c, same(lines_of(t, s3), "well-|known|and/or")); // and/or is exactly 60

    // Alignment: against the width given; with none, against the widest line.
    p.width = 100;
    p.align = TypesetAlign::Center;
    typeset_paragraph(f, "ab\nabcd", 7, p, t);
    KV_CHECK(c, t.lines[0].x == 40 && t.lines[1].x == 30 && t.glyphs[0].x == 40);
    p.width = 0;
    p.align = TypesetAlign::Right;
    typeset_paragraph(f, "ab\nabcd", 7, p, t);
    KV_CHECK(c, t.lines[0].x == 20 && t.lines[1].x == 0);
    p.align = TypesetAlign::Left;

    // Kerning between neighbours on a line, and a line height of its own (the baseline
    // centered in it).
    p.line_height = 20;
    typeset_paragraph(f, "AVA", 3, p, t);
    KV_CHECK(c, t.glyphs[1].x == 8 && t.glyphs[2].x == 18 && t.lines[0].width == 28 && t.height == 20 && t.lines[0].baseline == 13);
    p.line_height = 0;

    // Measure: widest unwrapped, narrowest unbreakable run, height at a width.
    TypesetMeasure m = typeset_measure(f, s2, (u32)strlen(s2), 55);
    KV_CHECK(c, m.widest == 240 && m.narrowest == 100 && m.lines == 6 && m.height == 72);

    // Geometry: the caret, the hit, up and down, a selection, a page.
    p.width = 75;
    typeset_paragraph(f, s2, (u32)strlen(s2), p, t);
    rect2 cr = typeset_caret(t, 5); // the 'b' after "aaa b"
    KV_CHECK(c, cr.x == 50 && cr.y == 0 && cr.h == 12);
    cr = typeset_caret(t, 8); // "cc" starts the second line
    KV_CHECK(c, cr.x == 0 && cr.y == 12);
    KV_CHECK(c, typeset_hit(t, 14, 3) == 1 && typeset_hit(t, 16, 3) == 2 && typeset_hit(t, 500, 3) == 7 && typeset_hit(t, 4, 15) == 8);
    KV_CHECK(c, typeset_hit(t, 0, 999) == 18); // the last line, "ddd ee"
    KV_CHECK(c, typeset_vertical(t, 5, 1, 50) == 10 && typeset_vertical(t, 9, -1, 10) == 1 && typeset_vertical(t, 1, -1, 10) == 0);
    rect2 sel[4];
    u32 ns = typeset_selection(t, 5, 9, sel, 4);
    KV_CHECK(c, ns == 2 && sel[0].x == 50 && sel[0].w > 20 && sel[1].x == 0 && sel[1].w == 10 && sel[1].y == 12);
    KV_CHECK(c, typeset_fit(t, 30) == 11 && typeset_fit(t, 11) == 0); // two lines fit 30 px: through "cc "
    KV_CHECK(c, typeset_line_of(t, 0) == 0 && typeset_line_of(t, 8) == 1 && typeset_line_of(t, (u32)strlen(s2)) == 3);

    // Words and characters, UTF-8 whole.
    const char* w = "foo, bar_baz \xc3\xa9t\xc3\xa9";
    u32 wl = (u32)strlen(w);
    KV_CHECK(c, typeset_next_word(w, wl, 0) == 3 && typeset_next_word(w, wl, 3) == 12 && typeset_prev_word(w, wl, 12) == 5);
    KV_CHECK(c, typeset_next_char(w, wl, 13) == 15 && typeset_prev_char(w, wl, 15) == 13 && typeset_next_word(w, wl, 12) == wl);
    u32 b = 0, e = 0;
    typeset_word_at(w, wl, 7, &b, &e);
    KV_CHECK(c, b == 5 && e == 12);
    typeset_word_at(w, wl, 3, &b, &e);
    KV_CHECK(c, b == 3 && e == 5); // ", ": one run of what is not a word

    // Empty text: one empty line, a caret at its start.
    typeset_paragraph(f, "", 0, p, t);
    KV_CHECK(c, t.lines.len == 1 && t.glyphs.len == 0 && typeset_caret(t, 0).x == 0 && typeset_hit(t, 30, 30) == 0);
    t.free();
}
