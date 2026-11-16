// Editing (docs/PRDs/Krome-Slice-3-PRD.md, "Krome"): the one edit that has the keyboard,
// its buffer (typeset's TypesetEdit), and what keys, typed text, an input method and the
// pointer do to it. Drawing it is the view's (view.cc); what needs its lines laid out (a
// press's caret, Up and Down) is resolved there or from the layout it last drew.
#include "internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include "korvid/platform_hid/module.h"
#include <string.h>

namespace kv {

namespace {

constexpr f64 kBlink = 0.53;        // seconds on, then off
constexpr f64 kMultiPress = 0.4;    // a second press this soon, this near, is a double
constexpr f32 kMultiPressPx = 4.0f;

void touch(KromeModule& s) {
    s.focus.blink_from = time_now();
    s.focus.blink_on = true;
    KromeScreen* sc = nullptr;
    if (krome_focus_tree(s, &sc) && sc) sc->repaint = true;
    engine_want_frame(*s.e);
}

// The buffer into the tree (so layout measures it) and the model (so what reads it follows).
void write_back(KromeModule& s) {
    KromeScreen* sc = nullptr;
    KromeTree* t = krome_focus_tree(s, &sc);
    if (!t || !sc) return;
    KromeNode& n = t->nodes[s.focus.node];
    t->set_text(s.focus.node, s.focus.buf.c_str(), s.focus.buf.len);
    t->dirty = true;
    sc->repaint = true;
    if (KromeOverlay* o = s.focus.overlay ? krome_overlay(*sc, s.focus.overlay) : nullptr) o->placed = false;
    if (!n.binding.is_null()) krome_model_write_string(s, *sc, name_str(n.binding), s.focus.buf.c_str());
}

void changed(KromeModule& s) {
    s.focus.changed = true;
    write_back(s);
    touch(s);
}

bool readonly(KromeModule& s) {
    KromeTree* t = krome_focus_tree(s, nullptr);
    return !t || (t->nodes[s.focus.node].flags & KROME_READONLY);
}

bool multiline(KromeModule& s) {
    KromeTree* t = krome_focus_tree(s, nullptr);
    return t && (t->nodes[s.focus.node].flags & KROME_MULTILINE);
}

// The buffer laid out as it was last drawn, for the keys that move by lines.
bool laid(KromeModule& s, Typeset& out) {
    if (!s.focus.font) return false;
    typeset_paragraph(*s.focus.font, s.focus.buf.c_str(), s.focus.buf.len, s.focus.params, out);
    return true;
}

void submit(KromeModule& s) {
    KromeScreen* sc = nullptr;
    KromeTree* t = krome_focus_tree(s, &sc);
    if (!t || !sc) return;
    if (!t->nodes[s.focus.node].event.is_null()) krome_fire_named(s, *sc, *t, s.focus.node, t->nodes[s.focus.node].event, 0.0);
}

// Typed text into the buffer: control characters dropped, a line break a submit in one
// line (and in many with nothing else), each line break its own step.
void insert(KromeModule& s, const char* text, u32 len) {
    if (readonly(s)) return;
    bool many = multiline(s);
    char clean[256];
    u32 n = 0;
    for (u32 i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n' && !many) c = ' ';
        if ((u8)c < 0x20 && c != '\n' && c != '\t') continue;
        if (n + 1 >= sizeof clean) { s.focus.buf.insert(clean, n, time_now()); n = 0; }
        clean[n++] = c;
    }
    if (n) s.focus.buf.insert(clean, n, time_now());
    s.focus.want_x = -1.0f;
    changed(s);
}

} // namespace

KromeTree* krome_focus_tree(KromeModule& s, KromeScreen** out) {
    if (s.focus.node == KROME_NONE) return nullptr;
    KromeScreen* sc = krome_screen_by_id(s, s.focus.screen);
    if (!sc) return nullptr;
    KromeTree* t = &sc->tree;
    if (s.focus.overlay) {
        KromeOverlay* o = krome_overlay(*sc, s.focus.overlay);
        if (!o) return nullptr;
        t = &o->tree;
    }
    if (s.focus.node >= t->nodes.len || (t->nodes[s.focus.node].flags & KROME_DEAD) || t->nodes[s.focus.node].type != KromeType::Edit) return nullptr;
    if (out) *out = sc;
    return t;
}

bool krome_is_focused(const KromeModule& s, u32 screen, u32 overlay, u32 node) {
    return s.focus.node != KROME_NONE && s.focus.screen == screen && s.focus.overlay == overlay && s.focus.node == node;
}

void krome_edit_take(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node) {
    krome_edit_blur(s);
    KromeTree* t = &sc.tree;
    if (overlay) { KromeOverlay* o = krome_overlay(sc, overlay); if (!o) return; t = &o->tree; }
    if (node >= t->nodes.len || (t->nodes[node].flags & KROME_DEAD)) return;
    s.focus.screen = sc.id;
    s.focus.overlay = overlay;
    s.focus.node = node;
    s.focus.id = t->nodes[node].id;
    s.focus.keyboard = false;
    s.focus.buf.set(t->text(node), (u32)strlen(t->text(node)));
    s.focus.scroll = vec2{0, 0};
    s.focus.want_x = -1.0f;
    s.focus.changed = false;
    s.focus.composing[0] = 0;
    s.focus.font = nullptr;
    platform_text_input(*s.e, true);
    touch(s);
}

void krome_edit_press(KromeModule& s, KromeScreen& sc, u32 overlay, u32 node, f32 x, f32 y, bool shift) {
    if (!krome_is_focused(s, sc.id, overlay, node)) {
        krome_edit_take(s, sc, overlay, node);
        if (!krome_is_focused(s, sc.id, overlay, node)) return;
        shift = false;
    }
    // A double press: a word; a triple: a line. Near in time and place to the last.
    f64 now = time_now();
    vec2 at{x, y};
    bool again = now - s.focus.click_time < kMultiPress && fabs(at.x - s.focus.click_at.x) < kMultiPressPx && fabs(at.y - s.focus.click_at.y) < kMultiPressPx;
    s.focus.clicks = again ? (s.focus.clicks >= 3 ? 1 : s.focus.clicks + 1) : 1;
    s.focus.click_time = now;
    s.focus.click_at = at;
    s.focus.pending = true;
    s.focus.pending_at = at;
    s.focus.pending_extend = shift;
    s.focus.pending_clicks = s.focus.clicks;
    touch(s);
}

void krome_edit_drag(KromeModule& s, f32 x, f32 y) {
    if (s.focus.node == KROME_NONE) return;
    // A press not yet resolved (the same frame as the drag) stands: a double press is not a drag.
    if (s.focus.pending && s.focus.pending_clicks) return;
    s.focus.pending = true;
    s.focus.pending_at = vec2{x, y};
    s.focus.pending_extend = true;
    s.focus.pending_clicks = 0;
    touch(s);
}

void krome_edit_blur(KromeModule& s) {
    if (s.focus.node == KROME_NONE) return;
    KromeScreen* sc = nullptr;
    KromeTree* t = krome_focused_tree(s, &sc);
    u32 node = s.focus.node;
    KromeTarget was{s.focus.screen, s.focus.overlay, node};
    bool edit = t && t->nodes[node].type == KromeType::Edit;
    if (t && sc) {
        if (edit && s.focus.changed && !t->nodes[node].change.is_null()) krome_fire_named(s, *sc, *t, node, t->nodes[node].change, 0.0);
        sc->repaint = true;
    }
    s.focus.node = KROME_NONE;
    s.focus.keyboard = false;
    s.focus.changed = false;
    s.focus.composing[0] = 0;
    s.focus.buf.free();
    if (edit) platform_text_input(*s.e, false);
    // A behaviour hears it lost the focus; then the blur event, last: its handler may open
    // or close what it likes.
    if (t && sc && !edit) {
        KromeCallArgs a;
        a.on = false;
        krome_behave(s, was, KromeCall::Focus, a);
        t = krome_target_tree(s, was, &sc);
    }
    if (t && sc && node < t->nodes.len && !(t->nodes[node].flags & KROME_DEAD) && !t->nodes[node].blur.is_null()) krome_fire_named(s, *sc, *t, node, t->nodes[node].blur, 0.0);
}

bool krome_edit_key(KromeModule& s, Key key, bool shift, bool ctrl, bool alt, bool gui) {
    if (s.focus.node == KROME_NONE || !krome_focus_tree(s, nullptr)) return false;
    TypesetEdit& b = s.focus.buf;
    const char* text = b.c_str();
    bool cmd = ctrl || gui;  // Ctrl, or Command on a Mac
    bool ro = readonly(s);
    bool secret = (krome_focus_tree(s, nullptr)->nodes[s.focus.node].flags & KROME_SECRET) != 0;
    bool word = (ctrl || alt) && !secret; // Ctrl, or Option on a Mac; a secret's words are not told apart
    f64 now = time_now();
    switch (key) {
    case Key::Left:
        if (b.selected() && !shift) b.move(b.sel_begin(), false);
        else b.move(word ? typeset_prev_word(text, b.len, b.caret) : typeset_prev_char(text, b.len, b.caret), shift);
        s.focus.want_x = -1.0f;
        break;
    case Key::Right:
        if (b.selected() && !shift) b.move(b.sel_end(), false);
        else b.move(word ? typeset_next_word(text, b.len, b.caret) : typeset_next_char(text, b.len, b.caret), shift);
        s.focus.want_x = -1.0f;
        break;
    case Key::Home:
    case Key::End: {
        bool end = key == Key::End;
        if (cmd || !multiline(s)) { b.move(end ? b.len : 0, shift); break; }
        Typeset t;
        if (laid(s, t)) {
            const TypesetLine& l = t.lines[typeset_line_of(t, b.caret)];
            b.move(end ? l.end : l.begin, shift);
        }
        t.free();
        s.focus.want_x = -1.0f;
        break;
    }
    case Key::Up:
    case Key::Down:
    case Key::PageUp:
    case Key::PageDown: {
        bool down = key == Key::Down || key == Key::PageDown;
        if (!multiline(s)) { b.move(down ? b.len : 0, shift); break; }
        Typeset t;
        if (laid(s, t)) {
            if (s.focus.want_x < 0.0f) s.focus.want_x = typeset_caret(t, b.caret).x;
            i32 lines = 1;
            if (key == Key::PageUp || key == Key::PageDown) {
                lines = t.line_height > 0.0f ? (i32)(s.focus.view_h / t.line_height) - 1 : 1;
                if (lines < 1) lines = 1;
            }
            u32 to = typeset_vertical(t, b.caret, down ? lines : -lines, s.focus.want_x);
            f32 keep = s.focus.want_x;
            b.move(to, shift);
            s.focus.want_x = keep;
        }
        t.free();
        break;
    }
    case Key::Backspace:
        if (ro) return true;
        if (!b.selected() && b.caret == 0) return true;
        b.erase_back(word, now);
        changed(s);
        return true;
    case Key::Delete:
        if (ro) return true;
        if (!b.selected() && b.caret >= b.len) return true;
        b.erase_forward(word, now);
        changed(s);
        return true;
    case Key::Enter: {
        if (multiline(s) && !cmd) { insert(s, "\n", 1); return true; }
        // With nothing to submit, Enter is not the edit's: a dialog's OK (`key=enter`) hears it.
        KromeTree* ft = krome_focus_tree(s, nullptr);
        if (!ft || ft->nodes[s.focus.node].event.is_null()) return false;
        submit(s);
        return true;
    }
    case Key::Escape:
        krome_edit_blur(s);
        return true;
    case Key::A:
        if (!cmd) return false;
        b.select_all();
        break;
    case Key::C:
    case Key::X: {
        if (!cmd) return false;
        if (!b.selected() || secret) return true; // a secret is never copied
        char* sel = (char*)alloc_in(s.general, b.sel_end() - b.sel_begin() + 1, 1);
        memcpy(sel, text + b.sel_begin(), b.sel_end() - b.sel_begin());
        sel[b.sel_end() - b.sel_begin()] = 0;
        platform_clipboard_set(*s.e, sel);
        free_in(s.general, sel, b.sel_end() - b.sel_begin() + 1);
        if (key == Key::X && !ro) { b.erase_selection(); changed(s); }
        return true;
    }
    case Key::V: {
        if (!cmd) return false;
        if (ro) return true;
        Array<char> clip{};
        if (platform_clipboard_get(*s.e, clip) && clip.len) {
            // One line takes it with its line breaks as spaces.
            if (!multiline(s)) for (char& ch : clip) if (ch == '\n' || ch == '\r') ch = ' ';
            u32 n = 0;
            for (u32 i = 0; i < clip.len; ++i) if (clip[i] != '\r') clip[n++] = clip[i];
            b.paste(clip.data, n);
            s.focus.want_x = -1.0f;
            changed(s);
        }
        clip.free();
        return true;
    }
    case Key::Z:
    case Key::Y:
        if (!cmd) return false;
        if (ro) return true;
        if ((key == Key::Z && shift) || key == Key::Y) { if (b.redo_step()) changed(s); }
        else if (b.undo_step()) changed(s);
        return true;
    default:
        return false;
    }
    touch(s);
    return true;
}

void krome_edit_typed(KromeModule& s, const char* utf8) {
    if (s.focus.node == KROME_NONE || !krome_focus_tree(s, nullptr) || !utf8) return;
    // A line break typed (a script, a CLI) is Enter.
    for (const char* p = utf8; *p;) {
        const char* nl = strchr(p, '\n');
        u32 n = nl ? (u32)(nl - p) : (u32)strlen(p);
        if (n) insert(s, p, n);
        if (!nl) break;
        krome_edit_key(s, Key::Enter, false, false, false, false);
        if (s.focus.node == KROME_NONE) return;
        p = nl + 1;
    }
}

void krome_edit_compose(KromeModule& s, const char* text, u32 caret) {
    if (s.focus.node == KROME_NONE || readonly(s)) return;
    usize n = strlen(text);
    if (!n && !s.focus.composing[0]) return; // nothing being composed, and still nothing
    if (n >= sizeof s.focus.composing) n = sizeof s.focus.composing - 1;
    memcpy(s.focus.composing, text, n);
    s.focus.composing[n] = 0;
    s.focus.compose_caret = caret > n ? (u32)n : caret;
    // Composing replaces the selection when it begins, as typing would.
    if (n && s.focus.buf.selected()) { s.focus.buf.erase_selection(); changed(s); }
    touch(s);
}

void krome_edit_frame(KromeModule& s) {
    if (s.focus.node == KROME_NONE) return;
    KromeScreen* sc = nullptr;
    KromeTree* any = krome_focused_tree(s, &sc);
    if (!any || !sc) { s.focus.node = KROME_NONE; s.focus.buf.free(); platform_text_input(*s.e, false); return; }
    KromeTree* t = krome_focus_tree(s, &sc);
    if (!t) return; // not an edit: nothing blinks
    if (s.focus.changed && !t->nodes[s.focus.node].change.is_null()) {
        krome_fire_named(s, *sc, *t, s.focus.node, t->nodes[s.focus.node].change, 0.0);
        s.focus.changed = false;
        t = krome_focus_tree(s, &sc);
        if (!t) return;
    }
    // The blink: a frame when it next turns, only while the window has the focus.
    if (!s.window_focused) return;
    f64 now = time_now();
    f64 since = now - s.focus.blink_from;
    f64 phase = since - (f64)(i64)(since / (2.0 * kBlink)) * (2.0 * kBlink);
    bool on = phase < kBlink;
    if (on != s.focus.blink_on) { s.focus.blink_on = on; sc->repaint = true; }
    engine_want_frame_at(*s.e, now + (on ? kBlink - phase : 2.0 * kBlink - phase));
}

void krome_edit_rebuilt(KromeModule& s, KromeScreen& sc) {
    if (s.focus.node == KROME_NONE || s.focus.screen != sc.id) return;
    KromeOverlay* o = s.focus.overlay ? krome_overlay(sc, s.focus.overlay) : nullptr;
    KromeTree* t = s.focus.overlay ? (o ? &o->tree : nullptr) : &sc.tree;
    u32 n = s.focus.id.is_null() || !t ? KROME_NONE : krome_find(*t, s.focus.id);
    bool edit = n != KROME_NONE && t->nodes[n].type == KromeType::Edit;
    if (n == KROME_NONE || (!edit && s.focus.buf.len) || !krome_focusable(*t, n)) {
        s.focus.node = KROME_NONE;
        s.focus.buf.free();
        platform_text_input(*s.e, false);
        return;
    }
    s.focus.node = n;
    if (edit) t->set_text(n, s.focus.buf.c_str(), s.focus.buf.len); // what was being typed stays
}

} // namespace kv
