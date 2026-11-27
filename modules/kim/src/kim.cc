// The .kim parser (kim.h). One pass over the text into a small tree of its own (lines,
// words, values already made as builder refs), then the document emitted from that tree in
// canonical order through core's JsonBuilder, the lines sidecar filled in the same walk.
// It knows the grammar only: a word it does not know is not its error.
#include "korvid/kim/kim.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/core/kbor.h"
#include <stdarg.h>
#include <string.h>

namespace kv {

namespace {

struct Prop {
    const char* key;
    JsonRef     value;
    u32         order; // as written, so the last of a repeated key wins
};

struct Look {
    const char* state;
    u32         line;
    Array<Prop> props;
};

struct Node {
    const char*        word = nullptr;
    u32                line = 0;
    const char*        id = nullptr;
    const char*        binding = nullptr;
    const char*        bind_value = nullptr; // @key=value
    const char*        event = nullptr;
    const char*        var = nullptr; // each
    const char*        in = nullptr;  // each
    JsonRef            height = KV_JSON_NO_REF; // each ... height=24: a virtualized each's rows
    JsonRef            count  = KV_JSON_NO_REF; // each ... count=@total: a sparse each's length, a number or a model key
    JsonRef            from   = KV_JSON_NO_REF; // each ... from=@first: the row its list's first element is
    const char*        on = nullptr;  // if
    const char*        on_value = nullptr; // if key=value
    JsonRef            range = KV_JSON_NO_REF;
    Array<const char*> flags{};
    Array<Prop>        props{};
    Array<const char*> styles{};
    Array<JsonRef>     strings{};
    Array<Node*>       kids{};
    Array<Look>        looks{}; // `hover { region=60,0,20,20 }` among its children: a state's look
};

struct Named { // a screen, definition, style or theme
    const char* name;
    u32         line;
    void*       what;
};

struct Style {
    Array<const char*> bases{};
    Array<const char*> flags{};
    Array<Prop>        props{};
    Array<Look>        looks{};
};

struct Def {
    Array<const char*> params{};
    Node*              body = nullptr;
};

struct Theme {
    Array<Prop> tokens{};
    Array<u32>  lines{};
};

struct Entry { // a use, a script, a prop
    const char* a;
    const char* b;
    u32         line;
};

struct Parser {
    const char* text;
    usize       len;
    usize       at = 0;
    u32         line = 1;
    usize       line_start = 0;
    const char* file;
    Allocator*  alloc;
    JsonBuilder b;
    KimErrors&  errors;
    bool        broken = false; // a structural error: stop

    Array<Entry> uses{}, scripts{}, props{};
    Array<Named> screens{}, defs{}, styles{}, themes{}, fonts{};
    u32          order = 0;

    Parser(const char* t, usize n, const char* f, Allocator* a, KimErrors& e) : text(t), len(n), file(f), alloc(a), b(a), errors(e) {
        uses.alloc = scripts.alloc = props.alloc = a;
        screens.alloc = defs.alloc = styles.alloc = themes.alloc = fonts.alloc = a;
    }

    template <class T> T* make() {
        T* p = (T*)alloc_in(alloc, sizeof(T), alignof(T));
        if (!p) return nullptr;
        *p = T{};
        return p;
    }
    template <class T> void owned(Array<T>& arr) { arr.alloc = alloc; }

    u32 col() const { return (u32)(at - line_start) + 1; }
    char peek(usize k = 0) const { return at + k < len ? text[at + k] : 0; }
    bool end() const { return at >= len; }

    void err(u32 c, const char* fmt, ...) KV_PRINTF(3, 4) {
        char msg[200];
        va_list args;
        va_start(args, fmt);
        vformat(msg, sizeof msg, fmt, args);
        va_end(args);
        errors.add(file, line, c, "%s", msg);
    }
    void err_at(u32 l, const char* fmt, ...) KV_PRINTF(3, 4) {
        char msg[200];
        va_list args;
        va_start(args, fmt);
        vformat(msg, sizeof msg, fmt, args);
        va_end(args);
        errors.add(file, l, 0, "%s", msg);
    }

    // Spaces, tabs and a comment; not a newline.
    void blanks() {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') { ++at; continue; }
            if (c == '/' && peek(1) == '/') { while (!end() && peek() != '\n') ++at; continue; }
            return;
        }
    }
    void newline() { ++at; ++line; line_start = at; }
    // Blanks and newlines.
    void space() {
        for (;;) {
            blanks();
            if (peek() == '\n') { newline(); continue; }
            return;
        }
    }
    // What may follow a node or a top-level line: the end of the line, or a closing brace.
    bool at_line_end() { blanks(); return end() || peek() == '\n' || peek() == '}'; }

