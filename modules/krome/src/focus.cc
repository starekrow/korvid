// Keyboard focus (docs/PRDs/Krome-Slice-4-PRD.md, "Keyboard focus and shortcuts"): any
// focusable node can have it, an edit as before; Tab moves it in document order (taborder
// first), Space and Enter fire what has it, a behaviour gets its keys, and a shortcut fires
// its node from anywhere in the top screen.
#include "internal.h"
#include "korvid/core/engine.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 k_max_focusable = 4096; // in one tree, for Tab; past this the rest are skipped

// The tree Tab goes through: a modal's when the screen has one open, else the overlay the
// focus is in, else the screen's own.
KromeTree* tab_tree(KromeModule& s, KromeScreen& sc, u32* overlay) {
    for (u32 k = sc.overlays.len; k > 0; --k) {
        KromeOverlay& o = *sc.overlays[k - 1];
        if (o.passive) continue;
        if (o.modal) { *overlay = o.id; return &o.tree; }
        break;
    }
    if (s.focus.node != KROME_NONE && s.focus.screen == sc.id && s.focus.overlay) {
        if (KromeOverlay* o = krome_overlay(sc, s.focus.overlay)) { *overlay = o->id; return &o->tree; }
    }
    *overlay = 0;
    return &sc.tree;
}

// What is focusable under a node, in document order; hidden subtrees skipped.
void collect(const KromeTree& t, u32 node, u32* out, u32* n) {
    const KromeNode& nd = t.nodes[node];
    if (nd.flags & (KROME_HIDDEN | KROME_DEAD)) return;
    if (*n < k_max_focusable && krome_focusable(t, node)) out[(*n)++] = node;
    for (u32 c = nd.first; c != KROME_NONE; c = t.nodes[c].next) collect(t, c, out, n);
}

// The scroll boxes a node is in moved so it shows.
void reveal(KromeTree& t, u32 node) {
    rect2 r = t.nodes[node].rect;
    for (u32 p = t.nodes[node].parent; p != KROME_NONE; p = t.nodes[p].parent) {
        KromeNode& b = t.nodes[p];
        if (!krome_scrolls_x(b) && !krome_scrolls_y(b)) continue;
        rect2 in{b.rect.x + b.pad[0], b.rect.y + b.pad[1], b.rect.w - b.pad[0] - b.pad[2], b.rect.h - b.pad[1] - b.pad[3]};
        if (krome_scrolls_y(b)) {
            if (r.y + r.h > in.y + in.h) b.scroll.y += r.y + r.h - (in.y + in.h);
            else if (r.y < in.y) b.scroll.y -= in.y - r.y;
        }
        if (krome_scrolls_x(b)) {
            if (r.x + r.w > in.x + in.w) b.scroll.x += r.x + r.w - (in.x + in.w);
            else if (r.x < in.x) b.scroll.x -= in.x - r.x;
        }
        t.dirty = true;
    }
}

bool hidden_or_disabled(const KromeTree& t, u32 node) {
    for (u32 c = node; c != KROME_NONE; c = t.nodes[c].parent)
        if (t.nodes[c].flags & (KROME_HIDDEN | KROME_DISABLED | KROME_DEAD)) return true;
    return false;
}

// A tree's node with this shortcut: 1 fired, 0 none, -1 one that is disabled (taken, nothing done).
int shortcut_in(KromeModule& s, KromeScreen& sc, KromeTree& t, Key key, u32 mods) {
    for (u32 i = 0; i < t.nodes.len; ++i) {
        const KromeNode& n = t.nodes[i];
        if (n.shortcut != (u16)key || n.shortcut_mods != mods || (n.flags & KROME_DEAD)) continue;
        if (hidden_or_disabled(t, i)) {
            if (n.flags & KROME_HIDDEN) continue; // a hidden page's: not there
            return -1;
        }
        krome_fire(s, sc, t, i, 1.0);
        return 1;
    }
    return 0;
}

} // namespace

KromeTree* krome_focused_tree(KromeModule& s, KromeScreen** out) {
    if (s.focus.node == KROME_NONE) return nullptr;
    KromeScreen* sc = krome_screen_by_id(s, s.focus.screen);
    if (!sc) return nullptr;
    KromeTree* t = &sc->tree;
    if (s.focus.overlay) {
        KromeOverlay* o = krome_overlay(*sc, s.focus.overlay);
        if (!o) return nullptr;
        t = &o->tree;
    }
    if (s.focus.node >= t->nodes.len || (t->nodes[s.focus.node].flags & KROME_DEAD)) return nullptr;
    if (out) *out = sc;
    return t;
}

