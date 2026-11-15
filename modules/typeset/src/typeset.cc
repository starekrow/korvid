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

namespace kv {

namespace {

constexpr f64 kGroupPause = 1.0; // seconds of no typing that end a group

void replace_raw(TypesetEdit& e, u32 at, u32 remove, const char* s, u32 n) {
    if (at > e.len) at = e.len;
    if (at + remove > e.len) remove = e.len - at;
    u32 grow = n > remove ? n - remove : 0;
    e.text.reserve(e.len + grow + 1);
    if (e.text.len < e.len + 1) e.text.resize_fill(e.len + 1, 0);
    if (grow) e.text.resize_fill(e.len + grow + 1, 0);
    memmove(e.text.data + at + n, e.text.data + at + remove, e.len - at - remove);
    if (n) memcpy(e.text.data + at, s, n);
    e.len = e.len + n - remove;
    e.text.data[e.len] = 0;
}

u32 pool_put(Array<char>& pool, const char* s, u32 n) {
    u32 off = pool.len;
    if (n) memcpy(pool.push_n(n), s, n);
    return off;
}

// One change, recorded for undo in the current group (or a new one).
void change(TypesetEdit& e, u32 at, u32 remove, const char* s, u32 n, TypesetEdit::Kind kind, bool joins) {
    if (!joins || e.open != kind) { ++e.group; e.open = kind; }
    TypesetEdit::Step st{};
    st.at = at;
    st.removed = pool_put(e.undo_pool, e.text.data ? e.text.data + at : "", remove);
    st.removed_len = remove;
    st.inserted = pool_put(e.undo_pool, s, n);
    st.inserted_len = n;
    st.caret_before = e.caret;
    st.anchor_before = e.anchor;
    st.group = e.group;
    e.undo.push(st);
    e.redo.clear();
    e.redo_pool.clear();
    replace_raw(e, at, remove, s, n);
    e.caret = e.anchor = at + n;
    if (kind == TypesetEdit::Kind::Other) e.open = TypesetEdit::Kind::None;
    // Past the cap the oldest group goes, and the pool is made again from what is kept.
    if (e.undo.len > e.max_steps) {
        u32 drop = 0, g = e.undo[0].group;
        while (drop < e.undo.len && e.undo[drop].group == g) ++drop;
        Array<char> pool{};
        for (u32 i = drop; i < e.undo.len; ++i) {
            TypesetEdit::Step s2 = e.undo[i];
            s2.removed = pool_put(pool, e.undo_pool.data + e.undo[i].removed, e.undo[i].removed_len);
            s2.inserted = pool_put(pool, e.undo_pool.data + e.undo[i].inserted, e.undo[i].inserted_len);
            e.undo[i - drop] = s2;
        }
        e.undo.len -= drop;
        e.undo_pool.free();
        e.undo_pool = pool;
    }
}

bool is_space_byte(char c) { return c == ' ' || c == '\t'; }

} // namespace

void TypesetEdit::set(const char* s, u32 n) {
    len = 0;
    text.clear();
    replace_raw(*this, 0, 0, s ? s : "", n);
    caret = anchor = len;
    undo.clear(); redo.clear(); undo_pool.clear(); redo_pool.clear();
    open = Kind::None;
}

void TypesetEdit::insert(const char* s, u32 n, f64 now) {
    bool newline = memchr(s, '\n', n) != nullptr;
    u32 at = sel_begin(), remove = sel_end() - at;
    // Typing continues a group: at the caret, soon after the last, and not a word begun after
    // a space.
    bool joins = !remove && !newline && now - last < kGroupPause && n > 0 && undo.len;
    if (joins && at > 0 && is_space_byte(text.data[at - 1]) && !is_space_byte(s[0])) joins = false;
    if (joins) {
        const Step& p = undo.last();
        if (p.at + p.inserted_len != at) joins = false;
    }
    change(*this, at, remove, s, n, remove || newline ? Kind::Other : Kind::Type, joins);
    last = now;
}

void TypesetEdit::paste(const char* s, u32 n) {
    u32 at = sel_begin();
    change(*this, at, sel_end() - at, s, n, Kind::Other, false);
}

void TypesetEdit::erase_selection() {
    if (!selected()) return;
    u32 at = sel_begin();
    change(*this, at, sel_end() - at, "", 0, Kind::Other, false);
}

void TypesetEdit::erase_back(bool word, f64 now) {
    if (selected()) { erase_selection(); return; }
    if (caret == 0) return;
    u32 from = word ? typeset_prev_word(c_str(), len, caret) : typeset_prev_char(c_str(), len, caret);
    bool joins = !word && now - last < kGroupPause && undo.len && undo.last().at == caret;
    change(*this, from, caret - from, "", 0, word ? Kind::Other : Kind::Delete, joins);
    last = now;
}

void TypesetEdit::erase_forward(bool word, f64 now) {
    if (selected()) { erase_selection(); return; }
    if (caret >= len) return;
    u32 to = word ? typeset_next_word(c_str(), len, caret) : typeset_next_char(c_str(), len, caret);
    bool joins = !word && now - last < kGroupPause && undo.len && undo.last().at == caret;
    change(*this, caret, to - caret, "", 0, word ? Kind::Other : Kind::Delete, joins);
    last = now;
}

void TypesetEdit::move(u32 at, bool extend) {
    if (at > len) at = len;
    while (at > 0 && at < len && ((u8)text.data[at] & 0xC0) == 0x80) --at;
    caret = at;
    if (!extend) anchor = at;
    open = Kind::None;
}

bool TypesetEdit::undo_step() {
    if (!undo.len) return false;
    u32 g = undo.last().group;
    while (undo.len && undo.last().group == g) {
        Step st = undo.pop();
        // The inverse: what was inserted goes, what was removed comes back. Kept for redo.
        Step back{};
        back.at = st.at;
        back.removed = pool_put(redo_pool, text.data + st.at, st.inserted_len);
        back.removed_len = st.inserted_len;
        back.inserted = pool_put(redo_pool, undo_pool.data + st.removed, st.removed_len);
        back.inserted_len = st.removed_len;
        back.caret_before = caret;
        back.anchor_before = anchor;
        back.group = g;
        redo.push(back);
        replace_raw(*this, st.at, st.inserted_len, undo_pool.data + st.removed, st.removed_len);
        caret = st.caret_before;
        anchor = st.anchor_before;
        undo_pool.len = st.removed; // the pool's tail was this step's
    }
    open = Kind::None;
    return true;
}

bool TypesetEdit::redo_step() {
    if (!redo.len) return false;
    u32 g = redo.last().group;
    while (redo.len && redo.last().group == g) {
        Step st = redo.pop();
        Step fwd{};
        fwd.at = st.at;
        fwd.removed = pool_put(undo_pool, text.data + st.at, st.inserted_len);
        fwd.removed_len = st.inserted_len;
        fwd.inserted = pool_put(undo_pool, redo_pool.data + st.removed, st.removed_len);
        fwd.inserted_len = st.removed_len;
        fwd.caret_before = caret;
        fwd.anchor_before = anchor;
        fwd.group = g;
        undo.push(fwd);
        replace_raw(*this, st.at, st.inserted_len, redo_pool.data + st.removed, st.removed_len);
        caret = anchor = st.at + st.removed_len;
        redo_pool.len = st.removed;
    }
    open = Kind::None;
    return true;
}

} // namespace kv