    static bool word_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool word_char(char c) { return word_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    static bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    // Where a bad token ends: the next blank, newline or brace.
    void skip_token() {
        while (!end()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '{' || c == '}') return;
            ++at;
        }
    }
    void skip_line() { while (!end() && peek() != '\n' && peek() != '}') ++at; }

    const char* copy(const char* s, usize n) {
        char* d = (char*)alloc_in(alloc, n + 1, 1);
        if (!d) { broken = true; return ""; }
        memcpy(d, s, n);
        d[n] = 0;
        return d;
    }

    const char* word() {
        if (!word_start(peek())) return nullptr;
        usize s = at;
        while (word_char(peek())) ++at;
        return copy(text + s, at - s);
    }

    // At the opening quote. The unescaped text, or null after reporting.
    bool string(const char** out, u32* n_out) {
        u32 c0 = col();
        ++at;
        char* buf = (char*)alloc_in(alloc, len - at + 1, 1);
        if (!buf) { broken = true; return false; }
        u32 n = 0;
        while (!end() && peek() != '"' && peek() != '\n') {
            char c = peek();
            if (c == '\\') {
                char e = peek(1);
                at += 2;
                switch (e) {
                case '"': buf[n++] = '"'; break;
                case '\\': buf[n++] = '\\'; break;
                case 'n': buf[n++] = '\n'; break;
                case 't': buf[n++] = '\t'; break;
                default: err(col() - 2, "unknown escape '\\%c'", e ? e : '?'); return false;
                }
                continue;
            }
            buf[n++] = c;
            ++at;
        }
        if (peek() != '"') { err(c0, "unterminated string"); return false; }
        ++at;
        buf[n] = 0;
        *out = buf;
        *n_out = n;
        return true;
    }

    // A number at `at`: its extent, whether it had a fraction. False if there is none.
    bool number_span(usize& s, usize& e, bool& frac) {
        s = at;
        usize p = at;
        frac = false;
        if (p < len && text[p] == '-') ++p;
        if (!(p < len && digit(text[p]))) return false;
        while (p < len && digit(text[p])) ++p;
        if (p + 1 < len && text[p] == '.' && digit(text[p + 1])) {
            frac = true;
            ++p;
            while (p < len && digit(text[p])) ++p;
        }
        e = p;
        return true;
    }

    JsonRef number_ref(usize s, usize e, bool frac) {
        char tmp[64];
        usize n = e - s < sizeof tmp - 1 ? e - s : sizeof tmp - 1;
        memcpy(tmp, text + s, n);
        tmp[n] = 0;
        if (!frac) {
            bool neg = tmp[0] == '-';
            u64 v = 0;
            for (const char* p = tmp + (neg ? 1 : 0); *p; ++p) v = v * 10 + (u64)(*p - '0');
            return neg ? b.integer(-(i64)v) : b.integer((i64)v);
        }
        // A decimal fraction: as the C library reads it.
        f64 v = 0, scale = 1;
        bool neg = tmp[0] == '-';
        const char* p = tmp + (neg ? 1 : 0);
        while (*p && *p != '.') v = v * 10 + (*p++ - '0');
        if (*p == '.') {
            ++p;
            u64 fr = 0;
            while (*p) { fr = fr * 10 + (u64)(*p++ - '0'); scale *= 10; }
            v += (f64)fr / scale;
        }
        return b.number(neg ? -v : v);
    }

    // A range or a number at `at` (a range written as a property's value is kept as its
    // text, "120..400", as a Lua table writes it). `bare`: the node's own range, [min, max].
    // KV_JSON_NO_REF when what is here is neither.
    JsonRef range_or_number(bool bare) {
        usize s0 = at;
        usize s = 0, e = 0;
        bool frac = false;
        bool lo = number_span(s, e, frac);
        usize after = lo ? e : at;
        if (after + 1 < len && text[after] == '.' && text[after + 1] == '.') {
            usize s2 = 0, e2 = 0;
            bool frac2 = false;
            usize save = at;
            at = after + 2;
            bool hi = number_span(s2, e2, frac2);
            at = save;
            if (!lo && !hi) return KV_JSON_NO_REF;
            usize stop = hi ? e2 : after + 2;
            JsonRef r;
            if (bare) {
                r = b.array();
                b.push(r, lo ? number_ref(s, e, frac) : b.null());
                b.push(r, hi ? number_ref(s2, e2, frac2) : b.null());
            } else {
                r = b.string(text + s0, (u32)(stop - s0));
            }
            at = stop;
            return r;
        }
        if (!lo || bare) return KV_JSON_NO_REF;
        at = e;
        return number_ref(s, e, frac);
    }

