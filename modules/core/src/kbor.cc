#include "korvid/core/kbor.h"
#include "korvid/core/hash.h"
#include "korvid/core/map.h"
#include "korvid/core/utf8.h"
#include <string.h>

namespace kv {

namespace {

// A key's text, for the maps that find a name already met. Points at bytes someone else
// keeps for as long as the map lives.
struct StrKey {
    const char* p;
    u32         n;
    bool operator==(const StrKey& o) const { return n == o.n && memcmp(p, o.p, n) == 0; }
};
u64 hash_key(const StrKey& k) { return hash_bytes(k.p, k.n); }

constexpr u8  k_break = 0xFF;
constexpr u32 k_u32_max = 0xFFFFFFFFu;

// Half precision to single, bit for bit (a NaN keeps its payload).
f32 half_to_f32(u32 h) {
    u32 sign = (h & 0x8000u) << 16, exp = (h >> 10) & 31, mant = h & 1023;
    u32 bits;
    if (exp == 31) bits = sign | 0x7F800000u | mant << 13;
    else if (exp == 0) {
        if (!mant) bits = sign;
        else { // subnormal: shift the leading 1 up to where the implicit bit goes
            u32 e = 113;
            while (!(mant & 0x400)) { mant <<= 1; --e; }
            bits = sign | e << 23 | (mant & 1023) << 13;
        }
    } else bits = sign | (exp + 112) << 23 | mant << 13;
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

struct Reader {
    const u8*   p;
    usize       size;
    usize       at = 0;
    Allocator*  alloc;
    const char* error = nullptr;
    usize       error_at = 0;
    usize       item_at = 0;   // where the head being read began
    bool        summarized = false;

    // The limits: the summary's, or what the count found. And what the parse has used.
    KborSummary cap, used;
    u64         index_keys = 0;
    bool        indefinite = false;

    JsonValue*   nodes = nullptr;
    u32          node_cap = 0, out_n = 0;
    const char** table = nullptr;
    u32          table_cap = 0, table_len = 0;
    char*        strs = nullptr;
    usize        str_cap = 0, str_used = 0;

    // Child counts of the indefinite-length containers, in the order they open: the count
    // finds them, the parse takes them.
    Array<u32> indef;
    u32        indef_next = 0;
    // Without a summary, the names met so far, so each is stored once.
    Map<StrKey, const char*> names;
    u64        stored_names = 0;

    bool fail(const char* why) {
        if (!error) { error = why; error_at = item_at; }
        return false;
    }
    usize left() const { return size - at; }

    // One item head: the major type, the additional information, and its argument (the
    // value, the length, or a float's bits; 0 for an indefinite length).
    bool head(u8& major, u8& info, u64& arg) {
        item_at = at;
        if (at >= size) return fail("truncated");
        u8 b = p[at++];
        major = b >> 5;
        info = b & 31;
        if (info < 24) { arg = info; return true; }
        if (info == 31) {
            arg = 0;
            if (major == 0 || major == 1 || major == 6) return fail("an indefinite length on an integer or a tag");
            return true;
        }
        if (info > 27) return fail("a reserved additional information value");
        u32 n = 1u << (info - 24);
        if (left() < n) { at = size; return fail("truncated"); }
        arg = 0;
        for (u32 i = 0; i < n; ++i) arg = arg << 8 | p[at++];
        return true;
    }

    // ---- the count ------------------------------------------------------------------------
    // A walk over the heads, skipping string bytes by their lengths: sizes everything the
    // parse allocates, and gives each indefinite container its count. What it checks is
    // what it must to walk; the parse checks the rest.

    bool count_key() {
        u8 major, info;
        u64 arg;
        if (!head(major, info, arg)) return false;
        if (major == 0) return true;
        if (major != 3) return fail("a map key that is neither text nor an index");
        if (info == 31) return fail("an indefinite-length string");
        if (arg > left()) return fail("a string past the end");
        at += (usize)arg;
        ++cap.keys;
        cap.bytes += arg;
        return true;
    }

    bool count_value(u32 depth) {
        ++cap.nodes;
        u8 major, info;
        u64 arg;
        if (!head(major, info, arg)) return false;
        switch (major) {
        case 0: case 1: return true;
        case 2: case 3:
            if (info == 31) return fail("an indefinite-length string");
            if (arg > left()) return fail("a string past the end");
            at += (usize)arg;
            ++cap.strings;
            cap.bytes += arg;
            return true;
        case 4: case 5: {
            bool map = major == 5;
            if (depth >= KV_JSON_MAX_DEPTH) return fail("nesting too deep");
            if (info == 31) {
                u32 slot = indef.len;
                indef.push(0);
                u64 n = 0;
                for (;;) {
                    if (at >= size) { item_at = at; return fail("truncated"); }
                    if (p[at] == k_break) { ++at; break; }
                    if (map && !count_key()) return false;
                    if (!count_value(depth + 1)) return false;
                    ++n;
                }
                if (n > k_u32_max) return fail("a container too large");
                indef[slot] = (u32)n;
                return true;
            }
            if (arg > left() / (map ? 2 : 1)) return fail("a length past the end");
            for (u64 i = 0; i < arg; ++i) {
                if (map && !count_key()) return false;
                if (!count_value(depth + 1)) return false;
            }
            return true;
        }
        case 6: return fail("a tag inside the document");
        default:
            if (info == 20 || info == 21 || info == 22 || (info >= 25 && info <= 27)) return true;
            if (info == 31) return fail("a break outside an indefinite-length container");
            return fail(info == 23 ? "undefined, which JSON cannot say" : "a simple value JSON cannot say");
        }
    }

    // ---- the parse ------------------------------------------------------------------------

    // A string's copy in the block: a value's with its u32 length before it, a key's not,
    // both with a NUL after. The block was sized for all of them.
    bool store(u64 n, bool value, const char** out) {
        if (n > left()) return fail("a string past the end");
        if (used.bytes + n > cap.bytes) return fail("more string bytes than the summary says");
        used.bytes += n;
        usize need = (value ? sizeof(u32) : 0) + (usize)n + 1;
        if (str_cap - str_used < need) return fail("more string bytes than the summary says"); // not reached: the counts bound it
        char* s = strs + str_used;
        str_used += need;
        if (value) { u32 len = (u32)n; memcpy(s, &len, sizeof len); s += sizeof len; }
        memcpy(s, p + at, (usize)n);
        s[n] = 0;
        at += (usize)n;
        *out = s;
        return true;
    }

    bool key(JsonValue& c) {
        u8 major, info;
        u64 arg;
        if (!head(major, info, arg)) return false;
        if (major == 0) {
            if (arg >= table_len) return fail("a key index past the table");
            c.key = table[arg];
            ++index_keys;
            return true;
        }
        if (major != 3) return fail("a map key that is neither text nor an index");
        if (info == 31) return fail("an indefinite-length string");
        if (arg > left()) return fail("a string past the end");
        if (table_len >= table_cap) return fail("more keys than the summary says");
        const char* text = (const char*)p + at;
        if (!utf8_valid(text, (usize)arg)) return fail("a key that is not UTF-8");
        if (memchr(text, 0, (usize)arg)) return fail("a key holding a NUL");
        ++used.keys;
        const char* name = nullptr;
        if (!summarized) {
            // A file with no summary may say a name in full every time: keep one copy.
            bool inserted = false;
            const char** found = names.get_or_insert(StrKey{text, (u32)arg}, nullptr, &inserted);
            if (!inserted) {
                if (used.bytes + arg > cap.bytes) return fail("more string bytes than the summary says");
                used.bytes += arg;
                at += (usize)arg;
                name = *found;
            } else {
                if (!store(arg, false, &name)) return false;
                *found = name;
                ++stored_names;
            }
        } else {
            // Normalized: every name in text is new, by the format's promise.
            if (!store(arg, false, &name)) return false;
            ++stored_names;
        }
        table[table_len++] = name;
        c.key = name;
        return true;
    }

    bool value(JsonValue& v, u32 depth) {
        u8 major, info;
        u64 arg;
        if (!head(major, info, arg)) return false;
        switch (major) {
        case 0:
            v.type = JsonType::Number;
            v.flags |= KV_JSON_VALUE_INTEGER;
            v.as.bits = arg;
            return true;
        case 1:
            if (arg > 0x7FFFFFFFFFFFFFFFull) return fail("an integer below -2^63");
            v.type = JsonType::Number;
            v.flags |= KV_JSON_VALUE_INTEGER | KV_JSON_VALUE_NEGATIVE;
            v.as.bits = ~arg; // -1 - arg
            return true;
        case 2: case 3: {
            if (info == 31) return fail("an indefinite-length string");
            if (used.strings >= cap.strings) return fail("more strings than the summary says");
            ++used.strings;
            if (major == 3 && arg <= left() && !utf8_valid((const char*)p + at, (usize)arg)) return fail("a string that is not UTF-8");
            v.type = major == 2 ? JsonType::Bytes : JsonType::String;
            return store(arg, true, &v.as.text);
        }
        case 4: case 5: {
            bool map = major == 5;
            if (depth >= KV_JSON_MAX_DEPTH) return fail("nesting too deep");
            u64 n;
            if (info == 31) {
                if (summarized) return fail("an indefinite length in a file with a summary");
                if (indef_next >= indef.len) return fail("an indefinite length the count did not see"); // not reached
                n = indef[indef_next++];
                indefinite = true;
            } else {
                if (arg > left() / (map ? 2 : 1)) return fail("a length past the end");
                n = arg;
            }
            if (n > node_cap - out_n) return fail("more values than the summary says");
            v.type = map ? JsonType::Object : JsonType::Array;
            u32 base = out_n;
            out_n += (u32)n;
            v.as.kids.first = n ? base : 0;
            v.as.kids.count = (u32)n;
            for (u32 i = 0; i < (u32)n; ++i) {
                JsonValue& c = nodes[base + i];
                c = JsonValue{};
                if (map && !key(c)) return false;
                if (!value(c, depth + 1)) return false;
            }
            if (n) nodes[base + n - 1].flags |= KV_JSON_VALUE_LAST;
            if (info == 31) {
                item_at = at;
                if (at >= size || p[at] != k_break) return fail("an indefinite-length container without its break");
                ++at;
            }
            return true;
        }
        case 6: return fail("a tag inside the document");
        default:
            switch (info) {
            case 20: v.type = JsonType::Bool; v.as.num = 0.0; return true;
            case 21: v.type = JsonType::Bool; v.as.num = 1.0; return true;
            case 22: v.type = JsonType::Null; return true;
            case 25: v.type = JsonType::Number; v.as.num = (f64)half_to_f32((u32)arg); return true;
            case 26: {
                u32 b = (u32)arg;
                f32 f;
                memcpy(&f, &b, sizeof f);
                v.type = JsonType::Number;
                v.as.num = (f64)f;
                return true;
            }
            case 27:
                v.type = JsonType::Number;
                memcpy(&v.as.num, &arg, sizeof arg);
                return true;
            case 31: return fail("a break outside an indefinite-length container");
            case 23: return fail("undefined, which JSON cannot say");
            default: return fail("a simple value JSON cannot say");
            }
        }
    }

    // The tag, the pair's head and the summary, when the file starts with a tag.
    bool summary() {
        u8 major, info;
        u64 arg;
        if (!head(major, info, arg)) return false;
        if (arg != KV_KBOR_SUMMARY_TAG) return fail("a tag that is not the summary's");
        if (!head(major, info, arg)) return false;
        if (major != 4 || info == 31 || arg != 2) return fail("the summary tag around something not a pair");
        if (!head(major, info, arg)) return false;
        if (major != 4 || info == 31) return fail("a summary that is not an array");
        if (arg < 4) return fail("a summary with fewer than four counts");
        if (arg > left()) return fail("a length past the end");
        u64 counts[4] = {};
        for (u64 i = 0; i < arg; ++i) {
            u64 c;
            if (!head(major, info, c)) return false;
            if (major != 0) return fail("a summary count that is not an unsigned integer");
            if (i < 4) counts[i] = c; // later counts are for later readers
        }
        cap = KborSummary{counts[0], counts[1], counts[2], counts[3]};
        // Every value, key and string byte takes at least a byte of what is left, so a
        // count past that is wrong, and allocating for it is not done.
        item_at = at;
        if (!cap.nodes) return fail("a summary of no values");
        if (cap.nodes > left() || cap.keys > left() || cap.strings > left() || cap.bytes > left())
            return fail("a summary larger than the file");
        summarized = true;
        return true;
    }
};

} // namespace

bool kbor_parse(const void* data, usize size, Allocator* alloc, JsonDoc& doc, KborInfo* info) {
    Reader r;
    r.p = (const u8*)data;
    r.size = data ? size : 0;
    r.alloc = alloc ? alloc : mem_general();
    r.indef.alloc = mem_general();
    r.names.init(mem_general());
    doc = JsonDoc{};

    bool ok = true;
    usize doc_at = 0;
    if (r.size && (r.p[0] >> 5) == 6) {
        ok = r.summary();
        doc_at = r.at;
    } else {
        ok = r.count_value(0);
        if (ok && r.at != r.size) { r.item_at = r.at; ok = r.fail("bytes after the document"); }
        if (ok && (r.cap.nodes > k_u32_max || r.cap.keys > k_u32_max || r.cap.strings > k_u32_max)) ok = r.fail("a document too large");
        r.at = 0;
    }

    JsonValue root{};
    if (ok) {
        r.node_cap = (u32)r.cap.nodes;
        r.table_cap = (u32)r.cap.keys;
        r.str_cap = (usize)(r.cap.bytes + r.cap.strings * (sizeof(u32) + 1) + r.cap.keys);
        r.nodes = alloc_items<JsonValue>(r.alloc, r.node_cap);
        r.table = r.table_cap ? alloc_items<const char*>(r.alloc, r.table_cap) : nullptr;
        r.strs = r.str_cap ? (char*)alloc_in(r.alloc, r.str_cap, 1) : nullptr;
        if (!r.nodes || (r.table_cap && !r.table) || (r.str_cap && !r.strs)) ok = r.fail("out of memory");
        r.out_n = 1;
        r.at = doc_at;
        if (ok) ok = r.value(root, 0);
        r.item_at = r.at;
        if (ok && r.at != r.size) ok = r.fail("bytes after the document");
        if (ok && r.summarized && (r.out_n != r.cap.nodes || r.used.keys != r.cap.keys || r.used.strings != r.cap.strings || r.used.bytes != r.cap.bytes))
            ok = r.fail("a summary larger than the document");
    }

    if (info) {
        *info = KborInfo{};
        info->summarized = r.summarized;
        info->indefinite = r.indefinite;
        info->counts = KborSummary{r.out_n, r.used.keys, r.used.strings, r.used.bytes};
        info->index_keys = r.index_keys;
        info->names = r.stored_names;
    }
    free_items(r.alloc, r.table, r.table_cap);
    r.indef.free();
    r.names.free();
    // A failed parse leaves a root to ask questions of, and nothing under it.
    if (!ok) {
        free_items(r.alloc, r.nodes, r.node_cap);
        free_in(r.alloc, r.strs, r.str_cap);
        r.nodes = alloc_items<JsonValue>(r.alloc, 1);
        r.out_n = 1;
        root = JsonValue{};
    }
    if (r.nodes) {
        root.flags |= KV_JSON_VALUE_LAST;
        r.nodes[0] = root;
        doc.nodes = r.nodes;
        doc.count = r.out_n;
    }
    doc.error = ok ? nullptr : (r.error ? r.error : "parse error");
    doc.error_at = ok ? 0 : (u32)(r.error_at > k_u32_max ? k_u32_max : r.error_at);
    doc.error_line = 0;
    return ok;
}

// ---- writing --------------------------------------------------------------------------------

namespace {

struct Encoder {
    const JsonDoc&    doc;
    Array<u8>&        out;
    Map<StrKey, u32>  names;
    KborSummary       s;

