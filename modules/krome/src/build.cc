// The one builder (docs/PRDs/Krome-Slice-2-PRD.md, "Lua and .kim together"): a node of a
// document, whether a .kimb's or a script's tables made into one, resolved against its
// scope and the theme, into the kernel's tree. Styles are merged left to right with the
// node line over them; a word that is a theme's token is its value; a state's look goes
// to that state.
#include "internal.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct Build {
    KromeModule&      s;
    KromeScreen&      sc;
    KromeTree&        tree;
    Array<KromeDep>*  deps;   // where texts that read the model are noted; null: filled once
    const KimbScope&  scope;
    const char*       theme;
    const char*       used_file = nullptr; // inside a definition: where it was used
    u32               used_line = 0;
    u32               errors = 0;
};

void error(Build& b, const char* file, u32 line, const char* fmt, ...) KV_PRINTF(4, 5);
void error(Build& b, const char* file, u32 line, const char* fmt, ...) {
    char msg[200];
    va_list args;
    va_start(args, fmt);
    vformat(msg, sizeof msg, fmt, args);
    va_end(args);
    krome_error(b.s, file, line, b.used_file, b.used_line, "%s", msg);
    ++b.errors;
}

KromeType type_of(const char* w, bool* ok) {
    *ok = true;
    if (!strcmp(w, "box")) return KromeType::Box;
    if (!strcmp(w, "text")) return KromeType::Text;
    if (!strcmp(w, "image")) return KromeType::Image;
    if (!strcmp(w, "hit")) return KromeType::Hit;
    *ok = false;
    return KromeType::Box;
}

// A value as the tree takes it: a number, a string ("120..400" a range, "8" a number, as a
// Lua script may write them), or up to four numbers.
bool to_value(const JsonDoc& doc, const JsonValue& v, KromeValue* out) {
    if (v.is_number()) { *out = KromeValue::num((f32)v.number()); return true; }
    if (const char* s = v.str()) {
        const char* dots = strstr(s, "..");
        if (dots && dots - s < 32) {
            char lo_text[32];
            memcpy(lo_text, s, (usize)(dots - s));
            lo_text[dots - s] = 0;
            char* end = nullptr;
            f64 lo = dots == s ? 0.0 : strtod(lo_text, &end);
            if (dots == s || *end == 0) {
                f64 hi = 1e30;
                if (dots[2]) { hi = strtod(dots + 2, &end); if (*end != 0) { *out = KromeValue::str(s); return true; } }
                *out = KromeValue::range((f32)lo, (f32)hi);
                return true;
            }
        }
        if (*s) {
            char* end = nullptr;
            f64 n = strtod(s, &end);
            if (*end == 0) { *out = KromeValue::num((f32)n); return true; }
        }
        *out = KromeValue::str(s);
        return true;
    }
    if (v.is_array()) {
        f32 n[4];
        u32 c = 0;
        for (const JsonValue* x = json_first(doc, v); x; x = json_next(doc, *x)) {
            if (!x->is_number() || c == 4) return false;
            n[c++] = (f32)x->number();
        }
        if (!c) return false;
        *out = KromeValue::nums(n, c);
        return true;
    }
    return false;
}

// A string a script wrote for a number ("8"): what slice 1's tables took.
bool numeric_string(const JsonValue& v) {
    const char* s = v.str();
    if (!s || !*s) return false;
    char* end = nullptr;
    strtod(s, &end);
    return *end == 0;
}

// One property into a state's look (Base: the node's own), its value resolved first.
void prop(Build& b, u32 node, const KimbFile& f, u32 line, const char* name, const JsonValue& v, KromeState state) {
    const KimbKernelProp* k = kimb_kernel_prop(name);
    if (!k) { error(b, f.file, line, "'%s' is not a property", name); return; }
    if (state != KromeState::Base && k->layout) { error(b, f.file, line, "%s.%s is layout, which a look cannot set", krome_state_name(state), name); return; }
    const char* why = nullptr;
    const JsonValue* r = kimb_resolve_value(b.scope, b.theme, k->type, v, &why);
    const JsonDoc* rdoc = &f.doc;
    if (r && r != &v) {
        // A token's value lives in the document of the theme that gave it.
        KimbFound tok;
        if (kimb_find_token(b.scope, b.theme, v.str(), &tok)) rdoc = &tok.file->doc;
    }
    if (!r && numeric_string(v)) r = &v;
    if (!r) {
        if (v.str()) error(b, f.file, line, "%s: '%s' is not %s, nor a token of theme %s", name, v.str(), kimb_type_name(k->type), b.theme ? b.theme : "(none)");
        else error(b, f.file, line, "%s takes %s", name, kimb_type_name(k->type));
        return;
    }
    KromeValue kv;
    if (!to_value(*rdoc, *r, &kv)) { error(b, f.file, line, "%s takes %s", name, kimb_type_name(k->type)); return; }
    if (const char* bad = b.tree.set(node, name, kv, state)) {
        if (state == KromeState::Base) error(b, f.file, line, "%s: %s", name, bad);
        else error(b, f.file, line, "%s.%s: %s", krome_state_name(state), name, bad);
    }
}

