#include "korvid/core/json.h"
#include "korvid/core/utf8.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

static_assert(sizeof(JsonValue) == 24, "JsonValue is meant to stay small");

namespace {

struct Parser {
    const char* text;
    usize       len;
    usize       at;
    Allocator*  alloc;
    // One block of `cap` nodes from the caller's allocator, counted before parsing (see
    // count_nodes). The document fills it from the front, slot 0 kept for the root; the
    // children read so far of every container still open are a stack at the back, growing
    // down, and move to the front as one run when their parent closes. A node is in one
    // or the other, so the two never meet.
    JsonValue*  nodes;
    u32         cap;
    u32         out_n, open_n;
    const char* error;
    u32         error_at;
    u32         error_line;
    u32         depth;
    u32         line;
    u32         flags;

    bool fail(const char* why) {
        if (!error) { error = why; error_at = (u32)at; error_line = line; }
        return false;
    }
    bool more() const { return at < len; }
    char peek() const { return at < len ? text[at] : 0; }
    void skip_space() {
        // The only place a newline is passed: a string cannot hold a raw one.
        for (;;) {
            while (at < len && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) { if (text[at] == '\n') ++line; ++at; }
            if (!(flags & KV_JSON_LINE_COMMENTS) || at + 1 >= len || text[at] != '/' || text[at + 1] != '/') break;
            while (at < len && text[at] != '\n') ++at;
        }
    }

    // Where white space (and comments, when allowed) starting at `i` ends. Counts no lines.
    usize past_space(usize i) const {
        for (;;) {
            while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
            if (!(flags & KV_JSON_LINE_COMMENTS) || i + 1 >= len || text[i] != '/' || text[i + 1] != '/') return i;
            while (i < len && text[i] != '\n') ++i;
        }
    }

    // How many nodes the text can make: the root, one for every comma, one for every
    // container that is not empty. Exact for a text that parses, and never too few for one
    // that does not, since the parser makes a child only after an opening bracket that is
    // not closed at once, or after a comma. Strings and comments are passed over as the
    // parser passes over them.
    u32 count_nodes() const {
        u32 n = 1;
        for (usize i = 0; i < len;) {
            char c = text[i];
            if (c == '"') {
                for (++i; i < len && text[i] != '"';) i += text[i] == '\\' ? 2 : 1;
                ++i;
                continue;
            }
            if (c == '/' && (flags & KV_JSON_LINE_COMMENTS) && i + 1 < len && text[i + 1] == '/') {
                while (i < len && text[i] != '\n') ++i;
                continue;
            }
            if (c == ',') ++n;
            else if (c == '[' || c == '{') {
                usize j = past_space(i + 1);
                if (!(j < len && text[j] == (c == '[' ? ']' : '}'))) ++n;
            }
            ++i;
        }
        return n;
    }

    static void put_utf8(char* out, u32& n, u32 cp) { n += utf8_encode(cp, out + n); }

    bool hex4(u32& out) {
        if (at + 4 > len) return fail("truncated \\u escape");
        out = 0;
        for (u32 i = 0; i < 4; ++i) {
            char c = text[at++];
            u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 16;
            if (d == 16) { --at; return fail("bad hex digit in \\u escape"); }
            out = out * 16 + d;
        }
        return true;
    }