    void head(u8 major, u64 arg) {
        u8 m = (u8)(major << 5);
        if (arg < 24) { out.push((u8)(m | arg)); return; }
        u32 n = arg <= 0xFF ? 1 : arg <= 0xFFFF ? 2 : arg <= 0xFFFFFFFFull ? 4 : 8;
        u8* b = out.push_n(1 + n);
        b[0] = (u8)(m | (n == 1 ? 24 : n == 2 ? 25 : n == 4 ? 26 : 27));
        for (u32 i = 0; i < n; ++i) b[n - i] = (u8)(arg >> (8 * i));
    }
    void bytes(const void* data, usize n) { if (n) memcpy(out.push_n((u32)n), data, n); }

    void number(const JsonValue& v) {
        if (v.integer()) {
            if ((v.flags & KV_JSON_VALUE_NEGATIVE) && v.as.bits) head(1, ~v.as.bits);
            else head(0, v.as.bits); // -0 written as an integer is 0
            return;
        }
        f64 d = v.as.num;
        f32 f = (f32)d;
        f64 back = (f64)f;
        if (memcmp(&back, &d, sizeof d) == 0) { // the single holds it to the bit (NaN payloads too)
            u32 b;
            memcpy(&b, &f, sizeof b);
            u8* o = out.push_n(5);
            o[0] = 0xFA;
            for (u32 i = 0; i < 4; ++i) o[4 - i] = (u8)(b >> (8 * i));
        } else {
            u64 b;
            memcpy(&b, &d, sizeof b);
            u8* o = out.push_n(9);
            o[0] = 0xFB;
            for (u32 i = 0; i < 8; ++i) o[8 - i] = (u8)(b >> (8 * i));
        }
    }

