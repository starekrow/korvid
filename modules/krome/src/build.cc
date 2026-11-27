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
#include "korvid/math/scalar.h"
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
    u32                 overlay = 0;     // the overlay being built (0: the screen's tree)
    // A whole rebuild's virtualized eaches' windows, in the old tree's document order: each
    // region built again takes the first not yet taken that was built from the same markup.
    struct Window { const JsonValue* src; u32 first, want; bool taken; };
    Array<Window>*      windows = nullptr;
    Array<KromeSlot>*   slots = nullptr; // uses' children kept for w:open_slot; null: not kept
    // A sparse each built again: its rows as they were, any of which whose value did not
    // change is moved into place rather than built (KR22), and the nodes so moved.
    Array<KromeSparseRow>* reuse  = nullptr;
    Array<u32>*            reused = nullptr;
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
    if (!strcmp(w, "edit")) return KromeType::Edit;
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
// The binding the nearest definition's use gave (`slider @level`), as a model path where
// that use was written; null when there is none.
const char* use_binding(Build& b, const KromeFrame* fr) {
    const KromeFrame* d = def_frame(fr);
    if (!d || !d->use || !d->use_file) return nullptr;
    const char* w = json_string(json_get(d->use_file->doc, *d->use, "b"), nullptr);
    if (!w) return nullptr;
    char key[256];
    if (const char* is = strchr(w, '=')) { format(key, sizeof key, "%.*s", (int)(is - w), w); w = key; }
    const char* path = model_path(b, d->outer, w);
    return path == key ? keep(b, key, strlen(key)) : path;
}

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
        bool from_model = false;
        char val[256];
        usize head = strcspn(key, ".");
        // {@}: the binding the definition's use was given, as the model shows it.
        if (!strcmp(key, "@")) {
            if (const char* bp = use_binding(b, fr)) { format(val, sizeof val, "{%s}", bp); value = val; }
        }
        for (const KromeFrame* f = fr; f && !value; f = f->outer) {
            if (f->var && strlen(f->var) == head && !strncmp(f->var, key, head)) {
                char path[300];
                format(path, sizeof path, "%s%s", f->path, key + head);
                value = krome_model_path_text(b.s, b.sc, path, val, sizeof val);
                from_model = true;
            } else if (f->def_file) {
                for (u32 i = 0; i < f->params; ++i) if (!strcmp(f->names[i], key)) value = f->values[i];
                break;
            }
        }
        // A model value is text as it is: its braces doubled, so the fill shows them rather
        // than reading them as keys. A parameter is markup, passed on as written.
        if (value && from_model) { for (; *value && n + 2 < sizeof out; ++value) { if (*value == '{') out[n++] = '{'; out[n++] = *value; } }
        else if (value) { for (; *value && n + 1 < sizeof out; ++value) out[n++] = *value; }
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
    if (const char* s = v.str()) { krome_string_value(s, out); return true; }
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

} // namespace

void krome_string_value(const char* s, KromeValue* out) {
    const char* dots = strstr(s, "..");
    if (dots && dots - s < 32) {
        char lo_text[32];
        memcpy(lo_text, s, (usize)(dots - s));
        lo_text[dots - s] = 0;
        char* end = nullptr;
        f64 lo = dots == s ? 0.0 : strtod(lo_text, &end);
        if (dots == s || *end == 0) {
            f64 hi = 1e30;
            if (dots[2]) { hi = strtod(dots + 2, &end); if (*end != 0) { *out = KromeValue::str(s); return; } }
            *out = KromeValue::range((f32)lo, (f32)hi);
            return;
        }
    }
    if (*s) {
        char* end = nullptr;
        f64 n = strtod(s, &end);
        if (*end == 0) { *out = KromeValue::num((f32)n); return; }
    }
    *out = KromeValue::str(s);
}

const char* krome_apply_bound(KromeModule& s, KromeScreen& sc, KromeTree& tree, const KromeDep& d) {
    char buf[256];
    KromeValue v;
    if (!krome_model_value(s, sc, name_str(d.path), &v, buf, sizeof buf)) return nullptr; // nothing there: left as it was
    return tree.set(d.node, name_str(d.prop), v, (KromeState)d.state);
}