void props(Build& b, u32 node, const KimbFile& f, u32 line, const JsonValue* p, KromeState state) {
    if (!p || !p->is_object()) return;
    for (const JsonValue* v = json_first(f.doc, *p); v; v = json_next(f.doc, *v)) prop(b, node, f, line, v->key, *v, state);
}

void flags(Build& b, u32 node, const KimbFile& f, u32 line, const JsonValue* fl) {
    if (!fl || !fl->is_array()) return;
    for (const JsonValue* v = json_first(f.doc, *fl); v; v = json_next(f.doc, *v))
        if (const char* w = v->str())
            if (const char* why = b.tree.flag(node, w)) error(b, f.file, line, "%s: %s", w, why);
}

// `looks`: { state: { props } }, a script's way of giving a node a look per state.
void looks(Build& b, u32 node, const KimbFile& f, u32 line, const JsonValue* l) {
    if (!l || !l->is_object()) return;
    for (const JsonValue* st = json_first(f.doc, *l); st; st = json_next(f.doc, *st)) {
        KromeState state;
        if (!krome_state_of(st->key, &state)) { error(b, f.file, st->line ? st->line : line, "'%s' is not a state (hover, pressed, focused, disabled, selected)", st->key); continue; }
        props(b, node, f, st->line ? st->line : line, st, state);
    }
}

void style(Build& b, u32 node, const char* name, const char* file, u32 line, u32 depth) {
    KimbFound st;
    if (!kimb_find_style(b.scope, name, &st)) { error(b, file, line, "no style '%s'", name); return; }
    if (depth > 16) { error(b, file, line, "style %s starts from itself", name); return; }
    const KimbFile& f = *st.file;
    u32 sl = st.value->line;
    if (const JsonValue* bases = json_get(f.doc, *st.value, "s"))
        for (const JsonValue* v = json_first(f.doc, *bases); v; v = json_next(f.doc, *v))
            if (v->str()) style(b, node, v->str(), f.file, sl, depth + 1);
    flags(b, node, f, sl, json_get(f.doc, *st.value, "f"));
    props(b, node, f, sl, json_get(f.doc, *st.value, "p"), KromeState::Base);
    looks(b, node, f, sl, json_get(f.doc, *st.value, "looks"));
}

void deps_of(Array<KromeDep>& deps, u32 node, const char* s) {
    for (const char* p = s; (p = strchr(p, '{'));) {
        if (p[1] == '{') { p += 2; continue; }
        const char* end = strchr(p + 1, '}');
        if (!end) break;
        if (end > p + 1 && p[1] != '*') deps.push(KromeDep{node, name_intern(p + 1, (usize)(end - p - 1)).hash});
        p = end + 1;
    }
}

bool reads_model(const char* s) {
    for (const char* p = s; (p = strchr(p, '{'));) {
        if (p[1] == '{') { p += 2; continue; }
        if (strchr(p + 1, '}')) return true;
        break;
    }
    return false;
}