    // One value of a property: a string, a color, a number or range, or a word.
    JsonRef scalar() {
        char c = peek();
        if (c == '"') {
            const char* s;
            u32 n;
            if (!string(&s, &n)) return KV_JSON_NO_REF;
            return b.string(s, n);
        }
        if (c == '#') {
            u32 c0 = col();
            usize s = at++;
            while (hex(peek())) ++at;
            usize n = at - s - 1;
            if ((n != 3 && n != 4 && n != 6 && n != 8) || word_char(peek())) {
                skip_token();
                err(c0, "a color is #rrggbb or #rrggbbaa (or #rgb, #rgba)");
                return KV_JSON_NO_REF;
            }
            return b.string(text + s, (u32)(at - s));
        }
        if (digit(c) || c == '-' || c == '.') {
            u32 c0 = col();
            usize start = at;
            JsonRef r = range_or_number(false);
            // A percentage is kept as written, "40%", as a Lua table writes it.
            if (r != KV_JSON_NO_REF && peek() == '%') { ++at; r = b.string(text + start, (u32)(at - start)); }
            if (r == KV_JSON_NO_REF || word_char(peek())) {
                skip_token();
                err(c0, "not a number, a percentage or a range");
                return KV_JSON_NO_REF;
            }
            return r;
        }
        if (c == '@') {
            // A property bound to the model: "@path", read when the node is built and set
            // again when the path's key changes.
            u32 c0 = col();
            usize start = at++;
            // @@: in a definition's body, the binding its use was given (`slider @level`).
            if (peek() == '@') { ++at; if (word_char(peek())) { err(c0, "@@ stands alone: the use's binding"); skip_token(); return KV_JSON_NO_REF; } return b.string("@@"); }
            const char* w = word();
            if (!w) { err(c0, "a bound value is @name"); skip_token(); return KV_JSON_NO_REF; }
            return b.string(text + start, (u32)(at - start));
        }
        if (word_start(c)) {
            const char* w = word();
            return b.string(w);
        }
        err(col(), "expected a value");
        skip_token();
        return KV_JSON_NO_REF;
    }

    // A property's value: a scalar, or a list of them joined by commas with no spaces.
    JsonRef value() {
        JsonRef first = scalar();
        if (first == KV_JSON_NO_REF || peek() != ',') return first;
        JsonRef list = b.array();
        b.push(list, first);
        while (peek() == ',') {
            ++at;
            JsonRef v = scalar();
            if (v == KV_JSON_NO_REF) return KV_JSON_NO_REF;
            b.push(list, v);
        }
        return list;
    }

    // `[a b c]` at the bracket, appended to `out`.
    bool style_list(Array<const char*>& out) {
        u32 c0 = col();
        ++at;
        for (;;) {
            blanks();
            if (peek() == ']') { ++at; return true; }
            const char* w = word();
            if (!w) { err(c0, "styles are words in brackets: [a b]"); skip_line(); return false; }
            out.push(w);
        }
    }

    // A block's children, at the opening brace, up to and past its closing one.
    void children(Node* n) {
        u32 open_line = line;
        ++at;
        for (;;) {
            space();
            if (broken) return;
            if (end()) { err_at(open_line, "this block is never closed"); broken = true; return; }
            if (peek() == '}') { ++at; return; }
            if (node_look(n)) continue;
            Node* k = node();
            if (k) n->kids.push(k);
            if (broken) return;
            if (!at_line_end()) { err(col(), "one node to a line"); skip_line(); }
        }
    }

    // A state's look among a node's children (`hover { bg=accent }`), as a style has: true
    // when there was one, read up to and past its closing brace.
    bool node_look(Node* n) {
        static const char* const states[] = {"hover", "pressed", "focused", "disabled", "selected"};
        usize was = at;
        const char* w = word();
        bool state = false;
        for (const char* st : states) if (w && !strcmp(w, st)) state = true;
        if (state) blanks();
        if (!state || peek() != '{') { at = was; return false; }
        u32 sl = line;
        Look look{w, sl, {}};
        owned(look.props);
        ++at; // the brace
        for (;;) {
            space();
            if (end()) { err_at(sl, "this block is never closed"); broken = true; return true; }
            if (peek() == '}') { ++at; break; }
            const char* inner = pairs(look.props, nullptr, nullptr);
            if (inner) { err(col(), "a look holds name=value, not a block"); skip_block(); }
        }
        for (Look& o : n->looks)
            if (!strcmp(o.state, w)) { for (const Prop& p : look.props) o.props.push(p); return true; }
        n->looks.push(look);
        return true;
    }

    Node* new_node(const char* w, u32 l) {
        Node* n = make<Node>();
        if (!n) { broken = true; return nullptr; }
        n->word = w;
        n->line = l;
        owned(n->flags); owned(n->props); owned(n->styles); owned(n->strings); owned(n->kids); owned(n->looks);
        return n;
    }

