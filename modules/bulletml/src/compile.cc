// Korvid bulletml: the compiler. A document is read through the xml module, its labelled
// actions, bullets and fires given ids, and every action's body compiled to ops in turn
// (an inline action is queued, not compiled in the middle of its parent, so each action's
// ops stay back to back). Expressions compile to postfix code, constants folded. Then the
// action graph is walked from the entry and every bullet: a recursion or a track nested
// deeper than a runner holds is an error. A document that fails leaves the library as it
// was.
#include "bml_internal.h"
#include "korvid/core/arena.h"
#include "korvid/core/format.h"
#include "korvid/xml/xml.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct Label { const char* name; u32 id; const XmlNode* node; };
enum class Job : u8 { Action, Fire, Ref }; // an action's body; a labelled fire as an action; a bullet's actionRef as a wrapper
struct Pending { u32 id; const XmlNode* node; Job job; };

struct Compiler {
    BmlLibrary&   lib;
    const XmlDoc& doc;
    char*         why;
    usize         why_cap;
    bool          ok = true;
    Array<Label>   actions = array_in<Label>(mem_scratch());
    Array<Label>   bullets = array_in<Label>(mem_scratch());
    Array<Label>   fires   = array_in<Label>(mem_scratch());
    Array<Pending> queue   = array_in<Pending>(mem_scratch());

    Compiler(BmlLibrary& l, const XmlDoc& d, char* w, usize cap) : lib(l), doc(d), why(w), why_cap(cap) {}

    bool fail(const XmlNode* where, const char* fmt, ...) {
        if (!ok) return false;
        ok = false;
        int n = format(why, why_cap, "line %u, <%s>: ", where ? where->line : 0, where ? where->name : "?");
        va_list ap;
        va_start(ap, fmt);
        if (n >= 0 && (usize)n < why_cap) vformat(why + n, why_cap - (usize)n, fmt, ap);
        va_end(ap);
        return false;
    }

    u32 new_action() { lib.actions.push(KV_BML_NONE); return lib.actions.len - 1; }
    u32 find(const Array<Label>& in, const char* name) {
        for (const Label& l : in) if (!strcmp(l.name, name)) return l.id;
        return KV_BML_NONE;
    }

    // ---- expressions ---------------------------------------------------------------------
    // Recursive descent over the text, postfix out, the stack's depth followed.
    const char* p = nullptr;
    const XmlNode* at = nullptr;
    u32 depth = 0, max_depth = 0;
    bool dynamic = false;

