// Paragraphs (typeset.h): the text decoded once into items, broken into lines greedily, each
// line's items placed with kerning and aligned; then the geometry an editor asks of it.
#include "korvid/typeset/typeset.h"
#include "korvid/core/arena.h"
#include "korvid/core/utf8.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kNone = ~0u;

struct Item {
    u32          cp;
    u32          byte;
    f32          advance; // the glyph's, kerning not included
    const Glyph* glyph;
};

bool is_space(u32 cp) { return cp == ' ' || cp == '\t' || cp == 0x3000; }
// A line may break after these, as after a run of spaces.
bool breaks_after(u32 cp) { return cp == '-' || cp == 0x2010 || cp == 0x2013 || cp == 0x2014 || cp == '/'; }

bool word_char(u32 cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_' || cp >= 0x80;
}

f32 snap(f32 v) { return floor(v + 0.5f); }

void decode(const FontAsset& f, const char* text, u32 len, Array<Item>& items) {
    u32 at = 0;
    while (at < len) {
        u32 step = 0;
        u32 cp = utf8_decode(text + at, len - at, &step);
        if (!step) step = 1;
        const Glyph* g = cp == '\n' ? nullptr : font_glyph_or_replacement(f, cp);
        items.push(Item{cp, at, g ? g->advance : 0.0f, g});
        at += step;
    }
}

f32 kern_between(const FontAsset& f, const Item* a, const Item& b) { return a && a->cp != '\n' ? font_kern(f, a->cp, b.cp) : 0.0f; }

// Lines over the items: [begin, ink) drawn, [ink, next) hanging (spaces, a `\n`).
struct Span3 { u32 begin, ink, next; };

void break_lines(const FontAsset& f, const Array<Item>& items, f32 width, Array<Span3>& out) {
    u32 n = items.len;
    u32 i = 0;
    while (i < n) {
        u32 start = i, j = i, ink = i, inks = 0;
        u32 last_ok = kNone, last_ink = kNone;
        f32 x = 0.0f;
        u32 next = n;
        bool ended = false;
        while (j < n) {
            const Item& it = items[j];
            if (it.cp == '\n') { next = j + 1; ended = true; break; }
            f32 k = kern_between(f, j > start ? &items[j - 1] : nullptr, it);
            if (is_space(it.cp)) {
                x += k + it.advance;
                ++j;
                last_ok = j;
                last_ink = ink;
                continue;
            }
            f32 nx = x + k + it.advance;
            if (width > 0.0f && nx > width && inks > 0) {
                if (last_ok != kNone) { ink = last_ink; next = last_ok; }
                else next = j;
                ended = true;
                break;
            }
            x = nx;
            ++j;
            ink = j;
            ++inks;
            if (breaks_after(it.cp)) { last_ok = j; last_ink = j; }
        }
        if (!ended) next = n;
        out.push(Span3{start, ink, next});
        i = next;
        // A `\n` last leaves an empty line after it, where a caret can go.
        if (ended && i == n && items[n - 1].cp == '\n') out.push(Span3{n, n, n});
    }
    if (!out.len) out.push(Span3{0, 0, 0});
}

} // namespace

