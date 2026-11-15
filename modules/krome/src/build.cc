// The one builder (docs/PRDs/Krome-Slice-2-PRD.md, "Lua and .kim together"): a node of a
// document, whether a .kimb's or a script's tables made into one, resolved against its
// scope and the theme, into the kernel's tree.
//
// - Styles merge left to right with the node line over them; a word that is a theme's
//   token is its value; a state's look goes to that state.
// - A word that is a definition is its body, expanded where it is used: its parameters
//   fill `{name}` in the body's texts, everything else on the use line goes onto the
//   body's root over what the body says, and the use's children go where the body's
//   `slot` is. A mistake inside names the body's line and the use's.
// - `each` and `if` build after a hidden anchor of their own; when the model key they read
//   changes, what follows the anchor is built again and nothing else.
#include "internal.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kMaxExpansion = 32; // definitions inside definitions: past this, one uses itself

// What a use line lays over its body's root, innermost first.
struct Over {
    const KimbFile*   file;
    const JsonValue*  use;
    const KromeFrame* frame;  // where the use was written: its paths resolve there
    const KromeFrame* def;    // the definition's frame: its parameters are not properties
    const Over*       next;
};

struct Build {
    KromeModule&        s;
    KromeScreen&        sc;
    KromeTree&          tree;
    Array<KromeDep>*    deps;    // texts and bindings that read the model; null: filled once
    Array<KromeRegion>* regions; // each and if, built again when their key changes; null: built once
    Arena*              frames;
    const KimbScope*    scope;
    const char*         theme;
    u32                 errors = 0;
    bool                waiting = false; // a kim() piece's file is not in yet
    u64                 nodes = 0;
};

const KromeFrame* def_frame(const KromeFrame* f) {
    while (f && !f->def_file) f = f->outer;
    return f;
}

u32 depth_of(const KromeFrame* f) {
    u32 d = 0;
    for (; f; f = f->outer) if (f->def_file) ++d;
    return d;
}

void error(Build& b, const KromeFrame* fr, const char* file, u32 line, const char* fmt, ...) KV_PRINTF(5, 6);
void error(Build& b, const KromeFrame* fr, const char* file, u32 line, const char* fmt, ...) {
    char msg[200];
    va_list args;
    va_start(args, fmt);
    vformat(msg, sizeof msg, fmt, args);
    va_end(args);
    const KromeFrame* d = def_frame(fr);
    krome_error(b.s, file, line, d ? d->use_file->file : nullptr, d ? d->use->line : 0, "%s", msg);
    ++b.errors;
}