    void space() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p; }
    void emit(u32 w) { lib.code.push(w); }
    void push_one() { if (++depth > max_depth) max_depth = depth; }
    bool primary() {
        space();
        if (*p == '(') {
            ++p;
            if (!sum()) return false;
            space();
            if (*p != ')') return fail(at, "a ')' is missing");
            ++p;
            return true;
        }
        if (*p == '$') {
            ++p;
            dynamic = true;
            if (!strncmp(p, "rand", 4)) { p += 4; emit(E_RAND); push_one(); return true; }
            if (!strncmp(p, "rank", 4)) { p += 4; emit(E_RANK); push_one(); return true; }
            if (*p < '1' || *p > '9') return fail(at, "a '$' names a param ($1..$%u), $rand or $rank", KV_BML_PARAMS);
            u32 n = 0;
            while (*p >= '0' && *p <= '9') n = n * 10 + (u32)(*p++ - '0');
            if (n > KV_BML_PARAMS) return fail(at, "$%u: at most %u params", n, KV_BML_PARAMS);
            emit(E_PARAM); emit(n - 1); push_one();
            return true;
        }
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            char* end = nullptr;
            f32 v = strtof(p, &end);
            if (end == p) return fail(at, "a number is malformed");
            p = end;
            u32 bits;
            memcpy(&bits, &v, sizeof bits);
            emit(E_CONST); emit(bits); push_one();
            return true;
        }
        return fail(at, *p ? "'%c' is not part of an expression" : "an expression ends early", *p);
    }
    bool unary() {
        space();
        if (*p == '-') { ++p; if (!unary()) return false; emit(E_NEG); return true; }
        if (*p == '+') { ++p; return unary(); }
        return primary();
    }
    bool product() {
        if (!unary()) return false;
        for (;;) {
            space();
            char c = *p;
            if (c != '*' && c != '/' && c != '%') return true;
            ++p;
            if (!unary()) return false;
            emit(c == '*' ? E_MUL : c == '/' ? E_DIV : E_MOD);
            --depth;
        }
    }
    bool sum() {
        if (!product()) return false;
        for (;;) {
            space();
            char c = *p;
            if (c != '+' && c != '-') return true;
            ++p;
            if (!product()) return false;
            emit(c == '+' ? E_ADD : E_SUB);
            --depth;
        }
    }
    // An element's text as an expression; KV_BML_NONE (and a failure) when it is not one.
    u32 expr(const XmlNode& n) {
        u32 start = lib.code.len;
        p = xml_text(n);
        at = &n;
        depth = max_depth = 0;
        dynamic = false;
        if (!sum()) return KV_BML_NONE;
        space();
        if (*p) { fail(at, "'%c' is not part of an expression", *p); return KV_BML_NONE; }
        if (max_depth > kBmlExprStack) { fail(at, "an expression nests too deeply"); return KV_BML_NONE; }
        emit(E_END);
        if (!dynamic) { // a constant: folded to one
            f32 v = bml_eval(lib.code.data, start, nullptr, 0, 0.0f, nullptr);
            lib.code.len = start;
            u32 bits;
            memcpy(&bits, &v, sizeof bits);
            emit(E_CONST); emit(bits); emit(E_END);
        }
        return start;
    }
    u32 child_expr(const XmlNode& parent, const char* name, bool required) {
        const XmlNode* c = xml_child(doc, parent, name);
        if (!c) { if (required) fail(&parent, "a <%s> is missing", name); return KV_BML_NONE; }
        return expr(*c);
    }

    // ---- types ------------------------------------------------------------------------------
    u32 dir_type(const XmlNode& n) {
        const char* t = xml_attr_or(doc, n, "type", "aim");
        if (!strcmp(t, "aim")) return DIR_AIM;
        if (!strcmp(t, "absolute")) return DIR_ABSOLUTE;
        if (!strcmp(t, "relative")) return DIR_RELATIVE;
        if (!strcmp(t, "sequence")) return DIR_SEQUENCE;
        fail(&n, "type \"%s\" is not aim, absolute, relative or sequence", t);
        return 0;
    }
    u32 speed_type(const XmlNode& n) {
        const char* t = xml_attr_or(doc, n, "type", "absolute");
        if (!strcmp(t, "absolute")) return SPEED_ABSOLUTE;
        if (!strcmp(t, "relative")) return SPEED_RELATIVE;
        if (!strcmp(t, "sequence")) return SPEED_SEQUENCE;
        fail(&n, "type \"%s\" is not absolute, relative or sequence", t);
        return 0;
    }
    // A direction or speed child: its type and expression, or none.
    void typed(const XmlNode& parent, const char* name, bool direction, u32& type, u32& e) {
        type = 0;
        e = KV_BML_NONE;
        const XmlNode* c = xml_child(doc, parent, name);
        if (!c) return;
        type = direction ? dir_type(*c) : speed_type(*c);
        e = expr(*c);
    }

    // ---- params -----------------------------------------------------------------------------
    // A reference's <param>s, compiled; their count, after the code offsets in `out`.
    u32 params(const XmlNode& ref, u32* out) {
        u32 n = 0;
        for (const XmlNode* c = xml_child(doc, ref, "param"); c; c = xml_next_named(doc, *c, "param")) {
            if (n == KV_BML_PARAMS) { fail(c, "at most %u params", KV_BML_PARAMS); return 0; }
            out[n++] = expr(*c);
        }
        return n;
    }
    void emit_call(u32 op, u32 first_word, u32 action, const XmlNode* ref) {
        lib.ops.push(op);
        if (op == OP_REPEAT) lib.ops.push(first_word);
        lib.ops.push(action);
        if (!ref) { lib.ops.push(KV_BML_NONE); return; }
        u32 e[KV_BML_PARAMS];
        u32 n = params(*ref, e);
        lib.ops.push(n);
        for (u32 i = 0; i < n; ++i) lib.ops.push(e[i]);
    }
    u32 ref_label(const XmlNode& ref, const Array<Label>& in, const char* what) {
        const char* label = xml_attr_or(doc, ref, "label", "");
        u32 id = find(in, label);
        if (id == KV_BML_NONE) fail(&ref, "no %s labelled \"%s\"", what, label);
        return id;
    }

    // ---- bullets ------------------------------------------------------------------------------
    // A bullet's own direction, speed and actions; an actionRef becomes a wrapper action that
    // calls it with its params, compiled from the queue like any action.
    u32 bullet(const XmlNode& n, u32 id = KV_BML_NONE) {
        if (id == KV_BML_NONE) { lib.bullets.push(BmlBullet{}); id = lib.bullets.len - 1; }
        BmlBullet b{};
        format(b.label, sizeof b.label, "%s", xml_attr_or(doc, n, "label", ""));
        typed(n, "direction", true, b.dir_type, b.dir_expr);
        typed(n, "speed", false, b.speed_type, b.speed_expr);
        for (const XmlNode* c = xml_child(doc, n); c && ok; c = xml_next_named(doc, *c)) {
            bool inline_action = !strcmp(c->name, "action"), ref = !strcmp(c->name, "actionRef");
            if (!inline_action && !ref) {
                if (strcmp(c->name, "direction") && strcmp(c->name, "speed")) fail(c, "a <bullet> holds direction, speed, action and actionRef");
                continue;
            }
            if (b.action_count == KV_BML_TRACKS) { fail(c, "a bullet runs at most %u actions", KV_BML_TRACKS); break; }
            u32 a = new_action();
            queue.push(Pending{a, c, inline_action ? Job::Action : Job::Ref});
            b.actions[b.action_count++] = a;
        }
        lib.bullets.data[id] = b;
        return id;
    }

    // ---- actions -----------------------------------------------------------------------------
    void fire(const XmlNode& n) {
        u32 dt, de, st, se;
        typed(n, "direction", true, dt, de);
        typed(n, "speed", false, st, se);
        const XmlNode* inline_bullet = xml_child(doc, n, "bullet");
        const XmlNode* ref = xml_child(doc, n, "bulletRef");
        if (!inline_bullet && !ref) { fail(&n, "a <fire> has a <bullet> or a <bulletRef>"); return; }
        u32 b = inline_bullet ? bullet(*inline_bullet) : ref_label(*ref, bullets, "bullet");
        u32 e[KV_BML_PARAMS];
        u32 np = ref ? params(*ref, e) : KV_BML_NONE;
        u32 words[] = {OP_FIRE, dt, de, st, se, b, np};
        for (u32 w : words) lib.ops.push(w);
        for (u32 i = 0; np != KV_BML_NONE && i < np; ++i) lib.ops.push(e[i]);
    }

    void body(const XmlNode& n) {
        for (const XmlNode* c = xml_child(doc, n); c && ok; c = xml_next_named(doc, *c)) {
            const char* name = c->name;
            if (!strcmp(name, "repeat")) {
                u32 times = child_expr(*c, "times", true);
                const XmlNode* in = xml_child(doc, *c, "action");
                const XmlNode* ref = xml_child(doc, *c, "actionRef");
                if (in) {
                    u32 a = new_action();
                    queue.push(Pending{a, in, Job::Action});
                    emit_call(OP_REPEAT, times, a, nullptr);
                } else if (ref) emit_call(OP_REPEAT, times, ref_label(*ref, actions, "action"), ref);
                else fail(c, "a <repeat> repeats an <action> or an <actionRef>");
            } else if (!strcmp(name, "fire")) fire(*c);
            else if (!strcmp(name, "fireRef")) emit_call(OP_CALL, 0, ref_label(*c, fires, "fire"), c);
            else if (!strcmp(name, "action")) {
                u32 a = new_action();
                queue.push(Pending{a, c, Job::Action});
                emit_call(OP_CALL, 0, a, nullptr);
            } else if (!strcmp(name, "actionRef")) emit_call(OP_CALL, 0, ref_label(*c, actions, "action"), c);
            else if (!strcmp(name, "wait")) { lib.ops.push(OP_WAIT); lib.ops.push(expr(*c)); }
            else if (!strcmp(name, "vanish")) lib.ops.push(OP_VANISH);
            else if (!strcmp(name, "changeDirection") || !strcmp(name, "changeSpeed")) {
                bool dir = name[6] == 'D';
                u32 t, e;
                typed(*c, dir ? "direction" : "speed", dir, t, e);
                if (e == KV_BML_NONE && ok) { fail(c, "a <%s> is missing", dir ? "direction" : "speed"); return; }
                u32 term = child_expr(*c, "term", true);
                u32 words[] = {dir ? (u32)OP_CHDIR : (u32)OP_CHSPEED, t, e, term};
                for (u32 w : words) lib.ops.push(w);
            } else if (!strcmp(name, "accel")) {
                u32 ht, he, vt, ve;
                typed(*c, "horizontal", false, ht, he);
                typed(*c, "vertical", false, vt, ve);
                u32 term = child_expr(*c, "term", true);
                u32 words[] = {OP_ACCEL, ht, he, vt, ve, term};
                for (u32 w : words) lib.ops.push(w);
            } else fail(c, "is not an action's element");
        }
        lib.ops.push(OP_END);
    }

    void compile(const Pending& job) {
        lib.actions.data[job.id] = lib.ops.len;
        switch (job.job) {
        case Job::Action: body(*job.node); break;
        case Job::Fire: fire(*job.node); lib.ops.push(OP_END); break;
        case Job::Ref: emit_call(OP_CALL, 0, ref_label(*job.node, actions, "action"), job.node); lib.ops.push(OP_END); break;
        }
    }

    // ---- the graph ---------------------------------------------------------------------------
    // Depth from each action, by walking its calls; 0 while being walked (a recursion).
    Array<u32> depth_of = array_in<u32>(mem_scratch());
    u32 walk(u32 a) {
        if (depth_of.data[a] == 0) return KV_BML_NONE;   // on the walk: a recursion
        if (depth_of.data[a] != KV_BML_NONE) return depth_of.data[a];
        depth_of.data[a] = 0;
        u32 deepest = 0;
        const u32* op = lib.ops.data + lib.actions.data[a];
        for (; *op != OP_END; op += bml_op_len(op)) {
            u32 child = op[0] == OP_REPEAT ? op[2] : op[0] == OP_CALL ? op[1] : KV_BML_NONE;
            if (child == KV_BML_NONE) continue;
            u32 d = walk(child);
            if (d == KV_BML_NONE) return KV_BML_NONE;
            if (d > deepest) deepest = d;
        }
        depth_of.data[a] = deepest + 1;
        return deepest + 1;
    }
    bool check(u32 first_action) {
        depth_of.resize_fill(lib.actions.len, KV_BML_NONE);
        for (u32 a = first_action; a < lib.actions.len; ++a) {
            u32 d = walk(a);
            if (d == KV_BML_NONE) { fail(&doc.root(), "an action calls itself, through references"); return false; }
            if (d > KV_BML_DEPTH) { fail(&doc.root(), "actions nest %u deep; a runner holds %u", d, KV_BML_DEPTH); return false; }
        }
        return true;
    }
};

} // namespace