bool krome_focusable(const KromeTree& t, u32 node) {
    const KromeNode& n = t.nodes[node];
    if (n.flags & (KROME_NOFOCUS | KROME_DEAD)) return false;
    if (hidden_or_disabled(t, node)) return false;
    if (n.flags & KROME_FOCUSABLE) return true;
    return n.type == KromeType::Edit || n.type == KromeType::Hit || !n.event.is_null() || !n.behave.is_null() || !n.binding.is_null();
}

void krome_focus_take(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node, bool keyboard) {
    KromeTree* t = &sc.tree;
    if (overlay) { KromeOverlay* o = krome_overlay(sc, overlay); if (!o) return; t = &o->tree; }
    if (node >= t->nodes.len) return;
    if (krome_is_focused(s, sc.id, overlay, node)) {
        if (s.focus.keyboard != keyboard) { s.focus.keyboard = keyboard; sc.repaint = true; }
        return;
    }
    if (t->nodes[node].type == KromeType::Edit) {
        krome_edit_take(s, sc, overlay, node);
        s.focus.keyboard = keyboard;
        return;
    }
    krome_edit_blur(s);
    // The blur's handler may have closed the overlay, or built the tree again.
    t = &sc.tree;
    if (overlay) { KromeOverlay* o = krome_overlay(sc, overlay); if (!o) return; t = &o->tree; }
    if (node >= t->nodes.len || (t->nodes[node].flags & KROME_DEAD)) return;
    s.focus.screen = sc.id;
    s.focus.overlay = overlay;
    s.focus.node = node;
    s.focus.id = t->nodes[node].id;
    s.focus.keyboard = keyboard;
    s.focus.changed = false;
    s.focus.buf.free();
    sc.repaint = true;
    KromeCallArgs a;
    a.on = true;
    krome_behave(s, KromeTarget{sc.id, overlay, node}, KromeCall::Focus, a);
}

bool krome_focus_tab(KromeModule& s, bool back) {
    KromeScreen* sc = nullptr;
    KromeTree* ft = krome_focused_tree(s, &sc);
    if (ft && ft->nodes[s.focus.node].type == KromeType::Edit && (ft->nodes[s.focus.node].flags & KROME_TABS)) return false;
    if (!sc) sc = krome_top_screen(s);
    if (!sc) return true;
    u32 overlay = 0;
    KromeTree* t = tab_tree(s, *sc, &overlay);
    if (t->root == KROME_NONE) return true;
    MemScope scope;
    u32* list = (u32*)alloc_in(mem_scratch(), sizeof(u32) * k_max_focusable, alignof(u32));
    u32 n = 0;
    collect(*t, t->root, list, &n);
    if (!n) return true;
    // taborder first, lower first; document order within one (an insertion sort keeps it).
    for (u32 i = 1; i < n; ++i)
        for (u32 j = i; j > 0 && t->nodes[list[j - 1]].taborder > t->nodes[list[j]].taborder; --j) {
            u32 x = list[j - 1]; list[j - 1] = list[j]; list[j] = x;
        }
    u32 at = n;
    if (s.focus.node != KROME_NONE && s.focus.screen == sc->id && s.focus.overlay == overlay)
        for (u32 i = 0; i < n; ++i) if (list[i] == s.focus.node) at = i;
    u32 next = at == n ? (back ? n - 1 : 0) : back ? (at + n - 1) % n : (at + 1) % n;
    u32 node = list[next];
    krome_focus_take(s, *sc, overlay, node, true);
    if (KromeTree* t2 = krome_focused_tree(s, nullptr)) {
        reveal(*t2, s.focus.node);
        if (overlay) if (KromeOverlay* o = krome_overlay(*sc, overlay)) o->placed = false;
    }
    sc->repaint = true;
    return true;
}