namespace {

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
    // Bound to the model (`w=@key`): set from it now, and again whenever the key changes.
    if (const char* at = v.str(); at && at[0] == '@') {
        const char* path = at[1] == '@' ? use_binding(b, fr) : model_path(b, fr, at + 1);
        if (!path) { error(b, fr, f.file, line, "%s=@@: only in a definition's body, used with a binding", name); return; }
        KromeDep d{node, head_key(path), KromeDep::Prop, (u8)state, name_intern(name), name_intern(path)};
        if (const char* bad = krome_apply_bound(b.s, b.sc, b.tree, d)) error(b, fr, f.file, line, "%s=%s: %s", name, at, bad);
        if (b.deps) b.deps->push(d);
        return;
    }
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
    // `font=digits`: a bitmap font written inline in the scope, by its key.
    KimbFound inline_font;
    if (!strcmp(name, "font") && kv.kind == KromeValue::String && kimb_find_font(*b.scope, kv.s, &inline_font))
        kv = KromeValue::str(krome_inline_font(b.s, inline_font, kv.s));
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

void binding(Build& b, const KromeFrame* fr, u32 node, const KimbFile& f, const JsonValue* bv, const JsonValue* eq) {
    const char* w = bv ? bv->str() : nullptr;
    if (!w) return;
    // `@key=value` is a binding by equality: markup gives the value apart ("bv"), a Lua
    // table writes it in the one string (bind = "tab=rules").
    const char* value = eq ? eq->str() : nullptr;
    char key[256];
    if (!value) {
        if (const char* is = strchr(w, '=')) {
            format(key, sizeof key, "%.*s", (int)(is - w), w);
            value = is + 1;
            w = key;
        }
    }
    const char* path = model_path(b, fr, w);
    KromeNode& n = b.tree.nodes[node];
    n.binding = name_intern(path);
    n.bind_value = value ? b.tree.string(value, (u32)strlen(value)) : KROME_NONE;
    if (n.type == KromeType::Edit) {
        // An edit's binding is its text, both ways.
        krome_model_into_text(b.s, b.sc, path, b.tree, node);
    } else if (krome_bound_selected(b.s, b.sc, b.tree, node)) n.flags |= KROME_SELECTED;
    else n.flags &= ~KROME_SELECTED;
    if (b.deps) b.deps->push(KromeDep{node, head_key(path), KromeDep::Binding});
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
// A spacer in a virtualized each: the height of `rows` elements not built, less the gap
// after the last of them (the parent puts one after the spacer).
void spacer(Build& b, u32 parent, u32 rows, f32 height) {
    if (!rows) return;
    u32 id = b.tree.add(KromeType::Box, parent);
    if (id == KROME_NONE) return;
    f32 gap = b.tree.nodes[parent].gap;
    f32 h = (f32)rows * (height + gap) - gap;
    b.tree.set(id, "h", KromeValue::num(h > 0.0f ? h : 0.0f));
}

bool has_region(const JsonDoc& doc, const JsonValue& n) {
    const char* w = json_string(json_get(doc, n, "w"), "");
    if (!strcmp(w, "each") || !strcmp(w, "if")) return true;
    if (const JsonValue* c = json_get(doc, n, "c"))
        for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k))
            if (has_region(doc, *k)) return true;
    return false;
}

// Appends an existing node, detached, as parent's last child.
void append_node(KromeTree& t, u32 parent, u32 id) {
    t.nodes[id].next = KROME_NONE;
    if (t.nodes[parent].last == KROME_NONE) t.nodes[parent].first = id;
    else t.nodes[t.nodes[parent].last].next = id;
    t.nodes[parent].last = id;
}

// A kept row of a sparse each, now `element` of the list at `list` where it was `was`: what
// read the model through it reads the new element.
void renumber(Build& b, const u32* tops, u32 ntops, const char* list, u32 was, u32 element) {
    if (!b.deps || was == element) return;
    KromeTree& t = b.tree;
    Array<u32> sub{};
    for (u32 k = 0; k < ntops; ++k) sub.push(tops[k]);
    for (u32 i = 0; i < sub.len; ++i)
        for (u32 c = t.nodes[sub[i]].first; c != KROME_NONE; c = t.nodes[c].next) sub.push(c);
    char from[300], to[300];
    i32 fl = format(from, sizeof from, "%s.%u", list, was);
    format(to, sizeof to, "%s.%u", list, element);
    for (KromeDep& d : *b.deps) {
        if (d.path.is_null()) continue;
        const char* p = name_str(d.path);
        if (strncmp(p, from, (usize)fl) != 0 || (p[fl] != '.' && p[fl] != 0)) continue;
        bool mine = false;
        for (u32 id : sub) if (id == d.node) { mine = true; break; }
        if (!mine) continue;
        char np[400];
        format(np, sizeof np, "%s%s", to, p + fl);
        d.path = name_intern(np);
        if (d.kind == KromeDep::Text) krome_fill_node(b.s, b.sc, t, d.node); // what it shows, from where it reads now
    }
    sub.free();
}