    // A node at its word.
    Node* node() {
        u32 c0 = col();
        const char* w = word();
        if (!w) { err(c0, "expected a node"); skip_line(); if (peek() == '{') skip_block(); return nullptr; }
        Node* n = new_node(w, line);
        if (!n) return nullptr;

        if (!strcmp(w, "each")) {
            blanks();
            n->var = word();
            blanks();
            const char* kw = word();
            blanks();
            n->in = word();
            if (!n->var || !kw || strcmp(kw, "in") || !n->in) { err(c0, "each is written 'each item in list { ... }'"); skip_line(); }
            blanks();
            // A fixed height for every element: only those in view are built.
            // A sparse one (count=, from=): the list is a window of rows, `count` long in all,
            // its first element row `from`; `-> name` is the event that says which rows are
            // in view. Any order.
            for (;;) {
                if (!strncmp(text + at, "height=", 7)) {
                    at += 7;
                    u32 c1 = col();
                    JsonRef h = scalar();
                    if (h == KV_JSON_NO_REF || b.type(h) != JsonType::Number) err(c1, "height takes a number of pixels");
                    else n->height = h;
                } else if (!strncmp(text + at, "count=", 6) || !strncmp(text + at, "from=", 5)) {
                    bool is_count = text[at] == 'c';
                    at += is_count ? 6 : 5;
                    u32 c1 = col();
                    JsonRef v = scalar();
                    if (v == KV_JSON_NO_REF || (b.type(v) != JsonType::Number && b.type(v) != JsonType::String))
                        err(c1, is_count ? "count takes a number or @key" : "from takes a number or @key");
                    else (is_count ? n->count : n->from) = v;
                } else if (peek() == '-' && peek(1) == '>') {
                    at += 2;
                    blanks();
                    n->event = word();
                    if (!n->event) err(col(), "an event is '-> name'");
                } else break;
                blanks();
            }
            if (n->height == KV_JSON_NO_REF && (n->count != KV_JSON_NO_REF || n->from != KV_JSON_NO_REF)) err(c0, "a sparse each (count=, from=) needs height=");
            if (peek() == '{') children(n);
            else err(c0, "each needs a block");
            return n;
        }
        if (!strcmp(w, "if")) {
            blanks();
            n->on = word();
            if (!n->on) { err(c0, "if is written 'if name { ... }'"); skip_line(); }
            // if key=value: built while the model's value there is that (a tab's page).
            else if (peek() == '=') {
                ++at;
                u32 c1 = col();
                if (peek() == '"') { const char* sv = nullptr; u32 sn = 0; if (string(&sv, &sn)) n->on_value = sv; }
                else {
                    usize vs = at;
                    while (!end() && (word_char(peek()) || peek() == '-')) ++at;
                    if (at > vs) n->on_value = copy(text + vs, at - vs);
                }
                if (!n->on_value) { err(c1, "if key=value takes a word or a string"); skip_line(); }
            }
            blanks();
            if (peek() == '{') children(n);
            else err(c0, "if needs a block");
            return n;
        }

        for (;;) {
            blanks();
            char c = peek();
            if (end() || c == '\n' || c == '}') return n;
            if (c == '{') { children(n); return n; }
            u32 ci = col();
            if (c == '"') {
                const char* s;
                u32 len_s;
                if (string(&s, &len_s)) n->strings.push(b.string(s, len_s));
                else skip_token();
                continue;
            }
            if (c == '[') { style_list(n->styles); continue; }
            if (c == '@') {
                ++at;
                const char* bw = word();
                if (!bw) { err(ci, "a binding is @name"); skip_token(); continue; }
                if (n->binding) err(ci, "one binding to a node");
                n->binding = bw;
                // @key=value: selected while the model holds that value; a press writes it.
                if (peek() == '=') {
                    ++at;
                    const char* v = nullptr;
                    u32 vn = 0;
                    if (peek() == '"') { if (!string(&v, &vn)) { skip_token(); continue; } }
                    else {
                        usize vs = at;
                        while (!end() && (word_char(peek()) || peek() == '-')) ++at;
                        if (at > vs) v = copy(text + vs, at - vs);
                    }
                    if (!v) { err(ci, "a binding's value is @name=value"); skip_token(); continue; }
                    n->bind_value = v;
                }
                continue;
            }
            if (c == '-' && peek(1) == '>') {
                at += 2;
                blanks();
                const char* ew = word();
                if (!ew) { err(ci, "an event is '-> name'"); skip_token(); continue; }
                if (n->event) err(ci, "one event to a node");
                n->event = ew;
                continue;
            }
            if (digit(c) || c == '.' || c == '-') {
                JsonRef r = range_or_number(true);
                if (r == KV_JSON_NO_REF || word_char(peek())) { err(ci, "a node takes a bare range (1..99), not a number"); skip_token(); continue; }
                if (n->range != KV_JSON_NO_REF) err(ci, "one range to a node");
                n->range = r;
                continue;
            }
            if (word_start(c)) {
                const char* fw = word();
                if (peek() == '=') {
                    ++at;
                    if (!strcmp(fw, "id")) {
                        const char* iw = nullptr;
                        if (peek() == '"') { u32 nn; if (!string(&iw, &nn)) { skip_token(); continue; } }
                        else iw = word();
                        if (!iw) { err(ci, "an id is a word"); skip_token(); continue; }
                        n->id = iw;
                        continue;
                    }
                    JsonRef v = value();
                    if (v != KV_JSON_NO_REF) n->props.push(Prop{fw, v, order++});
                    continue;
                }
                n->flags.push(fw);
                continue;
            }
            err(ci, "unexpected '%c'", c);
            skip_token();
        }
    }