char* keep(Build& b, const char* s, usize n) {
    char* d = (char*)arena_alloc(*b.frames, n + 1, 1);
    if (!d) return (char*)"";
    memcpy(d, s, n);
    d[n] = 0;
    return d;
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

// ---- paths and texts --------------------------------------------------------------------------

// A path as written ("item.name", "assets") as a path in the model ("assets.3.name"): an
// each's variable is the element it stands for. Written in a definition's body, it sees the
// eaches around it in that body, not the ones around the use.
const char* model_path(Build& b, const KromeFrame* fr, const char* expr) {
    usize head = strcspn(expr, ".");
    for (const KromeFrame* f = fr; f; f = f->outer) {
        if (f->var && strlen(f->var) == head && !strncmp(f->var, expr, head)) {
            char p[300];
            i32 n = format(p, sizeof p, "%s%s", f->path, expr + head);
            return keep(b, p, (usize)n);
        }
        if (f->def_file) break;
    }
    return expr;
}

u64 head_key(const char* path) {
    usize n = strcspn(path, ".");
    return name_intern(path, n).hash;
}

// A text with its definition's parameters and its eaches' elements filled in as they are
// now; {key} of the model, {{ and {*tag} left for the fill.
const char* expand(Build& b, const KromeFrame* fr, const char* text) {
    if (!fr || !strchr(text, '{')) return text;
    char out[2048];
    usize n = 0;
    for (const char* p = text; *p && n + 1 < sizeof out;) {
        if (p[0] == '{' && p[1] == '{') { out[n++] = *p++; if (n + 1 < sizeof out) out[n++] = *p++; continue; }
        const char* end = *p == '{' ? strchr(p + 1, '}') : nullptr;
        if (!end || p[1] == '*') { out[n++] = *p++; continue; }
        char key[128];
        usize klen = (usize)(end - p - 1);
        if (klen >= sizeof key) klen = sizeof key - 1;
        memcpy(key, p + 1, klen);
        key[klen] = 0;
        const char* value = nullptr;
        char val[256];
        usize head = strcspn(key, ".");
        for (const KromeFrame* f = fr; f && !value; f = f->outer) {
            if (f->var && strlen(f->var) == head && !strncmp(f->var, key, head)) {
                char path[300];
                format(path, sizeof path, "%s%s", f->path, key + head);
                value = krome_model_path_text(b.s, b.sc, path, val, sizeof val);
            } else if (f->def_file) {
                for (u32 i = 0; i < f->params; ++i) if (!strcmp(f->names[i], key)) value = f->values[i];
                break;
            }
        }
        if (value) { for (; *value && n + 1 < sizeof out; ++value) out[n++] = *value; }
        else { for (const char* q = p; q <= end && n + 1 < sizeof out; ++q) out[n++] = *q; }
        p = end + 1;
    }
    return keep(b, out, n);
}

// ---- values -----------------------------------------------------------------------------------

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

bool numeric_string(const JsonValue& v) {
    const char* s = v.str();
    if (!s || !*s) return false;
    char* end = nullptr;
    strtod(s, &end);
    return *end == 0;
}

// One property into a state's look (Base: the node's own), its value resolved first.
void prop(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, u32 line, const char* name, const JsonValue& v, KromeState state) {
    const KimbKernelProp* k = kimb_kernel_prop(name);
    if (!k) { error(b, fr, f.file, line, "'%s' is not a property", name); return; }
    if (state != KromeState::Base && k->layout) { error(b, fr, f.file, line, "%s.%s is layout, which a look cannot set", krome_state_name(state), name); return; }
    const char* why = nullptr;
    const JsonValue* r = kimb_resolve_value(*b.scope, b.theme, k->type, v, &why);
    const JsonDoc* rdoc = &f.doc;
    if (r && r != &v) {
        // A token's value lives in the document of the theme that gave it.
        KimbFound tok;
        if (kimb_find_token(*b.scope, b.theme, v.str(), &tok)) rdoc = &tok.file->doc;
    }
    if (!r && numeric_string(v)) r = &v;
    if (!r) {
        if (v.str()) error(b, fr, f.file, line, "%s: '%s' is not %s, nor a token of theme %s", name, v.str(), kimb_type_name(k->type), b.theme ? b.theme : "(none)");
        else error(b, fr, f.file, line, "%s takes %s", name, kimb_type_name(k->type));
        return;
    }
    KromeValue kv;
    if (!to_value(*rdoc, *r, &kv)) { error(b, fr, f.file, line, "%s takes %s", name, kimb_type_name(k->type)); return; }
    if (const char* bad = b.tree.set(node, name, kv, state)) {
        if (state == KromeState::Base) error(b, fr, f.file, line, "%s: %s", name, bad);
        else error(b, fr, f.file, line, "%s.%s: %s", krome_state_name(state), name, bad);
    }
}

bool is_param(const KromeFrame* def, const char* name) {
    if (!def) return false;
    for (u32 i = 0; i < def->params; ++i) if (!strcmp(def->names[i], name)) return true;
    return false;
}

void props(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, u32 line, const JsonValue* p, KromeState state, const KromeFrame* skip = nullptr) {
    if (!p || !p->is_object()) return;
    for (const JsonValue* v = json_first(f.doc, *p); v; v = json_next(f.doc, *v))
        if (!is_param(skip, v->key)) prop(b, fr, node, f, line, v->key, *v, state);
}

void flags(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, u32 line, const JsonValue* fl) {
    if (!fl || !fl->is_array()) return;
    for (const JsonValue* v = json_first(f.doc, *fl); v; v = json_next(f.doc, *v))
        if (const char* w = v->str())
            if (const char* why = b.tree.flag(node, w)) error(b, fr, f.file, line, "%s: %s", w, why);
}

// `looks`: { state: { props } }, a script's way of giving a node a look per state.
void looks(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, u32 line, const JsonValue* l) {
    if (!l || !l->is_object()) return;
    for (const JsonValue* st = json_first(f.doc, *l); st; st = json_next(f.doc, *st)) {
        KromeState state;
        if (!krome_state_of(st->key, &state)) { error(b, fr, f.file, st->line ? st->line : line, "'%s' is not a state (hover, pressed, focused, disabled, selected)", st->key); continue; }
        props(b, fr, node, f, st->line ? st->line : line, st, state);
    }
}

void style(Build& b, const KromeFrame* fr, u32 node, const char* name, const char* file, u32 line, u32 depth) {
    KimbFound st;
    if (!kimb_find_style(*b.scope, name, &st)) { error(b, fr, file, line, "no style '%s'", name); return; }
    if (depth > 16) { error(b, fr, file, line, "style %s starts from itself", name); return; }
    const KimbFile& f = *st.file;
    u32 sl = st.value->line;
    if (const JsonValue* bases = json_get(f.doc, *st.value, "s"))
        for (const JsonValue* v = json_first(f.doc, *bases); v; v = json_next(f.doc, *v))
            if (v->str()) style(b, fr, node, v->str(), f.file, sl, depth + 1);
    flags(b, fr, node, f, sl, json_get(f.doc, *st.value, "f"));
    props(b, fr, node, f, sl, json_get(f.doc, *st.value, "p"), KromeState::Base);
    looks(b, fr, node, f, sl, json_get(f.doc, *st.value, "looks"));
}

void styles(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, u32 line, const JsonValue* st) {
    if (!st) return;
    if (st->is_array()) {
        for (const JsonValue* v = json_first(f.doc, *st); v; v = json_next(f.doc, *v))
            if (v->str()) style(b, fr, node, v->str(), f.file, line, 0);
    } else if (st->str()) style(b, fr, node, st->str(), f.file, line, 0);
}

void binding(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, const JsonValue* bv) {
    const char* w = bv ? bv->str() : nullptr;
    if (!w) return;
    const char* path = model_path(b, fr, w);
    KromeNode& n = b.tree.nodes[node];
    n.binding = name_intern(path);
    if (krome_model_truthy(b.s, b.sc, path)) n.flags |= KROME_SELECTED;
    else n.flags &= (u16)~KROME_SELECTED;
    if (b.deps) b.deps->push(KromeDep{node, head_key(path), true});
    (void)f;
}

void deps_of(Array<KromeDep>& deps, u32 node, const char* s) {
    for (const char* p = s; (p = strchr(p, '{'));) {
        if (p[1] == '{') { p += 2; continue; }
        const char* end = strchr(p + 1, '}');
        if (!end) break;
        if (end > p + 1 && p[1] != '*') {
            usize n = (usize)(end - p - 1), head = 0;
            while (head < n && p[1 + head] != '.') ++head;
            deps.push(KromeDep{node, name_intern(p + 1, head).hash});
        }
        p = end + 1;
    }
}

// ---- nodes ------------------------------------------------------------------------------------

void node(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, const Over* over);

// The nodes a region holds, built after `parent`'s last child: an each's block once for
// each element, an if's when its value is truthy. How many top-level nodes it made.
u32 region_body(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr) {
    KromeTree& t = b.tree;
    u32 before = t.nodes[parent].last;
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    const JsonValue* kids = json_get(f.doc, n, "c");
    if (!strcmp(w, "each")) {
        const char* var = json_string(json_get(f.doc, n, "v"), nullptr);
        const char* in = json_string(json_get(f.doc, n, "in"), nullptr);
        if (!var || !in) { error(b, fr, f.file, n.line, "each is written 'each item in list { ... }'"); return 0; }
        const char* path = model_path(b, fr, in);
        u32 len = krome_model_length(b.s, b.sc, path);
        for (u32 i = 0; i < len; ++i) {
            KromeFrame* e = (KromeFrame*)arena_alloc(*b.frames, sizeof(KromeFrame), alignof(KromeFrame));
            if (!e) break;
            *e = KromeFrame{};
            e->outer = fr;
            e->var = var;
            char p[300];
            i32 pn = format(p, sizeof p, "%s.%u", path, i + 1);
            e->path = keep(b, p, (usize)pn);
            if (kids) for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k)) node(b, f, *k, parent, e, nullptr);
        }
    } else {
        const char* on = json_string(json_get(f.doc, n, "on"), nullptr);
        if (!on) { error(b, fr, f.file, n.line, "if is written 'if name { ... }'"); return 0; }
        if (krome_model_truthy(b.s, b.sc, model_path(b, fr, on)))
            if (kids) for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k)) node(b, f, *k, parent, fr, nullptr);
    }
    u32 count = 0;
    for (u32 c = before == KROME_NONE ? t.nodes[parent].first : t.nodes[before].next; c != KROME_NONE; c = t.nodes[c].next) ++count;
    return count;
}

