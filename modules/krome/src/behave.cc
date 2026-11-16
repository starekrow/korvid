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