bool krome_focus_key(KromeModule& s, Key key, u32 mods) {
    KromeScreen* sc = nullptr;
    KromeTree* t = krome_focused_tree(s, &sc);
    if (!t || !sc || t->nodes[s.focus.node].type == KromeType::Edit) return false;
    KromeTarget at{s.focus.screen, s.focus.overlay, s.focus.node};
    if (!t->nodes[at.node].behave.is_null()) {
        KromeCallArgs a;
        a.key = key;
        a.mods = mods;
        if (krome_behave(s, at, KromeCall::Key, a)) return true;
        t = krome_focused_tree(s, &sc);
        if (!t) return false;
    }
    const KromeNode& n = t->nodes[at.node];
    bool plain = !(mods & (KROME_MOD_CTRL | KROME_MOD_ALT | KROME_MOD_CMD));
    KromeOverlay* owned = at.overlay ? krome_overlay(*sc, at.overlay) : nullptr;
    if (owned && owned->owner.node == KROME_NONE) owned = nullptr;
    if ((key == Key::Space || key == Key::Enter) && plain && (!n.event.is_null() || !n.binding.is_null() || owned)) {
        if (!hidden_or_disabled(*t, at.node)) krome_fire(s, *sc, *t, at.node, 1.0);
        return true;
    }
    // In what a behaviour opened: the keys its node does not take are the behaviour's (a
    // dropdown's arrows).
    if (owned) {
        KromeCallArgs a;
        a.key = key;
        a.mods = mods;
        KromeTarget owner = owned->owner;
        if (krome_behave(s, owner, KromeCall::Key, a)) return true;
        t = krome_focused_tree(s, &sc);
        if (!t) return false;
    }
    if (key == Key::PageUp || key == Key::PageDown || key == Key::Home || key == Key::End) {
        for (u32 p = at.node; p != KROME_NONE; p = t->nodes[p].parent) {
            KromeNode& b = t->nodes[p];
            if (!krome_scrolls_y(b)) continue;
            f32 view = b.rect.h - b.pad[1] - b.pad[3];
            if (key == Key::PageUp) b.scroll.y -= view;
            else if (key == Key::PageDown) b.scroll.y += view;
            else b.scroll.y = key == Key::Home ? 0.0f : 1e30f; // layout holds it to the end
            t->dirty = true;
            if (at.overlay) if (KromeOverlay* o = krome_overlay(*sc, at.overlay)) o->placed = false;
            return true;
        }
    }
    return false;
}

void krome_focus_owned(KromeModule& s, const KromeTarget& owner, i32 delta) {
    KromeScreen* sc = krome_screen_by_id(s, owner.screen);
    if (!sc) return;
    KromeOverlay* o = nullptr;
    for (u32 k = sc->overlays.len; k > 0 && !o; --k) if (sc->overlays[k - 1]->owner == owner) o = sc->overlays[k - 1];
    if (!o || o->tree.root == KROME_NONE) return;
    MemScope scope;
    u32* list = (u32*)alloc_in(mem_scratch(), sizeof(u32) * k_max_focusable, alignof(u32));
    u32 n = 0;
    collect(o->tree, o->tree.root, list, &n);
    if (!n) return;
    i32 at = -1;
    if (s.focus.node != KROME_NONE && s.focus.screen == sc->id && s.focus.overlay == o->id)
        for (u32 i = 0; i < n; ++i) if (list[i] == s.focus.node) at = (i32)i;
    i32 next = at < 0 ? (delta >= 0 ? 0 : (i32)n - 1) : at + delta;
    if (next < 0) next = 0;
    if (next >= (i32)n) next = (i32)n - 1;
    krome_focus_take(s, *sc, o->id, list[next], true);
    if (KromeTree* t2 = krome_focused_tree(s, nullptr)) reveal(*t2, s.focus.node);
    o->placed = false;
}

bool krome_shortcut(KromeModule& s, Key key, u32 mods) {
    KromeScreen* sc = krome_top_screen(s);
    if (!sc) return false;
    // A modal holds the keys: only its shortcuts (a dialog's Enter and Escape).
    for (u32 k = sc->overlays.len; k > 0; --k) {
        KromeOverlay& o = *sc->overlays[k - 1];
        if (o.passive) continue;
        if (o.modal) return shortcut_in(s, *sc, o.tree, key, mods) != 0;
    }
    for (u32 k = sc->overlays.len; k > 0; --k) {
        KromeOverlay& o = *sc->overlays[k - 1];
        if (o.passive) continue;
        if (shortcut_in(s, *sc, o.tree, key, mods)) return true;
    }
    return shortcut_in(s, *sc, sc->tree, key, mods) != 0;
}

} // namespace kv