void region(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, const Over* over) {
    if (parent == KROME_NONE) { error(b, fr, f.file, n.line, "a screen's root is one node, not an each or an if"); return; }
    if (over) error(b, fr, f.file, n.line, "a definition's body is one node, not an each or an if");
    // The anchor: where the region is, taking no space.
    u32 anchor = b.tree.add(KromeType::Box, parent);
    if (anchor == KROME_NONE) return;
    b.tree.nodes[anchor].flags |= KROME_HIDDEN;
    b.tree.nodes[anchor].line = n.line;
    u32 count = region_body(b, f, n, parent, fr);
    if (!b.regions) return;
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    const char* expr = json_string(json_get(f.doc, n, !strcmp(w, "each") ? "in" : "on"), "");
    KromeRegion r{head_key(model_path(b, fr, expr)), anchor, count, &f, &n, fr, true};
    r.scope = b.scope;
    b.regions->push(r);
}

// Where the use's children go: built in the frame the use was written in.
void slot(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, const Over* over) {
    const KromeFrame* d = def_frame(fr);
    if (!d) { error(b, fr, f.file, n.line, "slot is only in a definition's body"); return; }
    if (over || parent == KROME_NONE) { error(b, fr, f.file, n.line, "a slot is a place in a body, not its root"); return; }
    if (const JsonValue* kids = json_get(d->use_file->doc, *d->use, "c"))
        for (const JsonValue* k = json_first(d->use_file->doc, *kids); k; k = json_next(d->use_file->doc, *k)) node(b, *d->use_file, *k, parent, d->outer, nullptr);
}