u32 region_body(Build& b, const KimbFile& f, const JsonValue& n, u32 parent, const KromeFrame* fr, KromeRegion* virt = nullptr) {
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
        u32 from = 0, to = len;
        // A sparse each: the list is a run of rows, the count and its first row the model's.
        u32 held_from = 0, held = len;
        const JsonValue* placeholder = nullptr;
        if (virt && virt->sparse) {
            f64 v = 0.0;
            u32 count = virt->count_path ? (krome_model_number(b.s, b.sc, virt->count_path, &v) && v > 0.0 ? (u32)v : 0) : virt->count_n;
            v = 0.0;
            held_from = virt->from_path ? (krome_model_number(b.s, b.sc, virt->from_path, &v) && v > 0.0 ? (u32)v : 0) : virt->from_n;
            virt->rows_from = held_from;
            virt->rows_len  = held;
            len = count;
            if (kids)
                for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k))
                    if (!strcmp(json_string(json_get(f.doc, *k, "w"), ""), "placeholder")) placeholder = k;
        }
        if (virt) {
            // Only the window, held inside the list as it is now.
            if (virt->first + virt->want > len) virt->first = len > virt->want ? len - virt->want : 0;
            from = virt->first;
            to = from + virt->want < len ? from + virt->want : len;
            virt->len = len;
            spacer(b, parent, from, virt->height);
        }
        bool sparse = virt && virt->sparse;
        // A row with an each or an if in it is always built: its regions' frames name its element.
        bool keepable = sparse && b.reuse && !(kids && has_region(f.doc, *kids));
        for (u32 i = from; i < to; ++i) {
            u32 row_before = t.nodes[parent].last;
            if (sparse) {
                bool is_held = i >= held_from && i - held_from < held;
                u32 element = is_held ? i - held_from + 1 : 0;
                char ep[300];
                format(ep, sizeof ep, "%s.%u", path, element);
                u64 hash = is_held ? krome_model_hash(b.s, b.sc, ep) : 0;
                bool kept = false;
                if (keepable)
                    for (KromeSparseRow& o : *b.reuse) {
                        if (o.row != i || !o.nodes || o.held != is_held || o.hash != hash || o.nodes > 64) continue;
                        u32 tops[64];
                        u32 c = o.first;
                        for (u32 k = 0; k < o.nodes && c != KROME_NONE; ++k) { tops[k] = c; c = t.nodes[c].next; }
                        for (u32 k = 0; k < o.nodes; ++k) { append_node(t, parent, tops[k]); b.reused->push(tops[k]); }
                        if (is_held) renumber(b, tops, o.nodes, path, o.element, element);
                        b.sc.sparse_rows.push(KromeSparseRow{virt->anchor, i, hash, element, tops[0], o.nodes, is_held});
                        o.nodes = 0; // taken
                        kept = true;
                        break;
                    }
                if (kept) continue;
                // Built below; its record once it is.
                b.sc.sparse_rows.push(KromeSparseRow{virt->anchor, i, hash, element, KROME_NONE, 0, is_held});
            }
            KromeFrame* e = (KromeFrame*)arena_alloc(*b.frames, sizeof(KromeFrame), alignof(KromeFrame));
            if (!e) break;
            *e = KromeFrame{};
            e->outer = fr;
            e->var = var;
            bool have = !virt || !virt->sparse || (i >= held_from && i - held_from < held);
            u32 element = virt && virt->sparse ? i - held_from + 1 : i + 1;
            char p[300];
            // A row the model does not hold reads element 0, which no list has: its
            // placeholder's texts that name the row's fields come out empty.
            i32 pn = format(p, sizeof p, "%s.%u", path, have ? element : 0u);
            e->path = keep(b, p, (usize)pn);
            if (!have) {
                if (placeholder) if (const JsonValue* pk = json_get(f.doc, *placeholder, "c"))
                    for (const JsonValue* k = json_first(f.doc, *pk); k; k = json_next(f.doc, *k)) node(b, f, *k, parent, e, nullptr);
            } else if (kids) {
                for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k))
                    if (k != placeholder) node(b, f, *k, parent, e, nullptr);
            }
            if (sparse) {
                // The row's top-level nodes: those after what the parent ended with before it.
                KromeSparseRow& rec = b.sc.sparse_rows.last();
                u32 c = row_before == KROME_NONE ? t.nodes[parent].first : t.nodes[row_before].next;
                rec.first = c;
                for (; c != KROME_NONE; c = t.nodes[c].next) ++rec.nodes;
            }
        }
        if (virt) spacer(b, parent, len - to, virt->height);
    } else {
        const char* on = json_string(json_get(f.doc, n, "on"), nullptr);
        if (!on) { error(b, fr, f.file, n.line, "if is written 'if name { ... }'"); return 0; }
        const char* ov = json_string(json_get(f.doc, n, "ov"), nullptr);
        bool yes;
        if (ov) {
            char val[256];
            yes = !strcmp(krome_model_path_text(b.s, b.sc, model_path(b, fr, on), val, sizeof val), ov);
        } else yes = krome_model_truthy(b.s, b.sc, model_path(b, fr, on));
        if (yes)
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
    const char* w = json_string(json_get(f.doc, n, "w"), "");
    const char* expr = json_string(json_get(f.doc, n, !strcmp(w, "each") ? "in" : "on"), "");
    KromeRegion r{head_key(model_path(b, fr, expr)), anchor, 0, &f, &n, fr, true};
    r.scope = b.scope;
    // A virtualized each (only where it can be built again, so not in an overlay): at first
    // what three windows' heights of rows fill; the view's first layout corrects it.
    f32 height = (f32)json_number(json_get(f.doc, n, "height"), 0.0);
    if (height > 0.0f && b.regions) {
        r.height = height;
        // Sparse (KR22): the count and the first row, each a number or @path.
        const JsonValue* cv = json_get(f.doc, n, "count");
        const JsonValue* fv = json_get(f.doc, n, "from");
        if (cv || fv) {
            r.sparse = true;
            auto take = [&](const JsonValue* v, const char** path, u32* num, u64* key) {
                if (!v) return;
                if (v->str() && v->str()[0] == '@') { *path = model_path(b, fr, v->str() + 1); *key = head_key(*path); }
                else *num = (u32)json_number(v, 0.0);
            };
            take(cv, &r.count_path, &r.count_n, &r.count_key);
            take(fv, &r.from_path, &r.from_n, &r.from_key);
            if (!cv) error(b, fr, f.file, n.line, "a sparse each (from=) needs count=");
            const char* ev = json_string(json_get(f.doc, n, "e"), nullptr);
            if (ev) r.view = name_intern(ev);
        }
        f32 view = b.s.win_h > 0.0f ? b.s.win_h / b.s.scale : 1000.0f;
        r.want = (u32)(view * 3.0f / height) + 8;
        // Built whole again: where it was, so nothing is built twice to get back there.
        if (b.windows)
            for (Build::Window& win : *b.windows)
                if (!win.taken && win.src == &n) { r.first = win.first; r.want = win.want; win.taken = true; break; }
    }
    r.count = region_body(b, f, n, parent, fr, r.height > 0.0f ? &r : nullptr);
    if (!b.regions) return;
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
    // Children with no slot to go in are a behaviour's to open (w:open_slot: a dropdown's options).
    const JsonValue* bp = json_get(dd, *body, "p");
    if (json_get(f.doc, n, "c") && !has_slot(dd, *body) && !(bp && json_get(dd, *bp, "behave")))
        error(b, fr, f.file, n.line, "%s takes no children: its body has no slot", w);
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
    if (!krome_file_scope(b.s, *d->file, *scope, &failed)) { if (failed) error(b, fr, f.file, n.line, "kim(\"%s\"): a prelude it uses would not load", file); else b.waiting = true; return; }
    const JsonValue* root = d->file->screens ? json_get(d->file->doc, *d->file->screens, screen) : nullptr;
    if (!root) { error(b, fr, f.file, n.line, "kim(\"%s\"): no screen '%s'", file, screen); return; }
    const KimbScope* was = b.scope;
    b.scope = scope;
    node(b, *d->file, *root, parent, nullptr, nullptr);
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
        error(b, fr, f.file, line, "'%s' is neither a kernel node (box, text, image, hit, edit) nor a definition", w);
        return;
    }
    u32 id = t.add(type, parent);
    if (id == KROME_NONE) { error(b, fr, f.file, line, "nesting deeper than %u", KROME_MAX_DEPTH); return; }
    ++b.nodes;
    t.nodes[id].line = line;
    styles(b, fr, id, f, line, json_get(f.doc, n, "s"));
    flags(b, fr, id, f, line, json_get(f.doc, n, "f"));
    // An image's string is its path (`image "skin/buttons.png" region=...`): what image= says, under it.
    const JsonValue* tv = json_get(f.doc, n, "t");
    if (type == KromeType::Image && tv && tv->str()) {
        t.set(id, "image", KromeValue::str(expand(b, fr, tv->str())));
        tv = nullptr;
    }
    props(b, fr, id, f, line, json_get(f.doc, n, "p"), KromeState::Base);
    looks(b, fr, id, f, line, json_get(f.doc, n, "looks"));
    if (tv) {
        if (const char* raw = tv->str()) {
            const char* text = expand(b, fr, raw);
            t.set_text(id, text);
            if (b.deps) deps_of(*b.deps, id, text);
            if (strchr(text, '{')) krome_fill_node(b.s, b.sc, t, id); // shown now: an escaped brace reads no key
        }
    }
    if (json_get(f.doc, n, "a")) error(b, fr, f.file, line, "a %s takes one content string", w);
    const JsonValue* range = json_get(f.doc, n, "r");
    const JsonDoc* range_doc = &f.doc;
    if (const char* i = json_string(json_get(f.doc, n, "id"), nullptr)) t.set(id, "id", KromeValue::str(expand(b, fr, i)));
    if (const char* e = json_string(json_get(f.doc, n, "e"), nullptr)) t.set(id, "event", KromeValue::str(e));
    binding(b, fr, id, f, json_get(f.doc, n, "b"), json_get(f.doc, n, "bv"));
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
        binding(b, o->frame, id, *o->file, json_get(ud, *o->use, "b"), json_get(ud, *o->use, "bv"));
        if (const JsonValue* r = json_get(ud, *o->use, "r")) { range = r; range_doc = &ud; }
    }
    // A range is a behaviour's (`slider @v 1..99`): kept on the node for it.
    if (range) {
        const JsonValue* lo = json_first(*range_doc, *range);
        const JsonValue* hi = lo ? json_next(*range_doc, *lo) : nullptr;
        if (t.nodes[id].behave.is_null()) error(b, fr, f.file, line, "a %s takes no range: only one with a behaviour does", w);
        else if (lo && hi && lo->is_number() && hi->is_number()) { t.nodes[id].lo = (f32)lo->number(); t.nodes[id].hi = (f32)hi->number(); }
    }
    if (!t.nodes[id].behave.is_null() && !krome_behaviour_known(b.s, t.nodes[id].behave))
        error(b, fr, f.file, line, "no behaviour '%s': a prelude's script returns it, or a module registers it", name_str(t.nodes[id].behave));
    if (const JsonValue* c = json_get(f.doc, n, "c"))
        for (const JsonValue* k = json_first(f.doc, *c); k; k = json_next(f.doc, *k)) node(b, f, *k, id, fr, nullptr);
    if (t.nodes[id].behave.is_null()) return;
    // A behaviour's node: the children its use was given, kept for w:open_slot (a
    // dropdown's options); its update now that its parts are built, and whenever its bound
    // key changes.
    if (b.slots)
        for (const Over* o = over; o; o = o->next)
            if (json_get(o->file->doc, *o->use, "c")) { b.slots->push(KromeSlot{id, o->file, o->use, o->frame}); break; }
    if (b.deps && !t.nodes[id].binding.is_null()) b.deps->push(KromeDep{id, head_key(name_str(t.nodes[id].binding)), KromeDep::Behave});
    krome_behave(b.s, KromeTarget{b.sc.id, b.overlay, id}, KromeCall::Update, KromeCallArgs{});
}

