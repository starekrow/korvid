// Behaviours (docs/PRDs/Krome-Slice-4-PRD.md, "Behaviour, in Lua or in C++"): what the
// pointer and the keys do to a node that names one. Registered from C++ here, returned by
// preludes' scripts on the Lua side (lua.cc); the kernel knows neither by name.
#include "internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

namespace {

const KromeBehaviourDesc* cc_behaviour(KromeModule& s, Name name) {
    for (const KromeModule::Behaviour& b : s.behaviours) if (b.name == name) return &b.desc;
    return nullptr;
}

// The state a C++ behaviour keeps for a node: by its id while it has one (so it lasts a
// rebuild), else by where it is.
void* blob_for(KromeModule& s, u32 screen, u32 overlay, u32 node, Name id, Name behave, u32 size) {
    if (!size) return nullptr;
    for (KromeModule::Blob& b : s.blobs) {
        if (b.screen != screen || b.behave != behave) continue;
        if (!id.is_null() ? b.id == id : (b.id.is_null() && b.overlay == overlay && b.node == node)) return b.data;
    }
    void* data = alloc_in(s.general, size, 16);
    memset(data, 0, size);
    s.blobs.alloc = s.general;
    s.blobs.push(KromeModule::Blob{screen, overlay, node, id, behave, data, size});
    return data;
}

KromeTree* widget_tree(const KromeWidget& w, KromeModule** s, KromeScreen** sc) {
    *s = w.e ? krome_state(*w.e) : nullptr;
    if (!*s) return nullptr;
    KromeTree* t = krome_target_tree(**s, KromeTarget{w.screen_id, w.overlay, w.node}, sc);
    if (!t || !*sc || w.node >= t->nodes.len || (t->nodes[w.node].flags & KROME_DEAD)) return nullptr;
    return t;
}

} // namespace

// ---- the C++ instance -------------------------------------------------------------------------------

f64 KromeWidget::number() const {
    KromeModule* s;
    KromeScreen* sc;
    KromeTree* t = widget_tree(*this, &s, &sc);
    f64 v = 0.0;
    if (t && !t->nodes[node].binding.is_null()) krome_model_number(*s, *sc, name_str(t->nodes[node].binding), &v);
    return v;
}

const char* KromeWidget::text() const {
    KromeModule* s;
    KromeScreen* sc;
    KromeTree* t = widget_tree(*this, &s, &sc);
    if (!t || t->nodes[node].binding.is_null()) return "";
    return krome_model_path_text(*s, *sc, name_str(t->nodes[node].binding), s->text, sizeof s->text);
}

void KromeWidget::set(f64 v) {
    KromeModule* s;
    KromeScreen* sc;
    KromeTree* t = widget_tree(*this, &s, &sc);
    if (t && !t->nodes[node].binding.is_null()) krome_model_write_number(*s, *sc, name_str(t->nodes[node].binding), v);
}

void KromeWidget::set(const char* v) {
    KromeModule* s;
    KromeScreen* sc;
    KromeTree* t = widget_tree(*this, &s, &sc);
    if (t && !t->nodes[node].binding.is_null()) krome_model_write_string(*s, *sc, name_str(t->nodes[node].binding), v ? v : "");
}

void KromeWidget::emit(const char* event, f64 value) {
    KromeModule* s;
    KromeScreen* sc;
    KromeTree* t = widget_tree(*this, &s, &sc);
    if (t && event && *event) krome_fire_named(*s, *sc, *t, node, name_intern(event), value);
}

void KromeWidget::part(const char* p, const char* prop, f64 v) {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    KromeValue kv = KromeValue::num((f32)v);
    if (s) krome_set_part(*s, KromeTarget{screen_id, overlay, node}, p, prop, &kv, -1);
}

void KromeWidget::part(const char* p, const char* prop, const char* v) {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    KromeValue kv;
    krome_string_value(v ? v : "", &kv);
    if (s) krome_set_part(*s, KromeTarget{screen_id, overlay, node}, p, prop, &kv, -1);
}

void KromeWidget::part_flag(const char* p, const char* flag, bool on) {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    if (s) krome_set_part(*s, KromeTarget{screen_id, overlay, node}, p, flag, nullptr, on ? 1 : 0);
}

void KromeWidget::open_slot(const char* style) {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    if (s) krome_open_slot(*s, KromeTarget{screen_id, overlay, node}, style);
}

void KromeWidget::close() {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    if (s) krome_close_owned(*s, KromeTarget{screen_id, overlay, node});
}

void KromeWidget::focus_next(i32 delta) {
    KromeModule* s = e ? krome_state(*e) : nullptr;
    if (s) krome_focus_owned(*s, KromeTarget{screen_id, overlay, node}, delta);
}