bool has_slot(const JsonDoc& doc, const JsonValue& n) {
    if (!strcmp(json_string(json_get(doc, n, "w"), ""), "slot")) return true;
    if (const JsonValue* c = json_get(doc, n, "c"))
        for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k))
            if (has_slot(doc, *k)) return true;
    return false;
}

// A use of a definition: its frame (the parameters, filled in where it is written), then its
// body with this use laid over the body's root.
void expand_def(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, const Over* over, const KimbFound& d) {
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    if (depth_of(fr) >= kMaxExpansion) { error(b, fr, f.file, n.line, "%s uses itself (definitions nested deeper than %u)", w, kMaxExpansion); return; }
    const JsonDoc& dd = d.file->doc;
    KromeFrame* df = (KromeFrame*)arena_alloc(*b.frames, sizeof(KromeFrame), alignof(KromeFrame));
    if (!df) return;
    *df = KromeFrame{};
    df->outer = fr;
    df->use_file = &f;
    df->use = &n;
    df->def_file = d.file;
    const JsonValue* params = json_get(dd, *d.value, "params");
    // Strings in order, then by name; a parameter given neither is empty.
    const JsonValue* t = json_get(f.doc, n, "t");
    const JsonValue* a = json_get(f.doc, n, "a");
    const JsonValue* next_string = a ? json_first(f.doc, *a) : nullptr;
    const JsonValue* p = json_get(f.doc, n, "p");
    u32 given = (t ? 1 : 0) + (a ? a->count() : 0);
    if (params)
        for (const JsonValue* q = json_first(dd, *params); q && df->params < 16; q = json_next(dd, *q)) {
            if (!q->str()) continue;
            const char* value = "";
            const JsonValue* byname = p ? json_get(f.doc, *p, q->str()) : nullptr;
            if (byname) {
                if (byname->str()) value = expand(b, fr, byname->str());
                else if (byname->is_number()) {
                    char num[48];
                    i32 nn = byname->integer() ? format(num, sizeof num, "%lld", (long long)json_i64(byname, 0)) : format(num, sizeof num, "%g", byname->number());
                    value = keep(b, num, (usize)nn);
                } else if (byname->is_bool()) value = json_bool(byname, false) ? "true" : "false";
            } else if (t) { value = expand(b, fr, t->str() ? t->str() : ""); t = nullptr; }
            else if (next_string) { value = expand(b, fr, next_string->str() ? next_string->str() : ""); next_string = json_next(f.doc, *next_string); }
            df->names[df->params] = q->str();
            df->values[df->params] = value;
            ++df->params;
        }
    u32 nparams = params ? params->count() : 0;
    if (given > nparams) error(b, fr, f.file, n.line, "%s takes %u parameter%s, and is given %u string%s", w, nparams, nparams == 1 ? "" : "s", given, given == 1 ? "" : "s");
    const JsonValue* body = json_get(dd, *d.value, "body");
    if (!body) { error(b, fr, d.file->file, d.value->line, "def %s has no body", w); return; }
    if (json_get(f.doc, n, "c") && !has_slot(dd, *body)) error(b, fr, f.file, n.line, "%s takes no children: its body has no slot", w);
    Over o{&f, &n, fr, df, over};
    node(b, *d.file, *body, parent, df, &o);
}

