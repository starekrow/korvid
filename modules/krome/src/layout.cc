// Layout: up for what each node wants, down for what it gets (the slice 1 PRD, "Layout").
#include "korvid/krome/tree.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 k_max_fills = 256; // fills in one container that share its space; past this they hug

f32 clampr(f32 v, const KromeAxis& a) { return v < a.lo ? a.lo : v > a.hi ? a.hi : v; }

// The axis a node has, once a bare `fill` is read against its parent's direction.
KromeSize mode_of(const KromeNode& n, bool horizontal, KromeDir parent_dir) {
    const KromeAxis& a = horizontal ? n.w : n.h;
    if (a.set && a.mode != KromeSize::Hug) return a.mode;
    if (a.set) return KromeSize::Hug;
    if (n.flags & KROME_FILL) {
        if (parent_dir == KromeDir::Stack) return KromeSize::Fill;
        if ((parent_dir == KromeDir::Row) == horizontal) return KromeSize::Fill;
    }
    return KromeSize::Hug;
}

bool visible(const KromeNode& n) { return !(n.flags & KROME_HIDDEN); }

bool scrolls_x(const KromeNode& n) { return krome_scrolls_x(n); }
bool scrolls_y(const KromeNode& n) { return krome_scrolls_y(n); }

bool wraps(const KromeNode& n) {
    if (n.type == KromeType::Edit) return (n.flags & KROME_MULTILINE) != 0;
    return n.type == KromeType::Text && !(n.flags & KROME_NOWRAP);
}

// What an edit is measured by: its text, its placeholder while it has none, and never less
// than a space (a caret needs a line).
const char* measured_text(const KromeTree& t, u32 i) {
    const KromeNode& n = t.nodes[i];
    const char* s = t.text(i);
    if (n.type == KromeType::Edit && !*s) s = n.placeholder != KROME_NONE ? t.strings.data + n.placeholder : " ";
    return *s ? s : " ";
}

struct Layout {
    KromeTree&          t;
    const KromeMeasure& m;
    bool                wrapped = false; // some text got less width than one line: heights again

    // ---- up -------------------------------------------------------------------------------

