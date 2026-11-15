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

struct Layout {
    KromeTree&          t;
    const KromeMeasure& m;

    // ---- up -------------------------------------------------------------------------------

    void want(u32 i, Name font, f32 size) {
        KromeNode& n = t.nodes[i];
        const KromeLook& l = t.looks[n.look];
        if (l.set & KROME_LOOK_FONT) font = l.font;
        if (l.set & KROME_LOOK_SIZE) size = l.size;
        vec2 c{0, 0};
        if (n.type == KromeType::Text) {
            const char* s = t.text(i);
            if (m.text) c = m.text(m.user, s, (u32)strlen(s), font, size);
        } else if (n.type == KromeType::Image) {
            if (m.image && (l.set & KROME_LOOK_IMAGE)) c = m.image(m.user, l.image);
        }
        // Children: along the direction they add up, across it the largest; in a stack the
        // largest each way.
        u32 k = 0;
        f32 along = 0, across = 0;
        bool row = n.dir == KromeDir::Row;
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& cn = t.nodes[ch];
            if (!visible(cn)) continue;
            want(ch, font, size);
            vec2 w = cn.want;
            if (n.dir == KromeDir::Stack) {
                if (w.x > c.x) c.x = w.x;
                if (w.y > c.y) c.y = w.y;
                continue;
            }
            along += row ? w.x : w.y;
            f32 x = row ? w.y : w.x;
            if (x > across) across = x;
            ++k;
        }
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
    }

    // ---- down -----------------------------------------------------------------------------

    // The size a child gets across a container's inside, and along a stack's.
    f32 cross_size(const KromeNode& c, bool horizontal, KromeDir parent_dir, f32 room) {
        KromeSize mode = mode_of(c, horizontal, parent_dir);
        const KromeAxis& a = horizontal ? c.w : c.h;
        f32 want = horizontal ? c.want.x : c.want.y;
        if (mode == KromeSize::Fill) return clampr(room, a);
        return want; // hug and fixed are already clamped
    }

    static f32 aligned(KromeAlign a, f32 room, f32 size) {
        f32 spare = room - size;
        if (spare <= 0 || a == KromeAlign::Start) return 0;
        return a == KromeAlign::Center ? spare * 0.5f : spare;
    }

    void place(u32 i, rect2 r) {
        KromeNode& n = t.nodes[i];
        n.rect = r;
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
        bool scrolls = (n.flags & KROME_SCROLL) != 0;
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
            taken += row ? c.want.x : c.want.y;
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
        (row ? sc.y : sc.x) = 0;
        n.scroll = sc;
        at -= s;

        u32 j = 0;
        for (u32 ch = n.first; ch != KROME_NONE; ch = t.nodes[ch].next) {
            KromeNode& c = t.nodes[ch];
            if (!visible(c)) continue;
            f32 len;
            if (j < fills && fill_at[j] == ch) len = share[j++];
            else len = row ? c.want.x : c.want.y;
            f32 x = cross_size(c, !row, n.dir, across);
            f32 off = aligned(row ? n.align_y : n.align_x, across, x);
            rect2 cr = row ? rect2{in.x + at, in.y + off, len, x} : rect2{in.x + off, in.y + at, x, len};
            place(ch, cr);
            at += len + n.gap;
        }
        n.content = row ? vec2{extent, n.content.y} : vec2{n.content.x, extent};
    }
};

} // namespace

void krome_layout(KromeTree& t, u32 node, rect2 space, const KromeMeasure& m, bool fill) {
    if (node >= t.nodes.len) return;
    Layout l{t, m};
    // Font and size inherit from above the subtree: an overlay's from the node it opened at.
    Name font = name_intern(KROME_DEFAULT_FONT);
    f32 size = KROME_DEFAULT_SIZE;
    {
        u32 chain[KROME_MAX_DEPTH];
        u32 depth = 0;
        for (u32 p = t.nodes[node].parent; p != KROME_NONE && depth < KROME_MAX_DEPTH; p = t.nodes[p].parent) chain[depth++] = p;
        while (depth) {
            const KromeLook& lk = t.looks[t.nodes[chain[--depth]].look];
            if (lk.set & KROME_LOOK_FONT) font = lk.font;
            if (lk.set & KROME_LOOK_SIZE) size = lk.size;
        }
    }
    l.want(node, font, size);
    KromeNode& n = t.nodes[node];
    rect2 r = fill ? space : rect2{space.x, space.y, n.want.x, n.want.y};
    l.place(node, r);
    if (node == t.root) t.dirty = false;
}

rect2 krome_pixel_rect(rect2 r, f32 scale) {
    f32 x0 = floor(r.x * scale + 0.5f), y0 = floor(r.y * scale + 0.5f);
    f32 x1 = floor((r.x + r.w) * scale + 0.5f), y1 = floor((r.y + r.h) * scale + 0.5f);
    return rect2{x0, y0, x1 - x0, y1 - y0};
}

} // namespace kv