// A piece of markup in a script's tree: kim("file.kimb", "screen") is that screen's root,
// built in that file's scope.
void splice(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr) {
    const char* file = json_string(json_get(f.doc, n, "file"), nullptr);
    const char* screen = json_string(json_get(f.doc, n, "screen"), nullptr);
    if (!file || !screen) { error(b, fr, f.file, n.line, "kim() takes a file and a screen"); return; }
    KromeDoc* d = krome_doc(b.s, name_intern(file), false);
    if (d->failed) { error(b, fr, f.file, n.line, "kim(\"%s\"): the file would not load", file); return; }
    if (!d->ready) { b.waiting = true; return; }
    KimbScope* scope = (KimbScope*)arena_alloc(*b.frames, sizeof(KimbScope), alignof(KimbScope));
    if (!scope) return;
    bool failed = false;
    if (!krome_file_scope(b.s, d->file, *scope, &failed)) { if (failed) error(b, fr, f.file, n.line, "kim(\"%s\"): a prelude it uses would not load", file); else b.waiting = true; return; }
    const JsonValue* root = d->file.screens ? json_get(d->file.doc, *d->file.screens, screen) : nullptr;
    if (!root) { error(b, fr, f.file, n.line, "kim(\"%s\"): no screen '%s'", file, screen); return; }
    const KimbScope* was = b.scope;
    b.scope = scope;
    node(b, d->file, *root, parent, nullptr, nullptr);
    b.scope = was;
}