    void want(u32 i, Name font, f32 size, f32 lh) {
        KromeNode& n = t.nodes[i];
        const KromeLook& l = t.looks[n.look];
        if (l.set & KROME_LOOK_FONT) font = l.font;
        if (l.set & KROME_LOOK_SIZE) size = l.size;
        if (l.set & KROME_LOOK_LINE_HEIGHT) lh = l.line_height;
        vec2 c{0, 0};
        f32 narrow = 0.0f;  // the content's least width
        if (n.type == KromeType::Text || n.type == KromeType::Edit) {
            const char* s = n.type == KromeType::Edit ? measured_text(t, i) : t.text(i);
            if (m.paragraph) {
                f32 nw = 0.0f;
                c = m.paragraph(m.user, s, (u32)strlen(s), font, size, lh, 0.0f, &nw);
                narrow = wraps(n) ? nw : c.x;
            } else if (m.text) {
                c = m.text(m.user, s, (u32)strlen(s), font, size);
                narrow = c.x;
            }
        } else if (n.type == KromeType::Image) {
            // A region measures as its own size, the file's whatever it is.
            if (l.set & KROME_LOOK_REGION) c = vec2{l.region[2], l.region[3]};
            else if (m.image && (l.set & KROME_LOOK_IMAGE)) c = m.image(m.user, l.image);
            // A strip, as one of its frames.
            if (l.frames > 1) {
                if (n.flags & KROME_ACROSS) c.x /= (f32)l.frames;
                else c.y /= (f32)l.frames;
            }
        }
        // Children: along the direction they add up, across it the largest; in a stack the
        // largest each way.
        if (n.type == KromeType::Image) narrow = c.x;
        u32 k = 0;
        f32 along = 0, across = 0, narrow_kids = 0;
        bool row = n.dir == KromeDir::Row;
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& cn = t.nodes[ch];
            if (!visible(cn)) continue;
            want(ch, font, size, lh);
            vec2 w = cn.want;
            if (n.dir == KromeDir::Stack) {
                if (w.x > c.x) c.x = w.x;
                if (w.y > c.y) c.y = w.y;
                if (cn.narrowest > narrow_kids) narrow_kids = cn.narrowest;
                continue;
            }
            along += row ? w.x : w.y;
            f32 x = row ? w.y : w.x;
            if (x > across) across = x;
            // A row's children stay side by side, at their own least widths; a column's least
            // width is its widest child's.
            if (row) narrow_kids += cn.narrowest;
            else if (cn.narrowest > narrow_kids) narrow_kids = cn.narrowest;
            ++k;
        }
        if (row && k > 1) narrow_kids += n.gap * (f32)(k - 1);
        if (narrow_kids > narrow) narrow = narrow_kids;
        if (n.dir != KromeDir::Stack && k) {
            along += n.gap * (f32)(k - 1);
            vec2 kids = row ? vec2{along, across} : vec2{across, along};
            if (kids.x > c.x) c.x = kids.x;
            if (kids.y > c.y) c.y = kids.y;
        }
        n.content = c;
        vec2 padded{c.x + n.pad[0] + n.pad[2], c.y + n.pad[1] + n.pad[3]};
        n.want.x = clampr(n.w.mode == KromeSize::Fixed ? n.w.value : padded.x, n.w);
        n.want.y = clampr(n.h.mode == KromeSize::Fixed ? n.h.value : padded.y, n.h);
        f32 least = narrow + n.pad[0] + n.pad[2];
        n.narrowest = n.w.mode == KromeSize::Fixed ? n.want.x : clampr(least < n.want.x ? least : n.want.x, n.w);
    }

    // ---- heights at the widths given -------------------------------------------------------

    // Again from the leaves up, heights only: a wrapped text's at the width it got, and every
    // container's from its children's. Widths do not change.
    void heights(u32 i, Name font, f32 size, f32 lh) {
        KromeNode& n = t.nodes[i];
        const KromeLook& l = t.looks[n.look];
        if (l.set & KROME_LOOK_FONT) font = l.font;
        if (l.set & KROME_LOOK_SIZE) size = l.size;
        if (l.set & KROME_LOOK_LINE_HEIGHT) lh = l.line_height;
        f32 c = 0.0f;
        if ((n.type == KromeType::Text || n.type == KromeType::Edit) && m.paragraph) {
            const char* s = n.type == KromeType::Edit ? measured_text(t, i) : t.text(i);
            f32 inner = n.rect.w - n.pad[0] - n.pad[2];
            f32 nw = 0.0f;
            c = m.paragraph(m.user, s, (u32)strlen(s), font, size, lh, wraps(n) ? inner : 0.0f, &nw).y;
        } else {
            c = n.content.y; // an image's, or what the children added up to before
            if (n.first != KROME_NONE) c = 0.0f;
            if (n.type == KromeType::Image) c = n.content.y;
        }
        u32 k = 0;
        f32 along = 0.0f;
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& cn = t.nodes[ch];
            if (!visible(cn)) continue;
            heights(ch, font, size, lh);
            if (n.dir == KromeDir::Col) { along += cn.want.y; ++k; }
            else if (cn.want.y > c) c = cn.want.y;
        }
        if (n.dir == KromeDir::Col && k) { along += n.gap * (f32)(k - 1); if (along > c) c = along; }
        n.want.y = clampr(n.h.mode == KromeSize::Fixed ? n.h.value : c + n.pad[1] + n.pad[3], n.h);
    }

    // ---- down -----------------------------------------------------------------------------

    // The size a child gets across a container's inside, and along a stack's.
    f32 cross_size(const KromeNode& c, bool horizontal, KromeDir parent_dir, f32 room) {
        KromeSize mode = mode_of(c, horizontal, parent_dir);
        const KromeAxis& a = horizontal ? c.w : c.h;
        f32 want = horizontal ? c.want.x : c.want.y;
        if (mode == KromeSize::Fill) return clampr(room, a);
        if (mode == KromeSize::Percent) return clampr(room * a.value * 0.01f, a);
        // Hugging across: no wider than the room, unless it cannot be narrower (a text wraps
        // to fit; one line, an image or a fixed size does not).
        if (horizontal && mode == KromeSize::Hug && want > room && c.narrowest < want)
            return room > c.narrowest ? room : c.narrowest;
        return want; // hug and fixed are already clamped
    }

    // The length a child takes along its parent's direction: what it wants, or its share of
    // the parent's inside when it is a percentage.
    static f32 along_of(const KromeNode& c, bool row, KromeSize mode, f32 room) {
        if (mode != KromeSize::Percent) return row ? c.want.x : c.want.y;
        const KromeAxis& a = row ? c.w : c.h;
        return clampr(room * a.value * 0.01f, a);
    }

    static f32 aligned(KromeAlign a, f32 room, f32 size) {
        f32 spare = room - size;
        if (spare <= 0 || a == KromeAlign::Start) return 0;
        return a == KromeAlign::Center ? spare * 0.5f : spare;
    }

    void place(u32 i, rect2 r) {
        KromeNode& n = t.nodes[i];
        n.rect = r;
        if (wraps(n) && r.w + 0.01f < n.want.x) wrapped = true;
        if (n.first == KROME_NONE) { n.scroll = vec2{0, 0}; return; }
        rect2 in{r.x + n.pad[0], r.y + n.pad[1], r.w - n.pad[0] - n.pad[2], r.h - n.pad[1] - n.pad[3]};
        if (in.w < 0) in.w = 0;
        if (in.h < 0) in.h = 0;

        if (n.dir == KromeDir::Stack) {
            for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
                KromeNode& c = t.nodes[ch];
                if (!visible(c)) continue;
                f32 w = cross_size(c, true, n.dir, in.w), h = cross_size(c, false, n.dir, in.h);
                KromeAlign ax = c.anchor_x >= 0 ? (KromeAlign)c.anchor_x : n.align_x;
                KromeAlign ay = c.anchor_y >= 0 ? (KromeAlign)c.anchor_y : n.align_y;
                place(ch, rect2{in.x + aligned(ax, in.w, w), in.y + aligned(ay, in.h, h), w, h});
            }
            n.scroll = vec2{0, 0};
            return;
        }

        bool row = n.dir == KromeDir::Row;
        bool scrolls = row ? scrolls_x(n) : scrolls_y(n);
        bool scrolls_across = row ? scrolls_y(n) : scrolls_x(n);
        f32 room = row ? in.w : in.h, across = row ? in.h : in.w;

        // Sizes along: fixed and hugging children take what they want; fills share the rest
        // equally, each held to its range, and one held at its maximum gives the rest back.
        // A scroll box's content has no end, so its fills hug.
        u32 count = 0, fills = 0;
        f32 taken = 0;
        u32 fill_at[k_max_fills];
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& c = t.nodes[ch];
            if (!visible(c)) continue;
            ++count;
            KromeSize mode = mode_of(c, row, n.dir);
            if (mode == KromeSize::Fill && !scrolls && fills < k_max_fills) { fill_at[fills++] = ch; continue; }
            taken += along_of(c, row, mode, room);
        }
        if (count > 1) taken += n.gap * (f32)(count - 1);
        f32 share[k_max_fills];
        bool fixed[k_max_fills] = {};
        f32 left = room - taken;
        u32 open = fills;
        // Equal shares, then pin whoever a share pushes past its range and share again: those
        // under their minimum first (they take more, so the rest get less), and only when
        // there are none, those over their maximum.
        for (u32 pass = 0; pass <= fills && open; ++pass) {
            f32 each = left / (f32)open;
            if (each < 0) each = 0;
            bool pinned = false;
            for (u32 side = 0; side < 2 && !pinned; ++side) {
                for (u32 j = 0; j < fills; ++j) {
                    if (fixed[j]) continue;
                    const KromeAxis& a = row ? t.nodes[fill_at[j]].w : t.nodes[fill_at[j]].h;
                    f32 v = side == 0 ? (each < a.lo ? a.lo : each) : (each > a.hi ? a.hi : each);
                    if (v != each) { share[j] = v; fixed[j] = true; left -= v; --open; pinned = true; }
                }
            }
            if (!pinned) { for (u32 j = 0; j < fills; ++j) if (!fixed[j]) share[j] = each; break; }
        }

        f32 used = taken;
        for (u32 j = 0; j < fills; ++j) used += share[j];
        f32 at = (fills || scrolls) ? 0 : aligned(row ? n.align_x : n.align_y, room, used);
        // A scroll box keeps its offset inside what there is to see.
        vec2 sc = n.scroll;
        f32 extent = used, view = room;
        f32 max_scroll = extent > view ? extent - view : 0;
        f32& s = row ? sc.x : sc.y;
        if (!scrolls) s = 0;
        if (s > max_scroll) s = max_scroll;
        if (s < 0) s = 0;
        f32& cs = row ? sc.y : sc.x;
        if (!scrolls_across || cs < 0) cs = 0;
        f32 cross_scroll = cs;
        n.scroll = sc;
        at -= s;

        f32 wide = 0.0f;
        u32 j = 0;
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& c = t.nodes[ch];
            if (!visible(c)) continue;
            f32 len;
            if (j < fills && fill_at[j] == ch) len = share[j++];
            else len = along_of(c, row, mode_of(c, row, n.dir), room);
            f32 x = cross_size(c, !row, n.dir, across);
            // Scrolling across, a child is as wide as it wants: the box shows part of it.
            if (scrolls_across) { f32 w = row ? c.want.y : c.want.x; if (w > x) x = w; }
            if (x > wide) wide = x;
            f32 off = scrolls_across ? -cross_scroll : aligned(row ? n.align_y : n.align_x, across, x);
            rect2 cr = row ? rect2{in.x + at, in.y + off, len, x} : rect2{in.x + off, in.y + at, x, len};
            place(ch, cr);
            at += len + n.gap;
        }
        // Across, the offset is held inside the widest child; it was used before it was known,
        // so a box whose content shrank under it is placed again.
        f32 most_across = wide > across ? wide - across : 0.0f;
        if (scrolls_across && cross_scroll > most_across) {
            (row ? n.scroll.y : n.scroll.x) = most_across;
            place(i, r);
            return;
        }
        n.content = row ? vec2{extent, scrolls_across ? wide : n.content.y} : vec2{scrolls_across ? wide : n.content.x, extent};
    }
};

} // namespace