    void skip_block() {
        int depth = 0;
        while (!end()) {
            char c = peek();
            if (c == '"') { const char* s; u32 n; if (!string(&s, &n)) skip_line(); continue; }
            if (c == '\n') { newline(); continue; }
            ++at;
            if (c == '{') ++depth;
            else if (c == '}' && --depth <= 0) return;
        }
    }

    // A block that must hold exactly one node: a screen's, a definition's.
    Node* one_node(const char* what, const char* name, u32 l) {
        blanks();
        if (peek() != '{') { err(col(), "%s %s needs a block", what, name); skip_line(); return nullptr; }
        Node holder{};
        owned(holder.kids);
        children(&holder);
        if (holder.kids.len != 1) {
            err_at(l, "%s %s holds one node, not %u", what, name, holder.kids.len);
            return holder.kids.len ? holder.kids[0] : nullptr;
        }
        return holder.kids[0];
    }

    // name=value pairs to the end of the line (a style's or a theme's), and in a style, flags.
    // Stops at the line's end or a closing brace. A word followed by a block is handed back.
    const char* pairs(Array<Prop>& out, Array<const char*>* flags, Array<u32>* line_at) {
        for (;;) {
            blanks();
            char c = peek();
            if (end() || c == '\n' || c == '}') return nullptr;
            u32 ci = col();
            const char* w = word();
            if (!w) { err(ci, "expected name=value"); skip_token(); continue; }
            if (peek() == '=') {
                ++at;
                JsonRef v = value();
                if (v != KV_JSON_NO_REF) {
                    out.push(Prop{w, v, order++});
                    if (line_at) line_at->push(line);
                }
                continue;
            }
            blanks();
            if (peek() == '{') return w;
            if (flags) flags->push(w);
            else err(ci, "expected name=value, not '%s'", w);
        }
    }

    void style(const char* name, u32 l) {
        Style* st = make<Style>();
        if (!st) { broken = true; return; }
        owned(st->bases); owned(st->flags); owned(st->props); owned(st->looks);
        blanks();
        if (peek() == '[') style_list(st->bases);
        blanks();
        if (peek() != '{') { err(col(), "style %s needs a block", name); skip_line(); return; }
        u32 open_line = line;
        ++at;
        for (;;) {
            space();
            if (end()) { err_at(open_line, "this block is never closed"); broken = true; return; }
            if (peek() == '}') { ++at; break; }
            const char* state = pairs(st->props, &st->flags, nullptr);
            if (!state) continue;
            u32 sl = line;
            Look look{state, sl, {}};
            owned(look.props);
            ++at; // the brace
            for (;;) {
                space();
                if (end()) { err_at(sl, "this block is never closed"); broken = true; return; }
                if (peek() == '}') { ++at; break; }
                const char* inner = pairs(look.props, nullptr, nullptr);
                if (inner) { err(col(), "a look holds name=value, not a block"); skip_block(); }
            }
            bool merged = false;
            for (Look& o : st->looks)
                if (!strcmp(o.state, state)) {
                    for (const Prop& p : look.props) o.props.push(p);
                    merged = true;
                }
            if (!merged) st->looks.push(look);
        }
        add_named(styles, "style", name, l, st);
    }

    void theme(const char* name, u32 l) {
        Theme* th = make<Theme>();
        if (!th) { broken = true; return; }
        owned(th->tokens); owned(th->lines);
        blanks();
        if (peek() != '{') { err(col(), "theme %s needs a block", name); skip_line(); return; }
        u32 open_line = line;
        ++at;
        for (;;) {
            space();
            if (end()) { err_at(open_line, "this block is never closed"); broken = true; return; }
            if (peek() == '}') { ++at; break; }
            const char* w = pairs(th->tokens, nullptr, &th->lines);
            if (w) { err(col(), "a theme holds name=value"); skip_block(); }
        }
        add_named(themes, "theme", name, l, th);
    }