void typeset_paragraph(const FontAsset& font, const char* text, u32 len, const TypesetParams& p, Typeset& out) {
    out.lines.clear();
    out.glyphs.clear();
    out.width = out.height = 0.0f;
    out.len = len;
    if (font.state != AssetState::Ready) return;
    f32 lh = p.line_height > 0.0f ? p.line_height : font.line_height;
    out.line_height = lh;
    out.ascent = font.ascent;
    // Its own working arrays, not the scratch arena's: the caller's output may be there, and
    // a scope opened here would give it back.
    Array<Item> items{};
    decode(font, text ? text : "", len, items);
    Array<Span3> spans{};
    break_lines(font, items, p.width, spans);

    // Each line's glyphs and width; then alignment against the width given, or the widest.
    f32 widest = 0.0f;
    for (u32 li = 0; li < spans.len; ++li) {
        const Span3& sp = spans[li];
        TypesetLine line{};
        line.begin = sp.begin < items.len ? items[sp.begin].byte : len;
        line.end   = sp.ink < items.len ? items[sp.ink].byte : len;
        line.next  = sp.next < items.len ? items[sp.next].byte : len;
        line.top   = (f32)li * lh;
        line.baseline = snap(line.top + (lh - font.line_height) * 0.5f + font.ascent);
        f32 x = 0.0f;
        for (u32 k = sp.begin; k < sp.next; ++k) {
            const Item& it = items[k];
            if (it.cp == '\n') continue;
            f32 kern = kern_between(font, k > sp.begin ? &items[k - 1] : nullptr, it);
            x += kern;
            out.glyphs.push(TypesetGlyph{it.glyph, x, it.advance, it.byte, li, 0});
            if (k > sp.begin) out.glyphs[out.glyphs.len - 2].advance += kern;
            x += it.advance;
            if (k + 1 == sp.ink) line.width = x;
        }
        if (sp.ink == sp.begin) line.width = 0.0f;
        if (line.width > widest) widest = line.width;
        out.lines.push(line);
    }
    out.width = widest;
    out.height = (f32)out.lines.len * lh;
    f32 frame = p.width > 0.0f ? p.width : widest;
    f32 frac = p.align == TypesetAlign::Center ? 0.5f : p.align == TypesetAlign::Right ? 1.0f : 0.0f;
    for (TypesetLine& l : out.lines) l.x = frac ? (frame - l.width) * frac : 0.0f;
    for (TypesetGlyph& g : out.glyphs) {
        g.x += out.lines[g.line].x;
        if (p.subpixel && g.glyph) {
            f32 whole = floor(g.x);
            u32 q = (u32)((g.x - whole) * 4.0f + 0.5f);
            if (q == 4) q = 0;
            g.quarter = q;
            if (q) {
                // The copy at that quarter: the same code point, found again by its byte.
                u32 step = 0;
                u32 cp = utf8_decode(text + g.byte, len - g.byte, &step);
                g.glyph = font_glyph_at(font, cp, q);
            }
        }
    }
    items.free();
    spans.free();
}

TypesetMeasure typeset_measure(const FontAsset& font, const char* text, u32 len, f32 width, f32 line_height) {
    TypesetMeasure m;
    if (font.state != AssetState::Ready) return m;
    MemScope scope;
    Array<Item> items = array_in<Item>(mem_scratch());
    decode(font, text ? text : "", len, items);
    // Unwrapped: the widest line between `\n`s. The narrowest: the widest run that cannot break.
    f32 x = 0.0f, run = 0.0f;
    for (u32 k = 0; k < items.len; ++k) {
        const Item& it = items[k];
        if (it.cp == '\n') { x = 0.0f; run = 0.0f; continue; }
        f32 kern = kern_between(font, k ? &items[k - 1] : nullptr, it);
        if (is_space(it.cp)) { run = 0.0f; x += kern + it.advance; continue; }
        x += kern + it.advance;
        run += kern + it.advance;
        if (x > m.widest) m.widest = x;
        if (run > m.narrowest) m.narrowest = run;
        if (breaks_after(it.cp)) run = 0.0f;
    }
    Array<Span3> spans = array_in<Span3>(mem_scratch());
    break_lines(font, items, width, spans);
    m.lines = spans.len;
    m.height = (f32)spans.len * (line_height > 0.0f ? line_height : font.line_height);
    return m;
}

u32 typeset_line_of(const Typeset& t, u32 byte) {
    for (u32 i = 0; i < t.lines.len; ++i) {
        const TypesetLine& l = t.lines[i];
        if (byte < l.next || i + 1 == t.lines.len) return i;
    }
    return 0;
}

namespace {

// The x of a caret before the glyph at `byte` (or after the last on its line).
f32 caret_x(const Typeset& t, u32 line, u32 byte) {
    const TypesetLine& l = t.lines[line];
    f32 x = l.x;
    for (const TypesetGlyph& g : t.glyphs) {
        if (g.line != line) continue;
        if (g.byte >= byte) return g.x;
        x = g.x + g.advance;
    }
    return x;
}

} // namespace

rect2 typeset_caret(const Typeset& t, u32 byte) {
    if (!t.lines.len) return rect2{0, 0, 1, t.line_height};
    u32 li = typeset_line_of(t, byte);
    return rect2{caret_x(t, li, byte), t.lines[li].top, 1.0f, t.line_height};
}

