// The .kimb document (kimb.h): reading it, its scope, the kernel's words, and the check.
#include "korvid/kimb/kimb.h"
#include "korvid/core/format.h"
#include "korvid/core/kbor.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

KimError* KimErrors::add(const char* file, u32 line, u32 col, const char* fmt, ...) {
    ++total;
    if (count == KV_KIMB_MAX_ERRORS) return nullptr;
    KimError& e = list[count++];
    e = KimError{};
    e.file = file;
    e.line = line;
    e.col = col;
    va_list args;
    va_start(args, fmt);
    vformat(e.text, sizeof e.text, fmt, args);
    va_end(args);
    return &e;
}

i32 KimErrors::format_one(u32 i, char* out, usize cap) const {
    if (i >= count) return format(out, cap, "%s", "");
    const KimError& e = list[i];
    char where[64] = {};
    if (e.col) format(where, sizeof where, "%u:%u", e.line, e.col);
    else format(where, sizeof where, "%u", e.line);
    if (e.used_file)
        return format(out, cap, "%s:%s: %s (used at %s:%u)", e.file ? e.file : "?", where, e.text, e.used_file, e.used_line);
    return format(out, cap, "%s:%s: %s", e.file ? e.file : "?", where, e.text);
}

// ---- reading -----------------------------------------------------------------------------

namespace {

struct Liner {
    JsonDoc&         doc;
    const JsonValue* lines;
    const JsonValue* at = nullptr; // the next line in the sidecar
    void take(const JsonValue& v) {
        if (!at) return;
        const_cast<JsonValue&>(v).line = (u32)json_u64(at, 0);
        at = json_next(doc, *at);
    }
    void node(const JsonValue& n) {
        take(n);
        if (const JsonValue* c = json_get(doc, n, "c"))
            for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k)) node(*k);
    }
    void each(const JsonValue* section) {
        if (section) for (const JsonValue* v = json_first(doc, *section); v; v = json_next(doc, *v)) take(*v);
    }
};

const JsonValue* section(const JsonDoc& doc, const char* name, JsonType t) {
    const JsonValue* v = json_get(doc, doc.root(), name);
    return v && v->type == t ? v : nullptr;
}

} // namespace

bool kimb_from_doc(JsonDoc& doc, KimbFile& f, KimErrors& errors, const char* name) {
    f = KimbFile{};
    f.doc = doc;
    const JsonValue& root = doc.root();
    if (!root.is_object()) { errors.add(name, 0, 0, "not a .kimb: the document is not a map"); return false; }
    u64 version = json_u64(json_get(doc, root, "kim"), 0);
    if (version != 1) { errors.add(name, 0, 0, "a .kimb of version %llu; this reads version 1", (unsigned long long)version); return false; }
    f.file    = json_string(json_get(doc, root, "file"), name ? name : "");
    f.uses    = section(doc, "uses", JsonType::Array);
    f.scripts = section(doc, "scripts", JsonType::Array);
    f.props   = section(doc, "props", JsonType::Array);
    f.screens = section(doc, "screens", JsonType::Object);
    f.defs    = section(doc, "defs", JsonType::Object);
    f.styles  = section(doc, "styles", JsonType::Object);
    f.themes  = section(doc, "themes", JsonType::Object);
    f.fonts   = section(doc, "fonts", JsonType::Object);
    const JsonValue* lines = section(doc, "lines", JsonType::Array);
    f.lines = lines != nullptr;
    if (!lines) return true;
    // The sidecar's walk (kim.h): uses, scripts, props; each screen (its line, then its
    // nodes); each definition (its line, then its body); each style (its line, then its
    // looks); each theme (its line, then its tokens).
    Liner l{doc, lines, json_first(doc, *lines)};
    l.each(f.uses);
    l.each(f.scripts);
    l.each(f.props);
    if (f.screens)
        for (const JsonValue* s = json_first(doc, *f.screens); s; s = json_next(doc, *s)) {
            if (l.at) l.at = json_next(doc, *l.at); // the screen's own line: its root has the one that matters
            l.node(*s);
        }
    if (f.defs)
        for (const JsonValue* d = json_first(doc, *f.defs); d; d = json_next(doc, *d)) {
            l.take(*d);
            if (const JsonValue* body = json_get(doc, *d, "body")) l.node(*body);
        }
    if (f.styles)
        for (const JsonValue* s = json_first(doc, *f.styles); s; s = json_next(doc, *s)) {
            l.take(*s);
            l.each(json_get(doc, *s, "looks"));
        }
    if (f.themes)
        for (const JsonValue* t = json_first(doc, *f.themes); t; t = json_next(doc, *t)) {
            l.take(*t);
            l.each(t);
        }
    l.each(f.fonts);
    return true;
}