    // A bitmap font inline (Krome slice 7): `font digits { image="skin/digits.png" cell=9,13
    // chars="0123456789" }`. Its words are kimb's to check.
    void font(const char* name, u32 l) {
        Theme* fo = make<Theme>();
        if (!fo) { broken = true; return; }
        owned(fo->tokens); owned(fo->lines);
        blanks();
        if (peek() != '{') { err(col(), "font %s needs a block", name); skip_line(); return; }
        u32 open_line = line;
        ++at;
        for (;;) {
            space();
            if (end()) { err_at(open_line, "this block is never closed"); broken = true; return; }
            if (peek() == '}') { ++at; break; }
            const char* w = pairs(fo->tokens, nullptr, &fo->lines);
            if (w) { err(col(), "a font holds name=value"); skip_block(); }
        }
        add_named(fonts, "font", name, l, fo);
    }

    void def(const char* name, u32 l) {
        Def* d = make<Def>();
        if (!d) { broken = true; return; }
        owned(d->params);
        for (;;) {
            blanks();
            if (peek() == '{' || at_line_end()) break;
            u32 ci = col();
            const char* p = word();
            if (!p) { err(ci, "a definition's parameters are words"); skip_token(); continue; }
            for (const char* q : d->params)
                if (!strcmp(q, p)) err(ci, "parameter '%s' is named twice", p);
            d->params.push(p);
        }
        d->body = one_node("def", name, l);
        add_named(defs, "def", name, l, d);
    }

    void add_named(Array<Named>& list, const char* what, const char* name, u32 l, void* p) {
        for (const Named& n : list)
            if (!strcmp(n.name, name)) { err_at(l, "%s %s is defined twice (first at line %u)", what, name, n.line); return; }
        list.push(Named{name, l, p});
    }

    void entry(Array<Entry>& list, const char* what) {
        u32 l = line;
        blanks();
        if (peek() != '"') { err(col(), "%s takes a path in quotes", what); skip_line(); return; }
        const char* s;
        u32 n;
        if (!string(&s, &n)) { skip_line(); return; }
        for (const Entry& e : list)
            if (!strcmp(e.a, s)) { err_at(l, "%s \"%s\" is written twice (first at line %u)", what, s, e.line); return; }
        list.push(Entry{s, nullptr, l});
    }

    void parse() {
        for (;;) {
            space();
            if (broken || end()) return;
            u32 l = line;
            u32 c0 = col();
            if (peek() == '}') { err(c0, "a closing brace with nothing open"); ++at; continue; }
            const char* w = word();
            if (!w) { err(c0, "expected use, script, prop, style, theme, def or screen"); skip_line(); if (peek() == '}') ++at; continue; }
            blanks();
            if (!strcmp(w, "use")) entry(uses, "use");
            else if (!strcmp(w, "script")) entry(scripts, "script");
            else if (!strcmp(w, "prop")) {
                const char* name = word();
                blanks();
                const char* type = word();
                if (!name || !type) { err(c0, "prop is written 'prop name type'"); skip_line(); }
                else {
                    bool dup = false;
                    for (const Entry& e : props)
                        if (!strcmp(e.a, name)) { err_at(l, "prop %s is declared twice (first at line %u)", name, e.line); dup = true; }
                    if (!dup) props.push(Entry{name, type, l});
                }
            } else {
                const char* name = word();
                if (!name) { err(col(), "%s needs a name", w); skip_line(); if (peek() == '{') skip_block(); continue; }
                if (!strcmp(w, "style")) style(name, l);
                else if (!strcmp(w, "theme")) theme(name, l);
                else if (!strcmp(w, "font")) font(name, l);
                else if (!strcmp(w, "def")) def(name, l);
                else if (!strcmp(w, "screen")) {
                    Node* root = one_node("screen", name, l);
                    if (root) add_named(screens, "screen", name, l, root);
                } else { err(c0, "'%s' is not use, script, prop, style, theme, font, def or screen", w); skip_line(); if (peek() == '{') skip_block(); continue; }
            }
            if (!broken && !at_line_end()) { err(col(), "unexpected text after %s", w); skip_line(); }
        }
    }

    // ---- emitting ------------------------------------------------------------------------

    JsonRef lines = KV_JSON_NO_REF;
    bool    keep_lines = true;

    void mark(u32 l) { if (keep_lines) b.push(lines, b.integer(l)); }