// ---- rebuilding ---------------------------------------------------------------------------------

// What a rebuild keeps: where each scroll box was scrolled to, by its id, or for one with
// none, by its place among the scroll boxes in document order.
struct Kept { Name id; u32 nth; vec2 scroll; u32 node; u32 now; }; // node: its index before; now: after (KROME_NONE: gone)

bool scrolls(const KromeNode& n) { return (n.flags & (KROME_SCROLL | KROME_SCROLL_X | KROME_SCROLL_Y)) != 0; }

void keep_walk(const KromeTree& t, u32 i, u32* nth, Array<Kept>& out) {
    const KromeNode& n = t.nodes[i];
    if (n.flags & KROME_DEAD) return;
    if (scrolls(n)) out.push(Kept{n.id, (*nth)++, n.scroll, i, KROME_NONE});
    for (u32 c = n.first; c != KROME_NONE; c = t.nodes[c].next) keep_walk(t, c, nth, out);
}
void keep_scroll(const KromeTree& t, Array<Kept>& out) {
    u32 nth = 0;
    if (t.root != KROME_NONE) keep_walk(t, t.root, &nth, out);
}
void restore_walk(KromeTree& t, u32 i, u32* nth, Array<Kept>& kept) {
    KromeNode& n = t.nodes[i];
    if (scrolls(n)) {
        u32 at = (*nth)++;
        for (Kept& k : kept)
            if (!n.id.is_null() ? k.id == n.id : (k.id.is_null() && k.nth == at)) { n.scroll = k.scroll; k.now = i; break; }
    }
    for (u32 c = n.first; c != KROME_NONE; c = t.nodes[c].next) restore_walk(t, c, nth, kept);
}
// The virtualized eaches' windows, in document order (by their anchors in the tree).
void keep_windows_walk(const KromeScreen& sc, u32 i, Array<Build::Window>& out) {
    const KromeNode& n = sc.tree.nodes[i];
    if (n.flags & KROME_DEAD) return;
    if (n.flags & KROME_HIDDEN)
        for (const KromeRegion& r : sc.regions)
            if (r.alive && r.anchor == i && r.height > 0.0f) out.push(Build::Window{r.src, r.first, r.want, false});
    for (u32 c = n.first; c != KROME_NONE; c = sc.tree.nodes[c].next) keep_windows_walk(sc, c, out);
}
void keep_windows(const KromeScreen& sc, Array<Build::Window>& out) {
    if (sc.tree.root != KROME_NONE) keep_windows_walk(sc, sc.tree.root, out);
}