// ---- parts, slots, picks ------------------------------------------------------------------------------

u32 krome_find_part(const KromeTree& t, u32 node, Name part) {
    for (u32 c = t.nodes[node].first; c != KROME_NONE; c = t.nodes[c].next) {
        if (t.nodes[c].flags & KROME_DEAD) continue;
        if (t.nodes[c].part == part) return c;
        u32 in = krome_find_part(t, c, part);
        if (in != KROME_NONE) return in;
    }
    return KROME_NONE;
}

void krome_set_part(KromeModule& s, const KromeTarget& t, const char* part, const char* prop, const KromeValue* v, i32 flag) {
    KromeScreen* sc = nullptr;
    KromeTree* tree = krome_target_tree(s, t, &sc);
    if (!tree || !sc || t.node >= tree->nodes.len || !part || !prop) return;
    u32 n = krome_find_part(*tree, t.node, name_intern(part));
    if (n == KROME_NONE) { krome_error(s, name_str(sc->name), tree->nodes[t.node].line, nullptr, 0, "no part '%s' under the node", part); return; }
    u8 was_tied = tree->nodes[n].tied;
    const char* bad = flag >= 0 ? tree->set_flag(n, prop, flag != 0) : tree->set(n, prop, *v, KromeState::Base);
    if (!bad && tree->nodes[n].tied != was_tied) {
        u32 up = tree->nodes[n].parent;
        if (up == KROME_NONE || tree->nodes[up].dir != KromeDir::Stack) { tree->nodes[n].tied = was_tied; bad = "places a child of a stack, and this part's parent is not one"; }
    }
    if (bad) { krome_error(s, name_str(sc->name), tree->nodes[n].line, nullptr, 0, "part %s: %s: %s", part, prop, bad); return; }
    sc->repaint = true;
    if (t.overlay) if (KromeOverlay* o = krome_overlay(*sc, t.overlay)) o->placed = false;
}

u32 krome_open_slot(KromeModule& s, const KromeTarget& t, const char* style) {
    KromeScreen* sc = t.overlay ? nullptr : krome_screen_by_id(s, t.screen);
    if (!sc) return 0;
    const KromeSlot* slot = nullptr;
    for (const KromeSlot& k : sc->slots) if (k.node == t.node) slot = &k;
    if (!slot) return 0;
    KromeSlot copy = *slot; // opening may grow the screen's arrays
    u32 id = krome_open_overlay(s, *sc);
    KromeOverlay* o = krome_overlay(*sc, id);
    o->owner = t;
    o->anchor_node = t.node;
    o->side = KromeSide::Below;
    if (!krome_build_slot(s, *sc, *o, copy, style)) { krome_close_overlay(s, *sc, id); return 0; }
    // At least as wide as what opened it.
    if (o->tree.root != KROME_NONE) {
        f32 w = sc->tree.nodes[t.node].rect.w;
        const KromeAxis& a = o->tree.nodes[o->tree.root].w;
        if (a.lo < w) o->tree.set(o->tree.root, "w", KromeValue::range(w, a.hi));
    }
    return id;
}

void krome_close_owned(KromeModule& s, const KromeTarget& t) {
    KromeScreen* sc = krome_screen_by_id(s, t.screen);
    if (!sc) return;
    for (u32 i = sc->overlays.len; i > 0; --i)
        if (i <= sc->overlays.len && sc->overlays[i - 1]->owner == t) krome_close_overlay(s, *sc, sc->overlays[i - 1]->id);
}

bool krome_pick(KromeModule& s, KromeScreen& sc, KromeTree& tree, u32 node) {
    KromeOverlay* o = krome_overlay_of(sc, tree);
    if (!o || o->owner.node == KROME_NONE || node >= tree.nodes.len) return false;
    const KromeNode& n = tree.nodes[node];
    char id[128], value[256];
    format(id, sizeof id, "%s", n.id.is_null() ? "" : name_str(n.id));
    if (n.bind_value != KROME_NONE) format(value, sizeof value, "%s", tree.strings.data + n.bind_value);
    else if (!n.id.is_null()) format(value, sizeof value, "%s", name_str(n.id));
    else {
        // A node with no value of its own: the first text under it (an option's label).
        value[0] = 0;
        for (u32 c = node; c != KROME_NONE && !value[0];) {
            if (tree.nodes[c].type == KromeType::Text) { format(value, sizeof value, "%s", tree.text(c)); break; }
            c = tree.nodes[c].first;
        }
    }
    KromeCallArgs a;
    a.id = id;
    a.text = value;
    return krome_behave(s, o->owner, KromeCall::Pick, a);
}

// ---- registering, and calling ------------------------------------------------------------------------