    // Sorted by name, the last of a repeated name kept.
    void sorted(Array<Prop>& props_in, Array<Prop>& out) {
        out.alloc = alloc;
        for (const Prop& p : props_in) {
            u32 i = 0;
            while (i < out.len && strcmp(out[i].key, p.key) < 0) ++i;
            if (i < out.len && !strcmp(out[i].key, p.key)) {
                if (p.order > out[i].order) out[i] = p;
                continue;
            }
            out.push(p);
            memmove(&out.data[i + 1], &out.data[i], (out.len - 1 - i) * sizeof(Prop));
            out.data[i] = p;
        }
    }
    // Indices of `list` in name order.
    void by_name(const Array<Named>& list, Array<u32>& out) {
        out.alloc = alloc;
        for (u32 k = 0; k < list.len; ++k) {
            u32 i = 0;
            while (i < out.len && strcmp(list[out[i]].name, list[k].name) < 0) ++i;
            out.push(k);
            memmove(&out.data[i + 1], &out.data[i], (out.len - 1 - i) * sizeof(u32));
            out.data[i] = k;
        }
    }

    JsonRef words(const Array<const char*>& w) {
        JsonRef a = b.array();
        for (const char* s : w) b.push(a, b.string(s));
        return a;
    }

    JsonRef prop_map(Array<Prop>& in) {
        Array<Prop> s{};
        sorted(in, s);
        JsonRef o = b.object();
        for (const Prop& p : s) b.set(o, p.key, p.value);
        return o;
    }

    // Looks by state name; a style's each have a line in the sidecar, a node's take the node's.
    JsonRef looks_map(Array<Look>& looks, bool marked) {
        Array<u32> li = array_in<u32>(alloc);
        for (u32 k = 0; k < looks.len; ++k) {
            u32 j = 0;
            while (j < li.len && strcmp(looks[li[j]].state, looks[k].state) < 0) ++j;
            li.push(k);
            memmove(&li.data[j + 1], &li.data[j], (li.len - 1 - j) * sizeof(u32));
            li.data[j] = k;
        }
        JsonRef lo = b.object();
        for (u32 k : li) {
            if (marked) mark(looks[k].line);
            b.set(lo, looks[k].state, prop_map(looks[k].props));
        }
        return lo;
    }

    JsonRef emit_node(Node* n) {
        JsonRef o = b.object();
        mark(n->line);
        b.set(o, "w", b.string(n->word));
        if (n->id) b.set(o, "id", b.string(n->id));
        if (n->flags.len) b.set(o, "f", words(n->flags));
        if (n->props.len) b.set(o, "p", prop_map(n->props));
        if (n->styles.len) b.set(o, "s", words(n->styles));
        if (n->strings.len) b.set(o, "t", n->strings[0]);
        if (n->strings.len > 1) {
            JsonRef a = b.array();
            for (u32 i = 1; i < n->strings.len; ++i) b.push(a, n->strings[i]);
            b.set(o, "a", a);
        }
        if (n->range != KV_JSON_NO_REF) b.set(o, "r", n->range);
        if (n->binding) b.set(o, "b", b.string(n->binding));
        if (n->bind_value) b.set(o, "bv", b.string(n->bind_value));
        if (n->event) b.set(o, "e", b.string(n->event));
        if (n->var) b.set(o, "v", b.string(n->var));
        if (n->in) b.set(o, "in", b.string(n->in));
        if (n->height != KV_JSON_NO_REF) b.set(o, "height", n->height);
        if (n->count != KV_JSON_NO_REF) b.set(o, "count", n->count);
        if (n->from != KV_JSON_NO_REF) b.set(o, "from", n->from);
        if (n->on) b.set(o, "on", b.string(n->on));
        if (n->on_value) b.set(o, "ov", b.string(n->on_value));
        if (n->looks.len) b.set(o, "looks", looks_map(n->looks, false));
        if (n->kids.len) {
            JsonRef c = b.array();
            for (Node* k : n->kids) b.push(c, emit_node(k));
            b.set(o, "c", c);
        }
        return o;
    }