bool kimb_read(const void* data, usize size, Allocator* alloc, KimbFile& f, KimErrors& errors, const char* name) {
    JsonDoc doc;
    if (!kbor_parse(data, size, alloc, doc)) {
        errors.add(name, 0, 0, "not a .kimb: %s at byte %u", doc.error, doc.error_at);
        f = KimbFile{};
        f.doc = doc;
        return false;
    }
    return kimb_from_doc(doc, f, errors, name);
}

// ---- the scope ---------------------------------------------------------------------------

namespace {

bool scope_add(KimbScope& s, const KimbFile& f, KimbFindUse find, void* user, KimErrors& errors, u32 depth) {
    for (u32 i = 0; i < s.count; ++i)
        if (s.list[i] == &f) return true;
    if (s.count == KV_KIMB_MAX_SCOPE) { errors.add(f.file, 0, 0, "more than %u files in one scope", KV_KIMB_MAX_SCOPE); return false; }
    s.list[s.count++] = &f;
    bool ok = true;
    if (f.uses)
        for (const JsonValue* u = json_first(f.doc, *f.uses); u; u = json_next(f.doc, *u)) {
            const char* name = u->str();
            if (!name) continue;
            const KimbFile* used = find ? find(user, name) : nullptr;
            if (!used) { errors.add(f.file, u->line, 0, "use \"%s\": no such prelude", name); ok = false; continue; }
            if (depth > 16) { errors.add(f.file, u->line, 0, "uses nested deeper than 16"); return false; }
            ok = scope_add(s, *used, find, user, errors, depth + 1) && ok;
        }
    return ok;
}

bool find_in(const KimbScope& s, const JsonValue* KimbFile::*section, const char* name, KimbFound* out) {
    for (u32 i = 0; i < s.count; ++i) {
        const KimbFile& f = *s.list[i];
        const JsonValue* sec = f.*section;
        if (!sec) continue;
        if (const JsonValue* v = json_get(f.doc, *sec, name)) {
            if (out) { out->file = &f; out->value = v; }
            return true;
        }
    }
    return false;
}

} // namespace

bool kimb_scope(KimbScope& scope, const KimbFile& f, KimbFindUse find, void* user, KimErrors& errors) {
    scope = KimbScope{};
    return scope_add(scope, f, find, user, errors, 0);
}

bool kimb_find_def(const KimbScope& s, const char* name, KimbFound* out) { return find_in(s, &KimbFile::defs, name, out); }
bool kimb_find_style(const KimbScope& s, const char* name, KimbFound* out) { return find_in(s, &KimbFile::styles, name, out); }
bool kimb_find_font(const KimbScope& s, const char* name, KimbFound* out) { return find_in(s, &KimbFile::fonts, name, out); }

bool kimb_find_token(const KimbScope& s, const char* theme, const char* token, KimbFound* out) {
    if (!theme) return false;
    for (u32 i = 0; i < s.count; ++i) {
        const KimbFile& f = *s.list[i];
        if (!f.themes) continue;
        const JsonValue* t = json_get(f.doc, *f.themes, theme);
        if (!t || !t->is_object()) continue;
        if (const JsonValue* v = json_get(f.doc, *t, token)) {
            if (out) { out->file = &f; out->value = v; }
            return true;
        }
    }
    return false;
}

bool kimb_has_theme(const KimbScope& s, const char* theme) {
    for (u32 i = 0; i < s.count; ++i)
        if (s.list[i]->themes && json_get(s.list[i]->doc, *s.list[i]->themes, theme)) return true;
    return false;
}

const char* kimb_first_theme(const KimbScope& s) {
    for (u32 i = 0; i < s.count; ++i)
        if (s.list[i]->themes && s.list[i]->themes->count()) return json_first(s.list[i]->doc, *s.list[i]->themes)->key;
    return nullptr;
}

