#include "korvid/core/json.h"
#include "korvid/core/utf8.h"
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

struct Parser {
    const char* text;
    usize       len;
    usize       at;
    Allocator*  alloc;
    JsonValue*  nodes;
    u32         count, cap;
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

    // Nodes live in one array that doubles; indices stay valid, pointers do not until the end.
    u32 node() {
        if (count == cap) {
            u32 ncap = cap ? cap * 2 : 32;
            JsonValue* n = alloc_items<JsonValue>(alloc, ncap);
            if (!n) return ~0u;
            if (count) memcpy(n, nodes, sizeof(JsonValue) * count);
            if (nodes) free_items(alloc, nodes, cap);
            nodes = n;
            cap   = ncap;
        }
        nodes[count] = JsonValue{};
        return count++;
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

    // At the opening quote. Produces an unescaped, NUL-terminated copy.
    bool string(const char** out, u32* out_len) {
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
        char* s = (char*)alloc_in(alloc, end - start + 1, 1);
        if (!s) return fail("out of memory");
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
        *out = s;
        *out_len = n;
        return true;
    }

    bool number(JsonValue& v) {
        f64* out = &v.number;
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
        *out = strtod(buf, nullptr);
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
            if (fits) { v.integer = true; v.bits = negative ? (u64)0 - mag : mag; }
        }
        return true;
    }

    bool literal(const char* word, usize n) {
        if (at + n > len || memcmp(text + at, word, n) != 0) return fail("unexpected token");
        at += n;
        return true;
    }

    // Parses one value into node `idx`.
    bool value(u32 idx) {
        skip_space();
        if (!more()) return fail("unexpected end of text");
        if (!nodes[idx].line) nodes[idx].line = line;
        char c = peek();
        if (c == '{' || c == '[') {
            if (++depth > KV_JSON_MAX_DEPTH) return fail("nesting too deep");
            bool object = c == '{';
            nodes[idx].type = object ? JsonType::Object : JsonType::Array;
            ++at;
            u32 last = 0;
            for (;;) {
                skip_space();
                if (peek() == (object ? '}' : ']')) { ++at; break; }
                if (nodes[idx].count) {
                    if (peek() != ',') return fail(object ? "expected ',' or '}'" : "expected ',' or ']'");
                    ++at;
                    skip_space();
                    if (peek() == (object ? '}' : ']')) return fail("trailing comma");
                }
                u32 child = node();
                if (child == ~0u) return fail("out of memory");
                if (object) {
                    if (peek() != '"') return fail("expected a member name");
                    nodes[child].line = line;
                    const char* key; u32 klen;
                    if (!string(&key, &klen)) return false;
                    nodes[child].key = key;
                    skip_space();
                    if (peek() != ':') return fail("expected ':'");
                    ++at;
                }
                if (!value(child)) return false;
                if (last) nodes[last].next = child; else nodes[idx].first = child;
                last = child;
                ++nodes[idx].count;
            }
            --depth;
            return true;
        }
        if (c == '"') { nodes[idx].type = JsonType::String; return string(&nodes[idx].str, &nodes[idx].len); }
        if (c == '-' || (c >= '0' && c <= '9')) { nodes[idx].type = JsonType::Number; return number(nodes[idx]); }
        if (c == 't') { nodes[idx].type = JsonType::Bool; nodes[idx].number = 1.0; return literal("true", 4); }
        if (c == 'f') { nodes[idx].type = JsonType::Bool; nodes[idx].number = 0.0; return literal("false", 5); }
        if (c == 'n') { nodes[idx].type = JsonType::Null; return literal("null", 4); }
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
    u32 root = p.node();
    bool ok = root != ~0u && p.value(root);
    if (ok) {
        p.skip_space();
        if (p.more()) ok = p.fail("text after the value");
    }
    doc.nodes    = p.nodes;
    doc.count    = p.count;
    doc.error    = ok ? nullptr : (p.error ? p.error : "parse error");
    doc.error_at = p.error_at;
    doc.error_line = ok ? 0 : p.error_line;
    return ok;
}

const JsonValue* json_first(const JsonDoc& doc, const JsonValue& v) {
    return (v.is_array() || v.is_object()) && v.first ? &doc.nodes[v.first] : nullptr;
}

const JsonValue* json_next(const JsonDoc& doc, const JsonValue& v) {
    return v.next ? &doc.nodes[v.next] : nullptr;
}

const JsonValue* json_get(const JsonDoc& doc, const JsonValue& object, const char* key) {
    if (!object.is_object() || !key) return nullptr;
    for (const JsonValue* m = json_first(doc, object); m; m = json_next(doc, *m))
        if (m->key && strcmp(m->key, key) == 0) return m;
    return nullptr;
}

const JsonValue* json_at(const JsonDoc& doc, const JsonValue& array, u32 index) {
    if (!array.is_array() || index >= array.count) return nullptr;
    const JsonValue* v = json_first(doc, array);
    for (u32 i = 0; v && i < index; ++i) v = json_next(doc, *v);
    return v;
}

f64 json_number(const JsonValue* v, f64 dflt) { return v && v->is_number() ? v->number : dflt; }
u64 json_u64(const JsonValue* v, u64 dflt) {
    if (!v || !v->is_number()) return dflt;
    if (v->integer) return v->number < 0.0 ? 0 : v->bits;
    if (!(v->number > 0.0)) return 0;
    return v->number >= 18446744073709551616.0 ? ~0ull : (u64)v->number;
}

i64 json_i64(const JsonValue* v, i64 dflt) {
    if (!v || !v->is_number()) return dflt;
    if (v->integer) {
        if (v->number >= 0.0 && v->bits > 0x7FFFFFFFFFFFFFFFull) return 0x7FFFFFFFFFFFFFFFll;
        return (i64)v->bits;
    }
    if (v->number != v->number) return 0;
    if (v->number >= 9223372036854775807.0) return 0x7FFFFFFFFFFFFFFFll;
    if (v->number <= -9223372036854775808.0) return (-0x7FFFFFFFFFFFFFFFll - 1);
    return (i64)v->number;
}

bool json_bool(const JsonValue* v, bool dflt) { return v && v->is_bool() ? v->number != 0.0 : dflt; }
const char* json_string(const JsonValue* v, const char* dflt) { return v && v->is_string() ? v->str : dflt; }

u32 json_numbers(const JsonDoc& doc, const JsonValue* array, f32* out, u32 cap) {
    if (!array || !array->is_array()) return 0;
    u32 n = 0;
    for (const JsonValue* v = json_first(doc, *array); v; v = json_next(doc, *v), ++n)
        if (n < cap) out[n] = v->is_number() ? (f32)v->number : 0.0f;
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