void node(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, const Over* over) {
    KromeTree& t = b.tree;
    u32 line = n.line;
    if (!n.is_object()) { error(b, fr, f.file, parent != KROME_NONE ? t.nodes[parent].line : 0, "a child that is not a node"); return; }
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    if (!strcmp(w, "each") || !strcmp(w, "if")) { region(b, f, n, parent, fr, over); return; }
    if (!strcmp(w, "slot")) { slot(b, f, n, parent, fr, over); return; }
    if (!strcmp(w, "@kim")) { splice(b, f, n, parent, fr); return; }
    bool ok = false;
    KromeType type = type_of(w, &ok);
    if (!ok) {
        KimbFound d;
        if (kimb_find_def(*b.scope, w, &d)) { expand_def(b, f, n, parent, fr, over, d); return; }
        error(b, fr, f.file, line, "'%s' is neither a kernel node (box, text, image, hit) nor a definition", w);
        return;
    }
    u32 id = t.add(type, parent);
    if (id == KROME_NONE) { error(b, fr, f.file, line, "nesting deeper than %u", KROME_MAX_DEPTH); return; }
    ++b.nodes;
    t.nodes[id].line = line;
    styles(b, fr, id, f, line, json_get(f.doc, n, "s"));
    flags(b, fr, id, f, line, json_get(f.doc, n, "f"));
    props(b, fr, id, f, line, json_get(f.doc, n, "p"), KromeState::Base);
    looks(b, fr, id, f, line, json_get(f.doc, n, "looks"));
    if (const JsonValue* tv = json_get(f.doc, n, "t")) {
        if (const char* raw = tv->str()) {
            const char* text = expand(b, fr, raw);
            t.set_text(id, text);
            if (b.deps) deps_of(*b.deps, id, text);
            else if (strchr(text, '{')) krome_fill_node(b.s, b.sc, t, id);
        }
    }
    if (json_get(f.doc, n, "a")) error(b, fr, f.file, line, "a %s takes one content string", w);
    if (json_get(f.doc, n, "r")) error(b, fr, f.file, line, "a %s takes no range", w);
    if (const char* i = json_string(json_get(f.doc, n, "id"), nullptr)) t.set(id, "id", KromeValue::str(expand(b, fr, i)));
    if (const char* e = json_string(json_get(f.doc, n, "e"), nullptr)) t.set(id, "event", KromeValue::str(e));
    binding(b, fr, id, f, json_get(f.doc, n, "b"));
    // The use lines over the body's root, innermost first: what each says that is not a
    // parameter of its definition.
    for (const Over* o = over; o; o = o->next) {
        const JsonDoc& ud = o->file->doc;
        u32 ul = o->use->line;
        styles(b, o->frame, id, *o->file, ul, json_get(ud, *o->use, "s"));
        flags(b, o->frame, id, *o->file, ul, json_get(ud, *o->use, "f"));
        props(b, o->frame, id, *o->file, ul, json_get(ud, *o->use, "p"), KromeState::Base, o->def);
        looks(b, o->frame, id, *o->file, ul, json_get(ud, *o->use, "looks"));
        if (const char* i = json_string(json_get(ud, *o->use, "id"), nullptr)) t.set(id, "id", KromeValue::str(expand(b, o->frame, i)));
        if (const char* e = json_string(json_get(ud, *o->use, "e"), nullptr)) t.set(id, "event", KromeValue::str(e));
        binding(b, o->frame, id, *o->file, json_get(ud, *o->use, "b"));
        if (json_get(ud, *o->use, "r")) error(b, o->frame, o->file->file, ul, "%s takes no range", json_string(json_get(ud, *o->use, "w"), "?"));
    }
    if (const JsonValue* c = json_get(f.doc, n, "c"))
        for (const JsonValue* k = json_first(f.doc, *c); k; k = json_next(f.doc, *k)) node(b, f, *k, id, fr, nullptr);
}

// ---- rebuilding ---------------------------------------------------------------------------------

// What a rebuild keeps: where each scroll box with an id was scrolled to.
struct Kept { Name id; vec2 scroll; };