bool kimb_find_prop(const KimbScope& s, const char* name, KimbFound* out) {
    for (u32 i = 0; i < s.count; ++i) {
        const KimbFile& f = *s.list[i];
        if (!f.props) continue;
        for (const JsonValue* p = json_first(f.doc, *f.props); p; p = json_next(f.doc, *p))
            if (!strcmp(json_string(json_get(f.doc, *p, "name"), ""), name)) {
                if (out) { out->file = &f; out->value = p; }
                return true;
            }
    }
    return false;
}

// ---- the kernel ----------------------------------------------------------------------------

namespace {

constinit const KimbKernelProp k_props[] = {
    {"anchor", KimbType::Anchor, true},
    {"angle", KimbType::Number, false},
    {"b", KimbType::Edge, true},
    {"behave", KimbType::String, true},
    {"bg", KimbType::Color, false},
    {"bg2", KimbType::Color, false},
    {"border", KimbType::Number, false},
    {"border-color", KimbType::Color, false},
    {"blur", KimbType::String, true},
    {"caret-color", KimbType::Color, false},
    {"change", KimbType::String, true},
    {"color", KimbType::Color, false},
    {"context", KimbType::String, true},
    {"cursor", KimbType::String, true},
    {"dir", KimbType::Dir, true},
    {"fit", KimbType::String, false},
    {"fps", KimbType::Number, false},
    {"frame", KimbType::Number, false},
    {"frame-at", KimbType::Number, false},
    {"frames", KimbType::Number, false},
    {"font", KimbType::String, false},
    {"gain", KimbType::Gain, false},
    {"gap", KimbType::Number, true},
    {"grip", KimbType::String, true},
    {"h", KimbType::Size, true},
    {"hue", KimbType::Number, false},
    {"image", KimbType::String, false},
    {"key", KimbType::String, true},
    {"l", KimbType::Edge, true},
    {"line-height", KimbType::Number, false},
    {"loops", KimbType::Number, false},
    {"opacity", KimbType::Number, false},
    {"pad", KimbType::Box4, true},
    {"part", KimbType::String, true},
    {"r", KimbType::Edge, true},
    {"placeholder", KimbType::String, true},
    {"radius", KimbType::Box4, false},
    {"range", KimbType::Size, true},
    {"region", KimbType::Box4, false},
    {"saturation", KimbType::Number, false},
    {"scroll", KimbType::String, true},
    {"scrollbar-color", KimbType::Color, false},
    {"scrollbar-size", KimbType::Number, false},
    {"scrollbar-track", KimbType::Color, false},
    {"selection-color", KimbType::Color, false},
    {"shadow", KimbType::Shadow, false},
    {"shadow-color", KimbType::Color, false},
    {"size", KimbType::Number, false},
    {"slice", KimbType::Box4, false},
    {"step", KimbType::Number, true},
    {"t", KimbType::Edge, true},
    {"taborder", KimbType::Number, true},
    {"tint", KimbType::Color, false},
    {"transition", KimbType::Number, false},
    {"tooltip", KimbType::String, true},
    {"w", KimbType::Size, true},
    {"x", KimbType::Number, true},
    {"y", KimbType::Number, true},
};

const char* const k_nodes[] = {"box", "text", "image", "hit", "edit"};
const char* const k_flags[] = {"fill", "clip", "scroll", "disabled", "selected", "hidden", "left", "center", "right", "top", "middle", "bottom",
                               "subpixel", "nowrap", "multiline", "readonly", "focusable", "nofocus", "secret", "tabs", "pixel", "across"};
const char* const k_states[] = {"hover", "pressed", "focused", "disabled", "selected"};

bool in(const char* w, const char* const* list, u32 n) {
    for (u32 i = 0; i < n; ++i) if (!strcmp(w, list[i])) return true;
    return false;
}

bool hex_color(const char* s) {
    if (!strcmp(s, "none") || !strcmp(s, "transparent")) return true;
    if (s[0] != '#') return false;
    usize n = strlen(s + 1);
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    for (usize i = 1; i <= n; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool number_text(const char* s, const char* end) {
    if (s < end && *s == '-') ++s;
    if (s == end) return false;
    bool digits = false, dot = false;
    for (; s < end; ++s) {
        if (*s >= '0' && *s <= '9') digits = true;
        else if (*s == '.' && !dot) dot = true;
        else return false;
    }
    return digits;
}

bool range_text(const char* s) {
    const char* dots = strstr(s, "..");
    if (!dots) return false;
    const char* hi = dots + 2;
    bool lo_ok = dots == s || number_text(s, dots);
    bool hi_ok = !*hi || number_text(hi, hi + strlen(hi));
    return lo_ok && hi_ok && (dots != s || *hi);
}

bool percent_text(const char* s) {
    usize n = strlen(s);
    return n > 1 && s[n - 1] == '%' && number_text(s, s + n - 1);
}

// "33.3%", "100%-40", "50%+8".
bool edge_text(const char* s) {
    const char* pct = strchr(s, '%');
    if (!pct || !number_text(s, pct)) return false;
    const char* p = pct + 1;
    if (!*p) return true;
    if (*p != '+' && *p != '-') return false;
    return p[1] != '-' && number_text(p + 1, p + strlen(p));
}

bool anchor_word(const char* s) {
    static const char* const ok[] = {"center", "top", "middle", "bottom", "left", "right", "top-left", "top-center", "top-right",
                                     "middle-left", "middle-center", "middle-right", "bottom-left", "bottom-center", "bottom-right"};
    return in(s, ok, sizeof ok / sizeof ok[0]);
}

} // namespace

const KimbKernelProp* kimb_kernel_prop(const char* name) {
    for (const KimbKernelProp& p : k_props) if (!strcmp(p.name, name)) return &p;
    return nullptr;
}
bool kimb_kernel_node(const char* w) { return in(w, k_nodes, 5); }
bool kimb_kernel_flag(const char* w) { return in(w, k_flags, sizeof k_flags / sizeof k_flags[0]); }
bool kimb_state(const char* w) { return in(w, k_states, 5); }

bool kimb_type_of(const char* name, KimbType* out) {
    struct T { const char* n; KimbType t; };
    static const T types[] = {{"number", KimbType::Number}, {"color", KimbType::Color}, {"string", KimbType::String},
                              {"size", KimbType::Size}, {"any", KimbType::Any}};
    for (const T& t : types) if (!strcmp(name, t.n)) { *out = t.t; return true; }
    return false;
}

const char* kimb_type_name(KimbType t) {
    switch (t) {
    case KimbType::Number: return "a number";
    case KimbType::Color: return "a color";
    case KimbType::String: return "a string";
    case KimbType::Size: return "a number, a percentage, hug, fill or a range";
    case KimbType::Box4: return "one, two or four numbers";
    case KimbType::Shadow: return "x, y, blur and an optional spread";
    case KimbType::Dir: return "row, col or stack";
    case KimbType::Anchor: return "top-left ... bottom-right, or center";
    case KimbType::Any: return "a value";
    case KimbType::Gain: return "one number, or three";
    case KimbType::Edge: return "a distance (10), a fraction (33.3%), both (100%-40), or none";
    }
    return "?";
}

bool kimb_literal(KimbType t, const JsonValue& v) {
    const char* s = v.str();
    switch (t) {
    case KimbType::Number: return v.is_number();
    case KimbType::Color: return s && hex_color(s);
    case KimbType::String: return s != nullptr;
    case KimbType::Size: return v.is_number() || (s && (!strcmp(s, "hug") || !strcmp(s, "fill") || range_text(s) || percent_text(s)));
    case KimbType::Box4:
    case KimbType::Shadow: {
        if (t == KimbType::Box4 && v.is_number()) return true;
        if (!v.is_array()) return false;
        u32 n = v.count();
        if (t == KimbType::Box4 ? (n != 1 && n != 2 && n != 4) : (n != 3 && n != 4)) return false;
        return true; // the elements are checked by the caller's document
    }
    case KimbType::Gain: return v.is_number() || (v.is_array() && (v.count() == 1 || v.count() == 3));
    case KimbType::Edge: return v.is_number() || (s && (!strcmp(s, "none") || edge_text(s)));
    case KimbType::Dir: return s && (!strcmp(s, "row") || !strcmp(s, "col") || !strcmp(s, "stack"));
    case KimbType::Anchor: return s && anchor_word(s);
    case KimbType::Any: return true;
    }
    return false;
}

const JsonValue* kimb_resolve_value(const KimbScope& s, const char* theme, KimbType t, const JsonValue& v, const char** why) {
    *why = nullptr;
    // A token first: a theme's word wins over a literal of the same spelling (a font
    // token named like a path), and a word that is no literal must be one.
    if (const char* word = v.str()) {
        KimbFound tok;
        if (theme && kimb_find_token(s, theme, word, &tok)) {
            if (kimb_literal(t, *tok.value)) return tok.value;
            *why = "the token's value is not what the property takes";
            return nullptr;
        }
    }
    if (kimb_literal(t, v)) return &v;
    *why = v.str() ? "neither a literal nor a token of the theme" : "not what the property takes";
    return nullptr;
}

// ---- the check -----------------------------------------------------------------------------

namespace {

struct Checker {
    const KimbScope& s;
    KimErrors&       errors;
    const KimbFile&  f;
    const char*      themes[KV_KIMB_MAX_SCOPE * 4] = {};
    u32              theme_count = 0;

    Checker(const KimbScope& sc, KimErrors& e) : s(sc), errors(e), f(*sc.list[0]) {
        for (u32 i = 0; i < s.count; ++i) {
            const KimbFile& g = *s.list[i];
            if (!g.themes) continue;
            for (const JsonValue* t = json_first(g.doc, *g.themes); t; t = json_next(g.doc, *t)) {
                bool seen = false;
                for (u32 k = 0; k < theme_count; ++k) if (!strcmp(themes[k], t->key)) seen = true;
                if (!seen && theme_count < sizeof themes / sizeof themes[0]) themes[theme_count++] = t->key;
            }
        }
    }

    void err(u32 line, const char* fmt, ...) KV_PRINTF(3, 4) {
        char msg[200];
        va_list args;
        va_start(args, fmt);
        vformat(msg, sizeof msg, fmt, args);
        va_end(args);
        errors.add(f.file, line, 0, "%s", msg);
    }

    // A property's value against its type: a literal, or a token every theme has.
    void value(u32 line, const char* what, const char* name, KimbType t, const JsonValue& v) {
        if (t == KimbType::Any) return;
        const char* word = v.str();
        if (word && word[0] == '@') return; // bound to the model: what it reads is known only when it runs
        if (word && theme_count) {
            bool somewhere = false;
            for (u32 i = 0; i < theme_count; ++i) if (kimb_find_token(s, themes[i], word, nullptr)) somewhere = true;
            if (somewhere) {
                for (u32 i = 0; i < theme_count; ++i) {
                    KimbFound tok;
                    if (!kimb_find_token(s, themes[i], word, &tok)) { err(line, "%s%s: the token '%s' is not in theme %s", what, name, word, themes[i]); return; }
                    if (!kimb_literal(t, *tok.value)) { err(line, "%s%s: the token '%s' in theme %s is not %s", what, name, word, themes[i], kimb_type_name(t)); return; }
                }
                return;
            }
        }
        if (kimb_literal(t, v)) {
            if ((t == KimbType::Box4 || t == KimbType::Shadow || t == KimbType::Gain) && v.is_array())
                for (const JsonValue* x = json_first(f.doc, v); x; x = json_next(f.doc, *x))
                    if (!x->is_number()) { err(line, "%s%s takes %s", what, name, kimb_type_name(t)); return; }
            return;
        }
        if (word && (t == KimbType::Color || t == KimbType::Number || t == KimbType::Box4 || t == KimbType::Shadow || t == KimbType::Gain || !theme_count) &&
            word[0] != '#' && !strstr(word, ".."))
            err(line, "%s%s: '%s' is not %s, nor a token of %s", what, name, word, kimb_type_name(t), theme_count ? "every theme" : "a theme (there are none)");
        else
            err(line, "%s%s takes %s", what, name, kimb_type_name(t));
    }

    void kernel_props(u32 line, const char* what, const JsonValue* p, bool looks_only) {
        if (!p) return;
        for (const JsonValue* v = json_first(f.doc, *p); v; v = json_next(f.doc, *v)) {
            const KimbKernelProp* k = kimb_kernel_prop(v->key);
            if (!k) { err(line, "%s'%s' is not a property", what, v->key); continue; }
            if (looks_only && k->layout) { err(line, "%s%s is layout, which a look cannot set", what, v->key); continue; }
            value(line, what, v->key, k->type, *v);
        }
    }

    void flags(u32 line, const JsonValue* fl) {
        if (!fl) return;
        for (const JsonValue* v = json_first(f.doc, *fl); v; v = json_next(f.doc, *v))
            if (v->str() && !kimb_kernel_flag(v->str())) err(line, "'%s' is not a flag", v->str());
    }

    void styles(u32 line, const JsonValue* st) {
        if (!st) return;
        for (const JsonValue* v = json_first(f.doc, *st); v; v = json_next(f.doc, *v))
            if (v->str() && !kimb_find_style(s, v->str(), nullptr)) err(line, "no style '%s'", v->str());
    }

    // A node's looks by state (`hover { region=... }` among its children), as a style's are.
    void node_looks(u32 line, const JsonValue* looks) {
        if (!looks) return;
        for (const JsonValue* l = json_first(f.doc, *looks); l; l = json_next(f.doc, *l)) {
            if (!kimb_state(l->key)) { err(line, "'%s' is not a state (hover, pressed, focused, disabled, selected)", l->key); continue; }
            char what[48];
            format(what, sizeof what, "%s.", l->key);
            kernel_props(line, what, l, true);
        }
    }

    static bool has_slot(const JsonDoc& doc, const JsonValue& n) {
        if (!strcmp(json_string(json_get(doc, n, "w"), ""), "slot")) return true;
        if (const JsonValue* c = json_get(doc, n, "c"))
            for (const JsonValue* k = json_first(doc, *c); k; k = json_next(doc, *k))
                if (has_slot(doc, *k)) return true;
        return false;
    }

    void node(const JsonValue& n, bool in_def) {
        const char* w = json_string(json_get(f.doc, n, "w"), "");
        u32 line = n.line;
        const JsonValue* kids = json_get(f.doc, n, "c");
        auto walk_kids = [&] {
            if (kids) for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k)) node(*k, in_def);
        };
        if (!strcmp(w, "each")) {
            // A sparse each (count= or from=) takes one placeholder: the block for rows the
            // model does not hold, its children checked as the each's own are. The builder
            // finds it by its word, so it wins over a definition of that name here.
            bool sparse = json_get(f.doc, n, "count") || json_get(f.doc, n, "from");
            bool seen = false;
            if (kids)
                for (const JsonValue* k = json_first(f.doc, *kids); k; k = json_next(f.doc, *k)) {
                    if (!sparse || strcmp(json_string(json_get(f.doc, *k, "w"), ""), "placeholder")) { node(*k, in_def); continue; }
                    if (seen) err(k->line, "a sparse each takes one placeholder");
                    seen = true;
                    static const char* const takes_none[] = {"p", "f", "s", "t", "a", "r", "e"};
                    for (const char* key : takes_none)
                        if (json_get(f.doc, *k, key)) { err(k->line, "a placeholder takes no properties, flags, styles or strings: only children"); break; }
                    if (const JsonValue* pk = json_get(f.doc, *k, "c"))
                        for (const JsonValue* c = json_first(f.doc, *pk); c; c = json_next(f.doc, *c)) node(*c, in_def);
                }
            return;
        }
        if (!strcmp(w, "if")) { walk_kids(); return; }
        if (!strcmp(w, "slot")) {
            if (!in_def) err(line, "slot is only in a definition's body");
            if (kids || json_get(f.doc, n, "p") || json_get(f.doc, n, "f")) err(line, "a slot is a place: it takes nothing");
            return;
        }
        styles(line, json_get(f.doc, n, "s"));
        flags(line, json_get(f.doc, n, "f"));
        node_looks(line, json_get(f.doc, n, "looks"));
        const JsonValue* p = json_get(f.doc, n, "p");
        const JsonValue* t = json_get(f.doc, n, "t");
        const JsonValue* a = json_get(f.doc, n, "a");
        if (kimb_kernel_node(w)) {
            kernel_props(line, "", p, false);
            bool takes_text = !strcmp(w, "text") || !strcmp(w, "edit");
            if (t && !takes_text && strcmp(w, "image")) err(line, "a %s takes no content: put a text in it", w); // an image's is its path
            if (a) err(line, "a %s takes one content string", w);
            if (json_get(f.doc, n, "r") && !(p && json_get(f.doc, *p, "behave"))) err(line, "a %s takes no range: only one with a behaviour does", w);
            if (kids && (takes_text || !strcmp(w, "image"))) err(line, "a %s takes no children", w);
            walk_kids();
            return;
        }
        KimbFound d;
        if (!kimb_find_def(s, w, &d)) {
            if (!strcmp(w, "placeholder")) err(line, "placeholder is a sparse each's: one with count= or from=");
            else err(line, "'%s' is neither a kernel node (box, text, image, hit, edit) nor a definition", w);
            walk_kids();
            return;
        }
        const JsonDoc& dd = d.file->doc;
        const JsonValue* params = json_get(dd, *d.value, "params");
        u32 nparams = params ? params->count() : 0;
        u32 nstrings = (t ? 1 : 0) + (a ? a->count() : 0);
        if (nstrings > nparams) err(line, "%s takes %u parameter%s, and is given %u string%s", w, nparams, nparams == 1 ? "" : "s", nstrings, nstrings == 1 ? "" : "s");
        if (p)
            for (const JsonValue* v = json_first(f.doc, *p); v; v = json_next(f.doc, *v)) {
                bool param = false;
                if (params) for (const JsonValue* q = json_first(dd, *params); q; q = json_next(dd, *q)) if (q->str() && !strcmp(q->str(), v->key)) param = true;
                if (param) {
                    KimbFound decl;
                    KimbType ty = KimbType::Any;
                    if (kimb_find_prop(s, v->key, &decl)) kimb_type_of(json_string(json_get(decl.file->doc, *decl.value, "type"), "any"), &ty);
                    value(line, "", v->key, ty, *v);
                    continue;
                }
                const KimbKernelProp* k = kimb_kernel_prop(v->key);
                if (!k) { err(line, "%s takes no property '%s'", w, v->key); continue; }
                value(line, "", v->key, k->type, *v);
            }
        const JsonValue* body = json_get(dd, *d.value, "body");
        // A range is a behaviour's (a slider's 1..99): the use's or its body's root's.
        const JsonValue* bp = body ? json_get(dd, *body, "p") : nullptr;
        if (json_get(f.doc, n, "r") && !(p && json_get(f.doc, *p, "behave")) && !(bp && json_get(dd, *bp, "behave")))
            err(line, "%s takes no range: only one with a behaviour does", w);
        if (kids && (!body || !has_slot(dd, *body)) && !(bp && json_get(dd, *bp, "behave"))) err(line, "%s takes no children: its body has no slot", w);
        walk_kids();
    }

    void style(const JsonValue& st) {
        u32 line = st.line;
        const JsonValue* bases = json_get(f.doc, st, "s");
        styles(line, bases);
        flags(line, json_get(f.doc, st, "f"));
        kernel_props(line, "", json_get(f.doc, st, "p"), false);
        if (const JsonValue* looks = json_get(f.doc, st, "looks"))
            for (const JsonValue* l = json_first(f.doc, *looks); l; l = json_next(f.doc, *l)) {
                if (!kimb_state(l->key)) { err(l->line ? l->line : line, "'%s' is not a state (hover, pressed, focused, disabled, selected)", l->key); continue; }
                char what[48];
                format(what, sizeof what, "%s.", l->key);
                kernel_props(l->line ? l->line : line, what, l, true);
            }
        // A style that starts from itself, however far round.
        const char* chain[KV_KIMB_MAX_SCOPE];
        u32 depth = 0;
        if (cycles(st.key, st, chain, depth)) err(line, "style %s starts from itself", st.key);
    }

    bool cycles(const char* start, const JsonValue& st, const char** chain, u32 depth) {
        if (depth >= KV_KIMB_MAX_SCOPE) return true;
        const JsonValue* bases = json_get(f.doc, st, "s");
        if (!bases) return false;
        for (const JsonValue* b = json_first(f.doc, *bases); b; b = json_next(f.doc, *b)) {
            const char* name = b->str();
            if (!name) continue;
            if (!strcmp(name, start)) return true;
            KimbFound next;
            if (!kimb_find_style(s, name, &next) || next.file != &f) continue;
            chain[depth] = name;
            if (cycles(start, *next.value, chain, depth + 1)) return true;
        }
        return false;
    }

    // An inline bitmap font: image, cell and chars said, each of its kind; missing one
    // character; advance a number.
    void font(const JsonValue& fo) {
        u32 line = fo.line;
        if (!fo.is_object()) { err(line, "font %s: not a block of name=value", fo.key); return; }
        bool image = false, cell = false, chars = false;
        for (const JsonValue* v = json_first(f.doc, fo); v; v = json_next(f.doc, *v)) {
            const char* k = v->key;
            if (!strcmp(k, "image")) { image = true; if (!v->str() || !*v->str()) err(line, "font %s: image takes an asset path", fo.key); }
            else if (!strcmp(k, "chars")) { chars = true; if (!v->str() || !*v->str()) err(line, "font %s: chars takes the characters, in a string", fo.key); }
            else if (!strcmp(k, "missing")) {
                const char* m = v->str();
                u32 n = 0;
                if (m) for (const char* p = m; *p; ++p) if ((*p & 0xC0) != 0x80) ++n;
                if (n != 1) err(line, "font %s: missing takes one character, in a string", fo.key);
            } else if (!strcmp(k, "advance")) { if (!v->is_number() || v->number() < 0) err(line, "font %s: advance takes a number of pixels", fo.key); }
            else if (!strcmp(k, "cell")) {
                cell = true;
                const JsonValue* w = v->is_array() ? json_first(f.doc, *v) : nullptr;
                const JsonValue* h = w ? json_next(f.doc, *w) : nullptr;
                if (!w || !h || json_next(f.doc, *h) || !w->is_number() || !h->is_number() || w->number() < 1 || h->number() < 1)
                    err(line, "font %s: cell takes a width and a height (cell=9,13)", fo.key);
            } else err(line, "font %s: '%s' is not a font's word (image, cell, chars, missing, advance)", fo.key, k);
        }
        if (!image || !cell || !chars) err(line, "font %s needs image, cell and chars", fo.key);
    }

    void run() {
        if (f.fonts) for (const JsonValue* fo = json_first(f.doc, *f.fonts); fo; fo = json_next(f.doc, *fo)) font(*fo);
        if (f.props)
            for (const JsonValue* p = json_first(f.doc, *f.props); p; p = json_next(f.doc, *p)) {
                KimbType t;
                const char* type = json_string(json_get(f.doc, *p, "type"), "");
                if (!kimb_type_of(type, &t)) err(p->line, "prop %s: '%s' is not a type (number, color, string, size, any)", json_string(json_get(f.doc, *p, "name"), "?"), type);
                const char* name = json_string(json_get(f.doc, *p, "name"), "");
                if (kimb_kernel_prop(name)) err(p->line, "prop %s: the kernel's property, which a prelude does not declare again", name);
            }
        if (f.styles) for (const JsonValue* st = json_first(f.doc, *f.styles); st; st = json_next(f.doc, *st)) style(*st);
        if (f.screens) for (const JsonValue* sc = json_first(f.doc, *f.screens); sc; sc = json_next(f.doc, *sc)) node(*sc, false);
        if (f.defs)
            for (const JsonValue* d = json_first(f.doc, *f.defs); d; d = json_next(f.doc, *d)) {
                if (kimb_kernel_node(d->key) || !strcmp(d->key, "each") || !strcmp(d->key, "if") || !strcmp(d->key, "slot"))
                    err(d->line, "def %s: a kernel word, which a definition cannot take", d->key);
                if (const JsonValue* body = json_get(f.doc, *d, "body")) node(*body, true);
                else err(d->line, "def %s has no body", d->key);
            }
    }
};

} // namespace

bool kimb_check(const KimbScope& s, KimErrors& errors) {
    if (!s.count) return true;
    u32 before = errors.total;
    Checker c(s, errors);
    c.run();
    return errors.total == before;
}

} // namespace kv