    JsonRef emit() {
        JsonRef root = b.object();
        lines = b.array();
        b.set(root, "kim", b.integer(KV_KIM_VERSION));
        b.set(root, "file", b.string(file ? file : ""));
        if (uses.len) {
            JsonRef a = b.array();
            for (const Entry& e : uses) { b.push(a, b.string(e.a)); mark(e.line); }
            b.set(root, "uses", a);
        }
        if (scripts.len) {
            JsonRef a = b.array();
            for (const Entry& e : scripts) { b.push(a, b.string(e.a)); mark(e.line); }
            b.set(root, "scripts", a);
        }
        if (props.len) {
            // In name order, as every map is: a prop is a declaration, not a sequence.
            Array<u32> idx = array_in<u32>(alloc);
            for (u32 k = 0; k < props.len; ++k) {
                u32 i = 0;
                while (i < idx.len && strcmp(props[idx[i]].a, props[k].a) < 0) ++i;
                idx.push(k);
                memmove(&idx.data[i + 1], &idx.data[i], (idx.len - 1 - i) * sizeof(u32));
                idx.data[i] = k;
            }
            JsonRef a = b.array();
            for (u32 i : idx) {
                JsonRef o = b.object();
                b.set(o, "name", b.string(props[i].a));
                b.set(o, "type", b.string(props[i].b));
                b.push(a, o);
                mark(props[i].line);
            }
            b.set(root, "props", a);
        }
        Array<u32> idx{};
        if (screens.len) {
            JsonRef o = b.object();
            by_name(screens, idx);
            for (u32 i : idx) {
                mark(screens[i].line);
                b.set(o, screens[i].name, emit_node((Node*)screens[i].what));
            }
            b.set(root, "screens", o);
        }
        if (defs.len) {
            JsonRef o = b.object();
            idx = Array<u32>{};
            by_name(defs, idx);
            for (u32 i : idx) {
                Def* d = (Def*)defs[i].what;
                mark(defs[i].line);
                JsonRef dd = b.object();
                b.set(dd, "params", words(d->params));
                if (d->body) b.set(dd, "body", emit_node(d->body));
                b.set(o, defs[i].name, dd);
            }
            b.set(root, "defs", o);
        }
        if (styles.len) {
            JsonRef o = b.object();
            idx = Array<u32>{};
            by_name(styles, idx);
            for (u32 i : idx) {
                Style* st = (Style*)styles[i].what;
                mark(styles[i].line);
                JsonRef so = b.object();
                if (st->bases.len) b.set(so, "s", words(st->bases));
                if (st->flags.len) b.set(so, "f", words(st->flags));
                if (st->props.len) b.set(so, "p", prop_map(st->props));
                if (st->looks.len) b.set(so, "looks", looks_map(st->looks, true));
                b.set(o, styles[i].name, so);
            }
            b.set(root, "styles", o);
        }
        if (themes.len) {
            JsonRef o = b.object();
            idx = Array<u32>{};
            by_name(themes, idx);
            for (u32 i : idx) {
                Theme* th = (Theme*)themes[i].what;
                mark(themes[i].line);
                // Tokens sorted, each with the line of its last writing.
                Array<Prop> s{};
                sorted(th->tokens, s);
                JsonRef to = b.object();
                for (const Prop& p : s) {
                    u32 l = 0;
                    for (u32 k = 0; k < th->tokens.len; ++k)
                        if (th->tokens[k].order == p.order) l = th->lines[k];
                    mark(l);
                    b.set(to, p.key, p.value);
                }
                b.set(o, themes[i].name, to);
            }
            b.set(root, "themes", o);
        }
        if (fonts.len) {
            JsonRef o = b.object();
            idx = Array<u32>{};
            by_name(fonts, idx);
            for (u32 i : idx) {
                mark(fonts[i].line);
                b.set(o, fonts[i].name, prop_map(((Theme*)fonts[i].what)->tokens));
            }
            b.set(root, "fonts", o);
        }
        if (keep_lines) b.set(root, "lines", lines);
        return root;
    }
};

} // namespace

bool kim_parse(const char* text, usize len, const char* file, Allocator* alloc, JsonDoc& doc, KimErrors& errors, u32 flags) {
    u32 before = errors.total;
    Parser p(text, len, file, alloc, errors);
    p.keep_lines = !(flags & KV_KIM_NO_LINES);
    p.parse();
    bool ok = errors.total == before && !p.broken;
    if (!ok) {
        JsonBuilder nb(alloc);
        nb.finish(nb.null(), doc);
        nb.free();
        doc.error = "the .kim file has errors";
        p.b.free();
        return false;
    }
    JsonRef root = p.emit();
    ok = p.b.finish(root, doc);
    if (!ok) errors.add(file, 0, 0, "out of memory building the document");
    p.b.free();
    return ok;
}

bool kim_compile(const char* text, usize len, const char* file, Array<u8>& out, KimErrors& errors, u32 flags) {
    Arena arena;
    if (!arena_create(arena, len * 64 + (1u << 20), "kim", MemTag{})) {
        errors.add(file, 0, 0, "out of memory");
        return false;
    }
    JsonDoc doc;
    bool ok = kim_parse(text, len, file, &arena.a, doc, errors, flags);
    if (ok) kbor_write(doc, out);
    arena_destroy(arena);
    return ok;
}

} // namespace kv