void restore(KromeTree& t, Array<Kept>& kept) {
    u32 nth = 0;
    if (t.root != KROME_NONE) restore_walk(t, t.root, &nth, kept);
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
    const KimbFile* f = sc.doc ? (sc.file ? sc.file : sc.doc->file) : sc.has_lua_file ? &sc.lua_file : nullptr;
    if (!f) return false;
    const char* screen = sc.doc ? name_str(sc.screen) : nullptr;
    const JsonValue* root = root_of(*f, screen);
    krome_drop_anims(s, sc.id, 0); // its nodes are new: none eases from what was there
    // What was opened from its uses' children was built from what is about to go.
    for (u32 i = sc.overlays.len; i > 0; --i) if (sc.overlays[i - 1]->slot) krome_close_overlay(s, sc, sc.overlays[i - 1]->id);
    sc.slots.alloc = s.general;
    sc.slots.clear();
    if (!root) { krome_error(s, f->file, 0, nullptr, 0, "no screen '%s'", screen ? screen : "?"); return false; }
    Array<Kept> kept{};
    keep_scroll(sc.tree, kept);
    Array<Build::Window> windows{};
    keep_windows(sc, windows);
    sc.tree.clear();
    sc.deps.clear();
    sc.regions.alloc = s.general;
    sc.regions.clear();
    sc.sparse_rows.alloc = s.general;
    sc.sparse_rows.clear();
    sc.dead = 0;
    if (!sc.frames.base) arena_create(sc.frames, 256u << 20, "krome.frames", MemTag{});
    else arena_reset(sc.frames);
    // What the pointer held of the old tree, by index, is gone with it.
    if (s.under.screen == sc.id && !s.under.overlay) s.under = KromeTarget{};
    if (s.captured.screen == sc.id && !s.captured.overlay) s.captured = KromeTarget{};
    if (s.hover_behave.screen == sc.id && !s.hover_behave.overlay) s.hover_behave = KromeTarget{};
    // A scrollbar being dragged goes on, on the same box in the new tree (below).
    bool held = s.bar.active && s.bar.box.screen == sc.id && !s.bar.box.overlay;
    u32 held_node = held ? s.bar.box.node : KROME_NONE;
    if (s.bar.box.screen == sc.id && !s.bar.box.overlay) s.bar.active = false;
    if (s.tip.screen == sc.id && !s.tip.overlay) s.tip = KromeTarget{};
    Build b{s, sc, sc.tree, &sc.deps, &sc.regions, &sc.frames, &sc.scope, krome_theme_of(s, sc.scope)};
    b.slots = &sc.slots;
    b.windows = &windows;
    node(b, *f, *root, KROME_NONE, nullptr, nullptr);
    windows.free();
    sc.waiting = b.waiting;
    restore(sc.tree, kept);
    for (const Kept& k : kept)
        if (held && k.node == held_node && k.now != KROME_NONE) { s.bar.box.node = k.now; s.bar.active = true; }
    kept.free();
    for (const KromeDep& d : sc.deps) if (d.kind == KromeDep::Text) krome_fill_node(s, sc, sc.tree, d.node);
    sc.tree.dirty = true;
    sc.repaint = true;
    for (KromeOverlay* o : sc.overlays) krome_build_overlay(s, sc, *o);
    krome_edit_rebuilt(s, sc);
    return sc.tree.root != KROME_NONE;
}