    // At the opening quote. Produces an unescaped, NUL-terminated copy; a value's (not a
    // member name's) has its length in the four bytes before it.
    bool string(const char** out, bool with_len) {
        ++at;
        usize start = at;
        // First pass: find the end and the worst-case size (escapes never grow the text).
        while (at < len && text[at] != '"') {
            if ((u8)text[at] < 0x20) return fail("control character in string");
            if (text[at] == '\\') { if (at + 1 >= len) return fail("truncated escape"); at += 2; }
            else ++at;
        }
        if (at >= len) { at = start - 1; return fail("unterminated string"); }
        usize end = at++;
        u32 head = with_len ? (u32)sizeof(u32) : 0;
        char* s = (char*)alloc_in(alloc, head + (end - start) + 1, with_len ? (u32)alignof(u32) : 1);
        if (!s) return fail("out of memory");
        s += head;
        u32 n = 0;
        for (usize i = start; i < end;) {
            char c = text[i];
            if (c != '\\') { s[n++] = c; ++i; continue; }
            char e = text[i + 1];
            i += 2;
            switch (e) {
            case '"': s[n++] = '"'; break;
            case '\\': s[n++] = '\\'; break;
            case '/': s[n++] = '/'; break;
            case 'b': s[n++] = '\b'; break;
            case 'f': s[n++] = '\f'; break;
            case 'n': s[n++] = '\n'; break;
            case 'r': s[n++] = '\r'; break;
            case 't': s[n++] = '\t'; break;
            case 'u': {
                usize save = at; at = i;
                u32 cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) { // a surrogate pair
                    if (at + 6 <= len && text[at] == '\\' && text[at + 1] == 'u') {
                        at += 2;
                        u32 lo;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else return fail("lone high surrogate");
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                i = at; at = save;
                put_utf8(s, n, cp);
                break;
            }
            default: at = i - 2; return fail("unknown escape");
            }
        }
        s[n] = 0;
        if (with_len) memcpy(s - sizeof n, &n, sizeof n);
        *out = s;
        return true;
    }

    bool number(JsonValue& v) {
        usize start = at;
        bool negative = peek() == '-';
        if (negative) ++at;
        if (!(peek() >= '0' && peek() <= '9')) return fail("bad number");
        if (peek() == '0') ++at;
        else while (peek() >= '0' && peek() <= '9') ++at;
        if (peek() == '.') { ++at; if (!(peek() >= '0' && peek() <= '9')) return fail("bad fraction"); while (peek() >= '0' && peek() <= '9') ++at; }
        if (peek() == 'e' || peek() == 'E') {
            ++at;
            if (peek() == '+' || peek() == '-') ++at;
            if (!(peek() >= '0' && peek() <= '9')) return fail("bad exponent");
            while (peek() >= '0' && peek() <= '9') ++at;
        }
        char buf[64];
        usize n = at - start;
        if (n >= sizeof buf) return fail("number too long");
        memcpy(buf, text + start, n);
        buf[n] = 0;
        v.as.num = strtod(buf, nullptr);
        // An integer literal keeps its exact value when it fits: u64, or i64 when negative.
        bool plain = true;
        for (usize i = negative ? 1 : 0; i < n; ++i) if (buf[i] < '0' || buf[i] > '9') { plain = false; break; }
        if (plain) {
            u64 mag = 0;
            bool fits = true;
            for (usize i = negative ? 1 : 0; i < n && fits; ++i) {
                u64 d = (u64)(buf[i] - '0');
                if (mag > (~0ull - d) / 10) fits = false;
                else mag = mag * 10 + d;
            }
            if (fits && negative && mag > 0x8000000000000000ull) fits = false;
            if (fits) { v.flags |= negative ? KV_JSON_VALUE_INTEGER | KV_JSON_VALUE_NEGATIVE : KV_JSON_VALUE_INTEGER; v.as.bits = negative ? (u64)0 - mag : mag; }
        }
        return true;
    }

    bool literal(const char* word, usize n) {
        if (at + n > len || memcmp(text + at, word, n) != 0) return fail("unexpected token");
        at += n;
        return true;
    }

    // Parses one value into `v`, which is the caller's own: nothing points into the arrays.
    bool value(JsonValue& v) {
        skip_space();
        if (!more()) return fail("unexpected end of text");
        if (!v.line) v.line = line;
        char c = peek();
        if (c == '{' || c == '[') {
            if (++depth > KV_JSON_MAX_DEPTH) return fail("nesting too deep");
            bool object = c == '{';
            v.type = object ? JsonType::Object : JsonType::Array;
            ++at;
            u32 base = open_n;
            for (;;) {
                skip_space();
                if (peek() == (object ? '}' : ']')) { ++at; break; }
                if (open_n > base) {
                    if (peek() != ',') return fail(object ? "expected ',' or '}'" : "expected ',' or ']'");
                    ++at;
                    skip_space();
                    if (peek() == (object ? '}' : ']')) return fail("trailing comma");
                }
                JsonValue child{};
                if (object) {
                    if (peek() != '"') return fail("expected a member name");
                    child.line = line;
                    if (!string(&child.key, false)) return false;
                    skip_space();
                    if (peek() != ':') return fail("expected ':'");
                    ++at;
                }
                if (!value(child)) return false;
                if (out_n + open_n >= cap) return fail("more values than were counted"); // not reached: see count_nodes
                nodes[cap - ++open_n] = child;
            }
            // The run sits at the top of the stack, last child first: turn it round, then
            // move it down to the front (the two may overlap).
            u32 n = open_n - base;
            v.as.kids.first = n ? out_n : 0;
            v.as.kids.count = n;
            if (n) {
                JsonValue* run = nodes + (cap - open_n);
                for (u32 i = 0, j = n - 1; i < j; ++i, --j) { JsonValue t = run[i]; run[i] = run[j]; run[j] = t; }
                run[n - 1].flags |= KV_JSON_VALUE_LAST;
                memmove(nodes + out_n, run, sizeof(JsonValue) * n);
                out_n += n;
                open_n = base;
            }
            --depth;
            return true;
        }
        if (c == '"') { v.type = JsonType::String; return string(&v.as.text, true); }
        if (c == '-' || (c >= '0' && c <= '9')) { v.type = JsonType::Number; return number(v); }
        if (c == 't') { v.type = JsonType::Bool; v.as.num = 1.0; return literal("true", 4); }
        if (c == 'f') { v.type = JsonType::Bool; v.as.num = 0.0; return literal("false", 5); }
        if (c == 'n') { v.type = JsonType::Null; return literal("null", 4); }
        return fail("unexpected character");
    }
};

} // namespace

bool json_parse(const char* text, usize len, Allocator* alloc, JsonDoc& doc, u32 flags) {
    Parser p{};
    p.text  = text;
    p.len   = len;
    p.alloc = alloc ? alloc : mem_general();
    p.line  = 1;
    p.flags = flags;
    doc = JsonDoc{};
    p.cap   = p.count_nodes();
    p.nodes = alloc_items<JsonValue>(p.alloc, p.cap);
    p.out_n = 1;
    JsonValue root{};
    bool ok = p.nodes ? p.value(root) : p.fail("out of memory");
    if (ok) {
        p.skip_space();
        if (p.more()) ok = p.fail("text after the value");
    }
    // A failed parse leaves a root to ask questions of, and nothing under it: a block of
    // one, so that whoever frees doc.nodes by doc.count frees what was allocated.
    if (!ok) {
        free_items(p.alloc, p.nodes, p.cap);
        p.nodes = alloc_items<JsonValue>(p.alloc, 1);
        p.cap = p.out_n = 1;
        root = JsonValue{};
    }
    if (p.nodes) {
        root.flags |= KV_JSON_VALUE_LAST;
        p.nodes[0] = root;
        doc.nodes = p.nodes;
        doc.count = p.out_n;
    }
    doc.error    = ok ? nullptr : (p.error ? p.error : "parse error");
    doc.error_at = p.error_at;
    doc.error_line = ok ? 0 : p.error_line;
    return ok;
}

const JsonValue* json_first(const JsonDoc& doc, const JsonValue& v) {
    return v.count() ? &doc.nodes[v.as.kids.first] : nullptr;
}

// Siblings are consecutive, and the last one says so.
const JsonValue* json_next(const JsonDoc&, const JsonValue& v) {
    return v.flags & KV_JSON_VALUE_LAST ? nullptr : &v + 1;
}

const JsonValue* json_get(const JsonDoc& doc, const JsonValue& object, const char* key) {
    if (!object.is_object() || !key) return nullptr;
    const JsonValue* m = json_first(doc, object);
    for (u32 i = 0; i < object.as.kids.count; ++i, ++m)
        if (m->key && strcmp(m->key, key) == 0) return m;
    return nullptr;
}

const JsonValue* json_at(const JsonDoc& doc, const JsonValue& array, u32 index) {
    if (!array.is_array() || index >= array.as.kids.count) return nullptr;
    return &doc.nodes[array.as.kids.first + index];
}

f64 json_number(const JsonValue* v, f64 dflt) { return v && v->is_number() ? v->number() : dflt; }
u64 json_u64(const JsonValue* v, u64 dflt) {
    if (!v || !v->is_number()) return dflt;
    if (v->integer()) return v->flags & KV_JSON_VALUE_NEGATIVE ? 0 : v->as.bits;
    f64 d = v->as.num;
    if (!(d > 0.0)) return 0;
    return d >= 18446744073709551616.0 ? ~0ull : (u64)d;
}

i64 json_i64(const JsonValue* v, i64 dflt) {
    if (!v || !v->is_number()) return dflt;
    if (v->integer()) {
        if (!(v->flags & KV_JSON_VALUE_NEGATIVE) && v->as.bits > 0x7FFFFFFFFFFFFFFFull) return 0x7FFFFFFFFFFFFFFFll;
        return (i64)v->as.bits;
    }
    f64 d = v->as.num;
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return 0x7FFFFFFFFFFFFFFFll;
    if (d <= -9223372036854775808.0) return (-0x7FFFFFFFFFFFFFFFll - 1);
    return (i64)d;
}

bool json_bool(const JsonValue* v, bool dflt) { return v && v->is_bool() ? v->as.num != 0.0 : dflt; }
const char* json_string(const JsonValue* v, const char* dflt) { return v && v->is_string() ? v->as.text : dflt; }

u32 json_numbers(const JsonDoc& doc, const JsonValue* array, f32* out, u32 cap) {
    if (!array || !array->is_array()) return 0;
    u32 n = 0;
    for (const JsonValue* v = json_first(doc, *array); v; v = json_next(doc, *v), ++n)
        if (n < cap) out[n] = v->is_number() ? (f32)v->number() : 0.0f;
    return n;
}

const char* json_type_name(JsonType t) {
    switch (t) {
    case JsonType::Null:   return "null";
    case JsonType::Bool:   return "bool";
    case JsonType::Number: return "number";
    case JsonType::String: return "string";
    case JsonType::Array:  return "array";
    case JsonType::Object: return "object";
    }
    return "?";
}

} // namespace kv