    void key(const char* k) {
        if (!k) k = "";
        u32 n = (u32)strlen(k);
        bool inserted = false;
        u32* index = names.get_or_insert(StrKey{k, n}, (u32)s.keys, &inserted);
        if (!inserted) { head(0, *index); return; }
        ++s.keys;
        s.bytes += n;
        head(3, n);
        bytes(k, n);
    }

    void value(const JsonValue& v) {
        ++s.nodes;
        switch (v.type) {
        case JsonType::Null:   out.push(0xF6); return;
        case JsonType::Bool:   out.push(v.as.num != 0.0 ? 0xF5 : 0xF4); return;
        case JsonType::Number: number(v); return;
        case JsonType::String:
        case JsonType::Bytes:
            ++s.strings;
            s.bytes += v.len();
            head(v.is_bytes() ? 2 : 3, v.len());
            bytes(v.as.text, v.len());
            return;
        case JsonType::Array:
        case JsonType::Object: {
            bool map = v.is_object();
            head(map ? 5 : 4, v.count());
            for (const JsonValue* c = json_first(doc, v); c; c = json_next(doc, *c)) {
                if (map) key(c->key);
                value(*c);
            }
            return;
        }
        }
    }
};

} // namespace

void kbor_write(const JsonDoc& doc, Array<u8>& out, KborSummary* summary) {
    if (!doc.nodes || !doc.count) return;
    u32 start = out.len;
    Encoder e{doc, out, {}, {}};
    e.names.init(mem_general());
    e.value(doc.root());
    e.names.free();
    // The summary goes in front of the document it counts: written after, then moved in.
    u32 body = out.len - start;
    e.head(6, KV_KBOR_SUMMARY_TAG);
    e.head(4, 2);
    e.head(4, 4);
    e.head(0, e.s.nodes);
    e.head(0, e.s.keys);
    e.head(0, e.s.strings);
    e.head(0, e.s.bytes);
    u32 h = out.len - start - body;
    u8 front[64];
    memcpy(front, out.data + start + body, h);
    memmove(out.data + start + h, out.data + start, body);
    memcpy(out.data + start, front, h);
    if (summary) *summary = e.s;
}

} // namespace kv