bool krome_build_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o) {
    const JsonValue* root = root_of(o.file, nullptr);
    if (!root) return false;
    o.tree.clear();
    Build b{s, sc, o.tree, nullptr, nullptr, &o.arena, &sc.scope, krome_theme_of(s, sc.scope)};
    b.overlay = o.id;
    node(b, o.file, *root, KROME_NONE, nullptr, nullptr);
    o.placed = false;
    return o.tree.root != KROME_NONE;
}

bool krome_strip_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o, const char* text) {
    if (o.arena.base) arena_destroy(o.arena);
    if (!arena_create(o.arena, 1u << 20, "krome.strip", MemTag{})) return false;
    JsonBuilder jb(&o.arena.a);
    JsonRef root = jb.object();
    jb.set(root, "kim", jb.integer(1));
    jb.set(root, "file", jb.string("krome:reload"));
    JsonRef box = jb.object();
    jb.set(box, "w", jb.string("box"));
    JsonRef p = jb.object();
    jb.set(p, "bg", jb.string("#8f1d1d"));
    jb.set(p, "color", jb.string("#ffffff"));
    jb.set(p, "size", jb.integer(13));
    JsonRef pad = jb.array();
    jb.push(pad, jb.integer(10));
    jb.push(pad, jb.integer(6));
    jb.set(p, "pad", pad);
    jb.set(box, "p", p);
    JsonRef kids = jb.array();
    JsonRef t = jb.object();
    jb.set(t, "w", jb.string("text"));
    jb.set(t, "t", jb.string(text));
    jb.push(kids, t);
    jb.set(box, "c", kids);
    JsonRef screens = jb.object();
    jb.set(screens, "root", box);
    jb.set(root, "screens", screens);
    JsonDoc doc;
    bool ok = jb.finish(root, doc);
    jb.free();
    KimErrors quiet;
    if (!ok || !kimb_from_doc(doc, o.file, quiet, "krome:reload")) return false;
    return krome_build_overlay(s, sc, o);
}

bool krome_tooltip_overlay(KromeModule& s, KromeScreen& sc, KromeOverlay& o, const char* text) {
    if (o.arena.base) arena_destroy(o.arena);
    if (!arena_create(o.arena, 1u << 20, "krome.tooltip", MemTag{})) return false;
    JsonBuilder jb(&o.arena.a);
    JsonRef root = jb.object();
    jb.set(root, "kim", jb.integer(1));
    jb.set(root, "file", jb.string(name_str(sc.name)));
    JsonRef box = jb.object();
    jb.set(box, "w", jb.string("box"));
    KimbFound st;
    if (kimb_find_style(sc.scope, "tooltip", &st)) {
        JsonRef styles = jb.array();
        jb.push(styles, jb.string("tooltip"));
        jb.set(box, "s", styles);
    } else {
        JsonRef p = jb.object();
        jb.set(p, "bg", jb.string("#1f2533"));
        jb.set(p, "color", jb.string("#e6e9ef"));
        JsonRef pad = jb.array();
        jb.push(pad, jb.integer(8));
        jb.push(pad, jb.integer(4));
        jb.set(p, "pad", pad);
        jb.set(p, "radius", jb.integer(4));
        jb.set(box, "p", p);
    }
    JsonRef kids = jb.array();
    JsonRef t = jb.object();
    jb.set(t, "w", jb.string("text"));
    jb.set(t, "t", jb.string(text));
    jb.push(kids, t);
    jb.set(box, "c", kids);
    JsonRef screens = jb.object();
    jb.set(screens, "root", box);
    jb.set(root, "screens", screens);
    JsonDoc doc;
    bool ok = jb.finish(root, doc);
    jb.free();
    KimErrors quiet;
    if (!ok || !kimb_from_doc(doc, o.file, quiet, name_str(sc.name))) return false;
    return krome_build_overlay(s, sc, o);
}