u32 bml_compile(BmlLibrary& lib, const char* name, const char* text, usize len, char* why, usize why_cap) {
    if (why_cap) why[0] = 0;
    MemScope scope;
    XmlDoc doc;
    if (!xml_parse(text, len, mem_scratch(), doc)) {
        format(why, why_cap, "line %u, column %u: %s", doc.error_line, doc.error_col, doc.error);
        return KV_BML_NONE;
    }
    const XmlNode& root = doc.root();
    if (strcmp(root.name, "bulletml") != 0) { format(why, why_cap, "the root is <%s>, not <bulletml>", root.name); return KV_BML_NONE; }
    const char* type = xml_attr_or(doc, root, "type", "none");
    if (strcmp(type, "vertical") && strcmp(type, "none")) {
        format(why, why_cap, "type \"%s\": this library reads vertical documents", type);
        return KV_BML_NONE;
    }
    if (bml_find(lib, name) != KV_BML_NONE) { format(why, why_cap, "a pattern \"%s\" is already in the library", name); return KV_BML_NONE; }
    // Where the library was, to put it back on a failure.
    const u32 ops0 = lib.ops.len, actions0 = lib.actions.len, code0 = lib.code.len, bullets0 = lib.bullets.len;

    Compiler c(lib, doc, why, why_cap);
    BmlEntry entry{};
    format(entry.name, sizeof entry.name, "%s", name);
    // Labels first, so references go forward and back.
    for (const XmlNode* n = xml_child(doc, root); n && c.ok; n = xml_next_named(doc, *n)) {
        const char* label = xml_attr_or(doc, *n, "label", "");
        bool is_action = !strcmp(n->name, "action"), is_bullet = !strcmp(n->name, "bullet"), is_fire = !strcmp(n->name, "fire");
        if (!is_action && !is_bullet && !is_fire) { c.fail(n, "a <bulletml> holds action, bullet and fire"); break; }
        if (!label[0]) { c.fail(n, "a top-level <%s> has a label", n->name); break; }
        Array<Label>& in = is_action ? c.actions : is_bullet ? c.bullets : c.fires;
        if (c.find(in, label) != KV_BML_NONE) { c.fail(n, "\"%s\" is labelled twice", label); break; }
        u32 id;
        if (is_bullet) { lib.bullets.push(BmlBullet{}); id = lib.bullets.len - 1; }
        else id = c.new_action();
        in.push(Label{label, id, n});
        if (is_action && !strncmp(label, "top", 3)) {
            if (entry.count == KV_BML_TRACKS) { c.fail(n, "at most %u top actions", KV_BML_TRACKS); break; }
            entry.actions[entry.count++] = id;
        }
    }
    if (c.ok && entry.count == 0) c.fail(&root, "no action is labelled top");
    for (const Label& b : c.bullets) if (c.ok) c.bullet(*b.node, b.id);
    for (const Label& a : c.actions) c.queue.push(Pending{a.id, a.node, Job::Action});
    for (const Label& f : c.fires) c.queue.push(Pending{f.id, f.node, Job::Fire});
    for (u32 i = 0; i < c.queue.len && c.ok; ++i) { Pending job = c.queue[i]; c.compile(job); } // the queue grows as it goes: a copy
    if (c.ok) c.check(actions0);
    if (!c.ok) {
        lib.ops.len = ops0; lib.actions.len = actions0; lib.code.len = code0; lib.bullets.len = bullets0;
        return KV_BML_NONE;
    }
    lib.entries.push(entry);
    return lib.entries.len - 1;
}

u32 bml_find(const BmlLibrary& lib, const char* name) {
    for (u32 i = 0; i < lib.entries.len; ++i)
        if (!strcmp(lib.entries.data[i].name, name)) return i;
    return KV_BML_NONE;
}

void bml_free(BmlLibrary& lib) {
    lib.ops.free(); lib.actions.free(); lib.code.free(); lib.bullets.free(); lib.entries.free();
}

bool bml_bullet_actions(const BmlLibrary& lib, u32 bullet) { return bullet < lib.bullets.len && lib.bullets.data[bullet].action_count > 0; }
const char* bml_bullet_label(const BmlLibrary& lib, u32 bullet) { return bullet < lib.bullets.len ? lib.bullets.data[bullet].label : ""; }

} // namespace kv