void krome_layout(KromeTree& t, u32 node, rect2 space, const KromeMeasure& m, bool fill) {
    if (node >= t.nodes.len) return;
    Layout l{t, m};
    // Font and size inherit from above the subtree: an overlay's from the node it opened at.
    Name font = name_intern(KROME_DEFAULT_FONT);
    f32 size = KROME_DEFAULT_SIZE;
    f32 line_height = 0.0f;
    {
        u32 chain[KROME_MAX_DEPTH];
        u32 depth = 0;
        for (u32 p = t.nodes[node].parent; p != KROME_NONE && depth < KROME_MAX_DEPTH; p = t.nodes[p].parent) chain[depth++] = p;
        while (depth) {
            const KromeLook& lk = t.looks[t.nodes[chain[--depth]].look];
            if (lk.set & KROME_LOOK_FONT) font = lk.font;
            if (lk.set & KROME_LOOK_SIZE) size = lk.size;
            if (lk.set & KROME_LOOK_LINE_HEIGHT) line_height = lk.line_height;
        }
    }
    l.want(node, font, size, line_height);
    KromeNode& n = t.nodes[node];
    rect2 r = fill ? space : rect2{space.x, space.y, n.want.x, n.want.y};
    l.place(node, r);
    // Text that wrapped is taller than one line: heights at the widths given, and placed again.
    if (l.wrapped) {
        l.heights(node, font, size, line_height);
        if (!fill) r.h = n.want.y;
        l.place(node, r);
    }
    if (node == t.root) t.dirty = false;
}

rect2 krome_pixel_rect(rect2 r, f32 scale) {
    f32 x0 = floor(r.x * scale + 0.5f), y0 = floor(r.y * scale + 0.5f);
    f32 x1 = floor((r.x + r.w) * scale + 0.5f), y1 = floor((r.y + r.h) * scale + 0.5f);
    return rect2{x0, y0, x1 - x0, y1 - y0};
}

} // namespace kv