namespace {

// One region built again in place. False when it is gone (its anchor rebuilt away).
bool rebuild_one(KromeModule& s, KromeScreen& sc, u32 i) {
    KromeTree& t = sc.tree;
    {
        KromeRegion r = sc.regions[i];
        if (t.nodes[r.anchor].flags & KROME_DEAD) { sc.regions[i].alive = false; return false; }
        u32 parent = t.nodes[r.anchor].parent;
        // What it built goes: out of the list, and dead. A sparse each's are only taken out
        // until it is built again: the rows it keeps are moved back in, and the rest die then.
        Array<u32> was{};
        Array<KromeSparseRow> old_rows{};
        Array<u32> reused{};
        u32 c = t.nodes[r.anchor].next;
        for (u32 k = 0; k < r.count && c != KROME_NONE; ++k) {
            u32 next = t.nodes[c].next;
            if (r.sparse) was.push(c);
            else sc.dead += kill(t, c);
            c = next;
        }
        t.nodes[r.anchor].next = c;
        if (c == KROME_NONE) t.nodes[parent].last = r.anchor;
        if (r.sparse) {
            u32 w = 0;
            for (u32 k = 0; k < sc.sparse_rows.len; ++k) {
                if (sc.sparse_rows[k].anchor == r.anchor) old_rows.push(sc.sparse_rows[k]);
                else sc.sparse_rows[w++] = sc.sparse_rows[k];
            }
            sc.sparse_rows.len = w;
        }
        // Built again at the end of the parent's list, then moved after the anchor.
        u32 tail = t.nodes[parent].last;
        Build b{s, sc, t, &sc.deps, &sc.regions, &sc.frames, r.scope, krome_theme_of(s, sc.scope)};
        b.slots = &sc.slots;
        if (r.sparse) { b.reuse = &old_rows; b.reused = &reused; }
        u32 deps_before = sc.deps.len;
        u32 count = region_body(b, *r.file, *r.src, parent, r.frame, r.height > 0.0f ? &r : nullptr);
        s.rebuilt_nodes += b.nodes;
        for (u32 id : was) {
            bool kept = false;
            for (u32 k : reused) if (k == id) { kept = true; break; }
            if (!kept) sc.dead += kill(t, id);
        }
        was.free();
        old_rows.free();
        reused.free();
        if (count && tail != r.anchor) {
            u32 first = t.nodes[tail].next, last = t.nodes[parent].last;
            t.nodes[tail].next = KROME_NONE;
            t.nodes[parent].last = tail;
            t.nodes[last].next = t.nodes[r.anchor].next;
            t.nodes[r.anchor].next = first;
            if (t.nodes[last].next == KROME_NONE) t.nodes[parent].last = last;
        }
        r.count = count;
        sc.regions[i] = r;
        for (u32 d = deps_before; d < sc.deps.len; ++d) if (sc.deps[d].kind == KromeDep::Text) krome_fill_node(s, sc, t, sc.deps[d].node);
        ++s.rebuilt_regions;
    }
    return true;
}

// After regions were built again: what went inside them dropped, and the tree to lay out.
void rebuilt(KromeModule&, KromeScreen& sc) {
    KromeTree& t = sc.tree;
    // Regions inside what went, and what read the model there.
    for (KromeRegion& r : sc.regions) if (r.alive && (t.nodes[r.anchor].flags & KROME_DEAD)) r.alive = false;
    {
        u32 w = 0;
        for (u32 i = 0; i < sc.sparse_rows.len; ++i) if (!(t.nodes[sc.sparse_rows[i].anchor].flags & KROME_DEAD)) sc.sparse_rows[w++] = sc.sparse_rows[i];
        sc.sparse_rows.len = w;
    }
    {
        u32 w = 0;
        for (u32 i = 0; i < sc.slots.len; ++i) if (!(t.nodes[sc.slots[i].node].flags & KROME_DEAD)) sc.slots[w++] = sc.slots[i];
        sc.slots.len = w;
    }
    u32 w = 0;
    for (u32 i = 0; i < sc.deps.len; ++i) if (!(t.nodes[sc.deps[i].node].flags & KROME_DEAD)) sc.deps[w++] = sc.deps[i];
    sc.deps.len = w;
    u32 rw = 0;
    for (u32 i = 0; i < sc.regions.len; ++i) if (sc.regions[i].alive) sc.regions[rw++] = sc.regions[i];
    sc.regions.len = rw;
    t.dirty = true;
    sc.repaint = true;
    // Past as many dead nodes as live ones, the tree is built whole again, which drops them:
    // in the next frame's pump, never here, where a handler or the pointer may be in the
    // middle of using the tree's nodes by index.
    if (sc.dead > 1024 && sc.dead > t.nodes.len - sc.dead) sc.rebuild = true;
}

} // namespace