void node(Build& b, const KimbFile& f, const JsonValue& n, u32 parent) {
    KromeTree& t = b.tree;
    u32 line = n.line;
    if (!n.is_object()) { error(b, f.file, parent != KROME_NONE ? t.nodes[parent].line : 0, "a child that is not a node"); return; }
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    bool ok = false;
    KromeType type = type_of(w, &ok);
    if (!ok) {
        error(b, f.file, line, "'%s' is neither a kernel node (box, text, image, hit) nor a definition", w);
        return;
    }
    u32 id = t.add(type, parent);
    if (id == KROME_NONE) { error(b, f.file, line, "nesting deeper than %u", KROME_MAX_DEPTH); return; }
    t.nodes[id].line = line;
    if (const JsonValue* st = json_get(f.doc, n, "s")) {
        if (st->is_array()) {
            for (const JsonValue* v = json_first(f.doc, *st); v; v = json_next(f.doc, *v))
                if (v->str()) style(b, id, v->str(), f.file, line, 0);
        } else if (st->str()) style(b, id, st->str(), f.file, line, 0);
    }
    flags(b, id, f, line, json_get(f.doc, n, "f"));
    props(b, id, f, line, json_get(f.doc, n, "p"), KromeState::Base);
    looks(b, id, f, line, json_get(f.doc, n, "looks"));
    if (const JsonValue* tv = json_get(f.doc, n, "t")) {
        if (const char* text = tv->str()) {
            t.set_text(id, text, tv->len());
            if (b.deps) deps_of(*b.deps, id, text);
            else if (reads_model(text)) krome_fill_node(b.s, b.sc, t, id);
        }
    }
    if (json_get(f.doc, n, "a")) error(b, f.file, line, "a %s takes one content string", w);
    if (json_get(f.doc, n, "r")) error(b, f.file, line, "a %s takes no range", w);
    if (const char* i = json_string(json_get(f.doc, n, "id"), nullptr)) t.set(id, "id", KromeValue::str(i));
    if (const char* e = json_string(json_get(f.doc, n, "e"), nullptr)) t.set(id, "event", KromeValue::str(e));
    if (const JsonValue* c = json_get(f.doc, n, "c"))
        for (const JsonValue* k = json_first(f.doc, *c); k; k = json_next(f.doc, *k)) node(b, f, *k, id);
}

// What a rebuild keeps: where each scroll box with an id was scrolled to.
struct Kept { Name id; vec2 scroll; };

void keep(const KromeTree& t, Array<Kept>& out) {
    for (const KromeNode& n : t.nodes)
        if (!n.id.is_null() && (n.flags & KROME_SCROLL)) out.push(Kept{n.id, n.scroll});
}
void restore(KromeTree& t, const Array<Kept>& kept) {
    for (const Kept& k : kept) {
        u32 i = krome_find(t, k.id);
        if (i != KROME_NONE) t.nodes[i].scroll = k.scroll;
    }
}

const JsonValue* root_of(const KimbFile& f, const char* screen) {
    if (!f.screens) return nullptr;
    if (screen) return json_get(f.doc, *f.screens, screen);
    return f.screens->count() ? json_first(f.doc, *f.screens) : nullptr;
}

} // namespace

const char* krome_theme_of(KromeModule& s, const KimbScope& scope) {
    if (!s.theme.is_null() && kimb_has_theme(scope, name_str(s.theme))) return name_str(s.theme);
    return kimb_first_theme(scope);
}

bool krome_build_screen(KromeModule& s, KromeScreen& sc) {
    const KimbFile* f = sc.doc ? &sc.doc->file : sc.has_lua_file ? &sc.lua_file : nullptr;
    if (!f) return false;
    const char* screen = sc.doc ? name_str(sc.screen) : nullptr;
    const JsonValue* root = root_of(*f, screen);
    if (!root) { krome_error(s, f->file, 0, nullptr, 0, "no screen '%s'", screen ? screen : "?"); return false; }
    Array<Kept> kept{};
    keep(sc.tree, kept);
    sc.tree.clear();
    sc.deps.clear();
    if (s.under.screen == sc.id) s.under = KromeTarget{};
    if (s.captured.screen == sc.id) s.captured = KromeTarget{};
    Build b{s, sc, sc.tree, &sc.deps, sc.scope, krome_theme_of(s, sc.scope)};
    node(b, *f, *root, KROME_NONE);
    restore(sc.tree, kept);
    kept.free();
    for (const KromeDep& d : sc.deps) krome_fill_node(s, sc, sc.tree, d.node);
    sc.tree.dirty = true;
    sc.repaint = true;
    for (KromeOverlay* o : sc.overlays) krome_build_overlay(s, sc, *o);
    return sc.tree.root != KROME_NONE;
}

bool krome_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o) {
    const JsonValue* root = root_of(o.file, nullptr);
    if (!root) return false;
    o.tree.clear();
    Build b{s, sc, o.tree, nullptr, sc.scope, krome_theme_of(s, sc.scope)};
    node(b, o.file, *root, KROME_NONE);
    o.placed = false;
    return o.tree.root != KROME_NONE;
}

} // namespace kv