void keep_scroll(const KromeTree& t, Array<Kept>& out) {
    for (const KromeNode& n : t.nodes)
        if (!n.id.is_null() && (n.flags & KROME_SCROLL) && !(n.flags & KROME_DEAD)) out.push(Kept{n.id, n.scroll});
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

u32 kill(KromeTree& t, u32 n) {
    u32 count = 1;
    KromeNode& node = t.nodes[n];
    node.flags |= KROME_DEAD | KROME_HIDDEN;
    node.id = Name{};
    node.event = Name{};
    node.binding = Name{};
    for (u32 c = node.first; c != KROME_NONE; c = t.nodes[c].next) count += kill(t, c);
    return count;
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
    keep_scroll(sc.tree, kept);
    sc.tree.clear();
    sc.deps.clear();
    sc.regions.alloc = s.general;
    sc.regions.clear();
    sc.dead = 0;
    if (!sc.frames.base) arena_create(sc.frames, 256u << 20, "krome.frames", MemTag{});
    else arena_reset(sc.frames);
    if (s.under.screen == sc.id) s.under = KromeTarget{};
    if (s.captured.screen == sc.id) s.captured = KromeTarget{};
    Build b{s, sc, sc.tree, &sc.deps, &sc.regions, &sc.frames, &sc.scope, krome_theme_of(s, sc.scope)};
    node(b, *f, *root, KROME_NONE, nullptr, nullptr);
    sc.waiting = b.waiting;
    restore(sc.tree, kept);
    kept.free();
    for (const KromeDep& d : sc.deps) if (!d.binding) krome_fill_node(s, sc, sc.tree, d.node);
    sc.tree.dirty = true;
    sc.repaint = true;
    for (KromeOverlay* o : sc.overlays) krome_build_overlay(s, sc, *o);
    return sc.tree.root != KROME_NONE;
}

bool krome_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o) {
    const JsonValue* root = root_of(o.file, nullptr);
    if (!root) return false;
    o.tree.clear();
    Build b{s, sc, o.tree, nullptr, nullptr, &o.arena, &sc.scope, krome_theme_of(s, sc.scope)};
    node(b, o.file, *root, KROME_NONE, nullptr, nullptr);
    o.placed = false;
    return o.tree.root != KROME_NONE;
}

void krome_rebuild_regions(KromeModule& s, KromeScreen& sc, u64 key) {
    KromeTree& t = sc.tree;
    bool any = false;
    u32 n = sc.regions.len;
    for (u32 i = 0; i < n; ++i) {
        if (!sc.regions[i].alive || sc.regions[i].key != key) continue;
        KromeRegion r = sc.regions[i];
        if (t.nodes[r.anchor].flags & KROME_DEAD) { sc.regions[i].alive = false; continue; }
        any = true;
        u32 parent = t.nodes[r.anchor].parent;
        // What it built goes: out of the list, and dead.
        u32 c = t.nodes[r.anchor].next;
        for (u32 k = 0; k < r.count && c != KROME_NONE; ++k) {
            u32 next = t.nodes[c].next;
            sc.dead += kill(t, c);
            c = next;
        }
        t.nodes[r.anchor].next = c;
        if (c == KROME_NONE) t.nodes[parent].last = r.anchor;
        // Built again at the end of the parent's list, then moved after the anchor.
        u32 tail = t.nodes[parent].last;
        Build b{s, sc, t, &sc.deps, &sc.regions, &sc.frames, r.scope, krome_theme_of(s, sc.scope)};
        u32 deps_before = sc.deps.len;
        u32 count = region_body(b, *r.file, *r.src, parent, r.frame);
        s.rebuilt_nodes += b.nodes;
        if (count && tail != r.anchor) {
            u32 first = t.nodes[tail].next, last = t.nodes[parent].last;
            t.nodes[tail].next = KROME_NONE;
            t.nodes[parent].last = tail;
            t.nodes[last].next = t.nodes[r.anchor].next;
            t.nodes[r.anchor].next = first;
            if (t.nodes[last].next == KROME_NONE) t.nodes[parent].last = last;
        }
        sc.regions[i].count = count;
        for (u32 d = deps_before; d < sc.deps.len; ++d) if (!sc.deps[d].binding) krome_fill_node(s, sc, t, sc.deps[d].node);
        ++s.rebuilt_regions;
    }
    if (!any) return;
    // Regions inside what went, and what read the model there.
    for (KromeRegion& r : sc.regions) if (r.alive && (t.nodes[r.anchor].flags & KROME_DEAD)) r.alive = false;
    u32 w = 0;
    for (u32 i = 0; i < sc.deps.len; ++i) if (!(t.nodes[sc.deps[i].node].flags & KROME_DEAD)) sc.deps[w++] = sc.deps[i];
    sc.deps.len = w;
    u32 rw = 0;
    for (u32 i = 0; i < sc.regions.len; ++i) if (sc.regions[i].alive) sc.regions[rw++] = sc.regions[i];
    sc.regions.len = rw;
    t.dirty = true;
    sc.repaint = true;
    // Past as many dead nodes as live ones, the tree is built whole again, which drops them.
    if (sc.dead > 1024 && sc.dead > t.nodes.len - sc.dead) krome_build_screen(s, sc);
}

} // namespace kv