bool krome_build_slot(KromeModule& s, KromeScreen& sc, KromeOverlay& o, const KromeSlot& slot, const char* style_name) {
    o.tree.clear();
    if (o.arena.base) arena_destroy(o.arena);
    if (!arena_create(o.arena, 1u << 20, "krome.slot", MemTag{})) return false;
    Build b{s, sc, o.tree, nullptr, nullptr, &o.arena, &sc.scope, krome_theme_of(s, sc.scope)};
    b.overlay = o.id;
    u32 root = o.tree.add(KromeType::Box, KROME_NONE);
    if (root == KROME_NONE) return false;
    if (style_name && *style_name) style(b, nullptr, root, style_name, slot.file->file, slot.use->line, 0);
    if (const JsonValue* c = json_get(slot.file->doc, *slot.use, "c"))
        for (const JsonValue* k = json_first(slot.file->doc, *c); k; k = json_next(slot.file->doc, *k)) node(b, *slot.file, *k, root, slot.frame, nullptr);
    o.slot = true;
    o.placed = false;
    return true;
}

void krome_rebuild_regions(KromeModule& s, KromeScreen& sc, u64 key) {
    bool any = false;
    u32 n = sc.regions.len;
    for (u32 i = 0; i < n; ++i) {
        KromeRegion& r = sc.regions[i];
        if (!r.alive) continue;
        // A sparse each is built again once a frame, before layout (krome_sparse_pending):
        // its rows are handed over in more than one write (the list, `from`), and between
        // them the two disagree, which would build every row twice and keep none.
        if (r.sparse) {
            if (r.key == key || r.count_key == key || r.from_key == key) { r.stale = true; sc.tree.dirty = true; }
            continue;
        }
        if (r.key == key && rebuild_one(s, sc, i)) any = true;
    }
    if (any) rebuilt(s, sc);
}

void krome_sparse_pending(KromeModule& s, KromeScreen& sc) {
    bool any = false;
    u32 n = sc.regions.len;
    for (u32 i = 0; i < n; ++i) {
        if (!sc.regions[i].alive || !sc.regions[i].stale) continue;
        sc.regions[i].stale = false;
        if (rebuild_one(s, sc, i)) any = true;
    }
    if (any) rebuilt(s, sc);
}

bool krome_virtual_regions(KromeModule& s, KromeScreen& sc) {
    KromeTree& t = sc.tree;
    bool any = false;
    u32 n = sc.regions.len;
    for (u32 i = 0; i < n; ++i) {
        KromeRegion& r = sc.regions[i];
        if (!r.alive || r.height <= 0.0f || !r.count || (t.nodes[r.anchor].flags & KROME_DEAD)) continue;
        u32 parent = t.nodes[r.anchor].parent;
        u32 box = parent;
        while (box != KROME_NONE && !krome_scrolls_y(t.nodes[box])) box = t.nodes[box].parent;
        if (box == KROME_NONE) continue; // in no scroll box: what was built stays
        const KromeNode& bx = t.nodes[box];
        f32 view = bx.rect.h - bx.pad[1] - bx.pad[3];
        if (view <= 0.0f) continue;
        // The region's top in the box's content: its first node, the spacer or the first row.
        u32 head = t.nodes[r.anchor].next;
        if (head == KROME_NONE) continue;
        f32 top = t.nodes[head].rect.y - (bx.rect.y + bx.pad[1]) + bx.scroll.y;
        f32 pitch = r.height + t.nodes[parent].gap;
        f32 a = (bx.scroll.y - top) / pitch, z = (bx.scroll.y + view - top) / pitch;
        i64 vf = (i64)floor(a), vl = (i64)ceil(z);
        if (vf < 0) vf = 0;
        if (vl > (i64)r.len) vl = (i64)r.len;
        if (vl < vf) vl = vf;
        u32 built_to = r.first + r.want < r.len ? r.first + r.want : r.len;
        u32 page = (u32)ceil(view / pitch) + 1;
        u32 need = (u32)(vl - vf) + 2 * page;
        bool covered = (u32)vf >= r.first && (u32)vl <= built_to;
        if (covered && r.want >= need - page && r.want <= 3 * need) continue;
        // A screenful each side of what is in view.
        r.first = (u32)vf > page ? (u32)vf - page : 0;
        r.want = need;
        if (rebuild_one(s, sc, i)) any = true;
    }
    if (any) rebuilt(s, sc);
    return any;
}

void krome_send_views(KromeModule& s, KromeScreen& sc) {
    // Gathered first: a handler that puts rows in the model rebuilds regions as it does.
    struct View { Name event; u32 first, count, total; };
    View views[16];
    u32 n = 0;
    for (KromeRegion& r : sc.regions) {
        if (!r.alive || !r.sparse || (sc.tree.nodes[r.anchor].flags & KROME_DEAD)) continue;
        u32 to = r.first + r.want < r.len ? r.first + r.want : r.len;
        u32 count = to > r.first ? to - r.first : 0;
        if (r.first == r.sent_first && count == r.sent_count && r.len == r.sent_len) continue;
        r.sent_first = r.first;
        r.sent_count = count;
        r.sent_len   = r.len;
        if (n < 16) views[n++] = View{r.view, r.first, count, r.len};
    }
    for (u32 i = 0; i < n; ++i) krome_fire_view(s, sc, views[i].event, views[i].first, views[i].count, views[i].total);
}

} // namespace kv