u32 typeset_hit(const Typeset& t, f32 x, f32 y) {
    if (!t.lines.len) return 0;
    i32 li = t.line_height > 0.0f ? (i32)(y / t.line_height) : 0;
    if (li < 0) li = 0;
    if (li >= (i32)t.lines.len) li = (i32)t.lines.len - 1;
    const TypesetLine& l = t.lines[(u32)li];
    for (const TypesetGlyph& g : t.glyphs) {
        if (g.line != (u32)li) continue;
        if (g.byte >= l.end) break; // past the ink: hanging spaces go to the line's end
        if (x < g.x + g.advance * 0.5f) return g.byte;
    }
    return l.end;
}

u32 typeset_vertical(const Typeset& t, u32 byte, i32 lines, f32 x) {
    if (!t.lines.len) return 0;
    i32 li = (i32)typeset_line_of(t, byte) + lines;
    if (li < 0) return 0;
    if (li >= (i32)t.lines.len) return t.len;
    return typeset_hit(t, x, t.lines[(u32)li].top + t.line_height * 0.5f);
}

u32 typeset_selection(const Typeset& t, u32 from, u32 to, rect2* out, u32 cap) {
    if (from > to) { u32 s = from; from = to; to = s; }
    if (from == to || !t.lines.len) return 0;
    u32 n = 0;
    u32 a = typeset_line_of(t, from), b = typeset_line_of(t, to);
    for (u32 li = a; li <= b && n < cap; ++li) {
        const TypesetLine& l = t.lines[li];
        f32 x0 = li == a ? caret_x(t, li, from) : l.x;
        f32 x1 = li == b ? caret_x(t, li, to) : l.x + l.width + (l.next > l.end ? t.line_height * 0.25f : 0.0f); // a line's break shows as a sliver
        if (x1 > x0) out[n++] = rect2{x0, l.top, x1 - x0, t.line_height};
    }
    return n;
}

u32 typeset_fit(const Typeset& t, f32 height) {
    u32 end = 0;
    for (const TypesetLine& l : t.lines) {
        if (l.top + t.line_height > height + 0.01f) break;
        end = l.next;
    }
    return end;
}

u32 typeset_next_char(const char* text, u32 len, u32 at) {
    if (at >= len) return len;
    ++at;
    while (at < len && ((u8)text[at] & 0xC0) == 0x80) ++at;
    return at;
}

u32 typeset_prev_char(const char* text, u32, u32 at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && ((u8)text[at] & 0xC0) == 0x80) --at;
    return at;
}

namespace {
u32 cp_at(const char* text, u32 len, u32 at) {
    u32 step = 0;
    return at < len ? utf8_decode(text + at, len - at, &step) : 0;
}
} // namespace

u32 typeset_next_word(const char* text, u32 len, u32 at) {
    while (at < len && !word_char(cp_at(text, len, at))) at = typeset_next_char(text, len, at);
    while (at < len && word_char(cp_at(text, len, at))) at = typeset_next_char(text, len, at);
    return at;
}

u32 typeset_prev_word(const char* text, u32 len, u32 at) {
    while (at > 0 && !word_char(cp_at(text, len, typeset_prev_char(text, len, at)))) at = typeset_prev_char(text, len, at);
    while (at > 0 && word_char(cp_at(text, len, typeset_prev_char(text, len, at)))) at = typeset_prev_char(text, len, at);
    return at;
}

void typeset_word_at(const char* text, u32 len, u32 at, u32* begin, u32* end) {
    if (at >= len && len) at = typeset_prev_char(text, len, len);
    bool w = word_char(cp_at(text, len, at));
    u32 b = at, e = at;
    while (b > 0) {
        u32 p = typeset_prev_char(text, len, b);
        u32 cp = cp_at(text, len, p);
        if (word_char(cp) != w || cp == '\n') break;
        b = p;
    }
    while (e < len) {
        u32 cp = cp_at(text, len, e);
        if (word_char(cp) != w || cp == '\n') break;
        e = typeset_next_char(text, len, e);
    }
    *begin = b;
    *end = e;
}

} // namespace kv