bool krome_behaviour(Engine& e, const char* name, const KromeBehaviourDesc& desc) {
    KromeModule* s = krome_state(e);
    if (!s || !name || !*name) return false;
    Name n = name_intern(name);
    if (cc_behaviour(*s, n) || krome_lua_has_behaviour(*s, n)) {
        KV_ERROR("krome", "the behaviour '%s' is registered already", name);
        return false;
    }
    s->behaviours.alloc = s->general;
    s->behaviours.push(KromeModule::Behaviour{n, desc});
    return true;
}

bool krome_behaviour_known(KromeModule& s, Name name) { return cc_behaviour(s, name) || krome_lua_has_behaviour(s, name); }

bool krome_prelude_scripts(KromeModule& s, const KimbScope& scope) {
    // The first in a scope is the screen's own file, whose scripts are its handlers.
    for (u32 i = 1; i < scope.count; ++i) {
        const KimbFile* f = scope.list[i];
        if (!f || !f->scripts) continue;
        for (const JsonValue* v = json_first(f->doc, *f->scripts); v; v = json_next(f->doc, *v)) {
            if (!v->str()) continue;
            Name path = name_intern(v->str());
            bool ran = false;
            for (Name r : s.ran_scripts) if (r == path) ran = true;
            if (ran) continue;
            KromeDoc* d = krome_doc(s, path, true);
            if (d->loading) return false;
            s.ran_scripts.alloc = s.general;
            s.ran_scripts.push(path);
            if (d->failed) { krome_error(s, f->file, v->line, nullptr, 0, "script \"%s\": cannot read it", v->str()); continue; }
            krome_lua_prelude(s, d->source, d->source_len, v->str());
        }
    }
    return true;
}

bool krome_behave(KromeModule& s, const KromeTarget& t, KromeCall call, const KromeCallArgs& a) {
    KromeScreen* sc = nullptr;
    KromeTree* tree = t.node != KROME_NONE ? krome_target_tree(s, t, &sc) : nullptr;
    if (!tree || !sc || t.node >= tree->nodes.len) return false;
    const KromeNode& n = tree->nodes[t.node];
    if (n.behave.is_null() || (n.flags & KROME_DEAD)) return false;
    f32 lx = a.x / s.scale - n.rect.x, ly = a.y / s.scale - n.rect.y;
    if (const KromeBehaviourDesc* d = cc_behaviour(s, n.behave)) {
        KromeWidget w;
        w.e = s.e;
        w.screen = sc->name;
        w.id = n.id;
        w.rect = n.rect;
        w.lo = n.lo;
        w.hi = n.hi;
        w.step = n.step;
        w.state = blob_for(s, t.screen, t.overlay, t.node, n.id, n.behave, d->state_size);
        w.focused = krome_is_focused(s, t.screen, t.overlay, t.node);
        w.screen_id = t.screen;
        w.overlay = t.overlay;
        w.node = t.node;
        // What the call does may build the tree again: n is not read after it.
        switch (call) {
        case KromeCall::Press:   if (!d->press) return false; d->press(w, lx, ly, a.button); return true;
        case KromeCall::Drag:    if (!d->drag) return false; d->drag(w, lx, ly); return true;
        case KromeCall::Release: if (!d->release) return false; d->release(w, lx, ly); return true;
        case KromeCall::Wheel:   return d->wheel && d->wheel(w, a.dy);
        case KromeCall::Key: {
            char name[32];
            return d->key && d->key(w, krome_key_name(a.key, name, sizeof name), a.mods);
        }
        case KromeCall::Focus:   if (!d->focus) return false; d->focus(w, a.on); return true;
        case KromeCall::Hover:   if (!d->hover) return false; d->hover(w, a.on); return true;
        case KromeCall::Update:  if (!d->update) return false; d->update(w); return true;
        case KromeCall::Pick:    return d->pick && d->pick(w, a.id ? a.id : "", a.text ? a.text : "");
        }
        return false;
    }
    bool taken = false;
    if (!krome_lua_behave(s, *sc, *tree, t.overlay, t.node, call, lx, ly, a, &taken)) return false;
    return taken;
}

void krome_free_behaviours(KromeModule& s) {
    for (KromeModule::Blob& b : s.blobs) free_in(s.general, b.data, b.size);
    s.blobs.free();
    s.behaviours.free();
    s.ran_scripts.free();
}

const char* krome_key_name(Key k, char* out, usize cap) {
    const char* n = key_name(k);
    usize i = 0;
    for (; n[i] && i + 1 < cap; ++i) out[i] = n[i] >= 'A' && n[i] <= 'Z' ? (char)(n[i] + 32) : n[i];
    out[i] = 0;
    return out;
}

} // namespace kv
