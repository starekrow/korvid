// Korvid core: a JSON reader. Parses a text into a flat tree of small plain nodes in an
// allocator of the caller's choosing (the scratch arena, usually: nothing here frees), so
// a file's structure can be walked and copied out without any allocation of its own.
// Strings are unescaped and NUL-terminated; numbers are doubles; every error names its
// byte offset and line, and every node knows the line it began on, so whoever gives the
// values meaning can say where a mistake is. The same tree comes from KBOR (kbor.h), which
// adds one type JSON cannot say: Bytes. json_write renders a tree as text again, for the
// tools that convert; the engine itself reads formats and does not write them.
//
//   JsonDoc doc;
//   if (!json_parse(text, len, mem_scratch(), doc)) KV_ERROR("...", "%s at %u", doc.error, doc.error_at);
//   const JsonValue* tint = json_get(doc, doc.root(), "tint");
//   for (const JsonValue* v = json_first(doc, *tint); v; v = json_next(doc, *v)) ...
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

// Bytes comes only from KBOR: a byte string, which JSON writes as base64 text.
enum class JsonType : u8 { Null, Bool, Number, String, Array, Object, Bytes };

// 24 bytes. What a value holds depends on its type, so it is read through the accessors
// below, each of which answers for any type (0, null or false when it does not apply).
struct JsonValue {
    union {
        f64         num;                 // Number that is not an exact integer; Bool as 0 or 1
        u64         bits;                // an integer literal (two's complement when negative)
        const char* text;                // String, Bytes: unescaped, NUL after it, its u32 length just before it
        struct { u32 first, count; } kids; // Array, Object: the children, side by side in doc.nodes
    } as = {0.0};
    const char* key   = nullptr; // an object member's name
    u32         line  = 0;       // 1-based line the value starts on (a member's: its name's)
    JsonType    type  = JsonType::Null;
    u8          flags = 0;       // KV_JSON_VALUE_*

    bool is_null()   const { return type == JsonType::Null; }
    bool is_bool()   const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array()  const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }
    bool is_bytes()  const { return type == JsonType::Bytes; }

    // The Number was written without fraction or exponent and fits 64 bits.
    bool        integer() const;
    // Number, and Bool as 0 or 1.
    f64         number() const;
    // An integer literal's exact value (two's complement when negative).
    u64         bits() const { return integer() ? as.bits : 0; }
    // String: the unescaped text, and its length in bytes.
    const char* str() const { return type == JsonType::String ? as.text : nullptr; }
    // Bytes: the bytes. len() is their count, for Bytes as for String.
    const u8*   bytes() const { return type == JsonType::Bytes ? (const u8*)as.text : nullptr; }
    u32         len() const;
    // Children of an array or object.
    u32         count() const { return type == JsonType::Array || type == JsonType::Object ? as.kids.count : 0; }
};

constexpr u8 KV_JSON_VALUE_INTEGER  = 1 << 0; // as.bits holds the value
constexpr u8 KV_JSON_VALUE_NEGATIVE = 1 << 1; // the integer literal began with '-'
constexpr u8 KV_JSON_VALUE_LAST     = 1 << 2; // no sibling follows

inline bool JsonValue::integer() const { return (flags & KV_JSON_VALUE_INTEGER) != 0; }
inline f64 JsonValue::number() const {
    if (type != JsonType::Number && type != JsonType::Bool) return 0.0;
    if (!(flags & KV_JSON_VALUE_INTEGER)) return as.num;
    // What strtod would have made of the literal: both round to nearest, and -0 stays -0.
    return flags & KV_JSON_VALUE_NEGATIVE ? -(f64)((u64)0 - as.bits) : (f64)as.bits;
}
inline u32 JsonValue::len() const {
    if (type != JsonType::String && type != JsonType::Bytes) return 0;
    u32 n;
    memcpy(&n, as.text - sizeof n, sizeof n);
    return n;
}

struct JsonDoc {
    JsonValue*  nodes    = nullptr; // node 0 is the root; a container's children are consecutive
    u32         count    = 0;
    const char* error    = nullptr; // static text; null when the parse succeeded
    u32         error_at = 0;       // byte offset
    u32         error_line = 0;     // and its 1-based line

    const JsonValue& root() const { return nodes[0]; }
};

constexpr u32 KV_JSON_MAX_DEPTH = 64;

// Parses [text, text + len). Nodes and strings come from `alloc`; the caller owns them
// (and frees nothing when the allocator is an arena). The nodes are one block of exactly
// doc.count: the text is scanned once to count them, then parsed into the block, so
// nothing grows and nothing is left behind. False with doc.error set on failure, and the
// root then a lone null.
//
// flags: KV_JSON_LINE_COMMENTS also passes over `//` to the end of the line wherever
// white space may stand. That is not JSON, and nothing of the engine's own is written
// that way; it is there for the formats that choose to let their authors do it (.berg).
constexpr u32 KV_JSON_LINE_COMMENTS = 1u << 0;
bool json_parse(const char* text, usize len, Allocator* alloc, JsonDoc& doc, u32 flags = 0);

// Walking. Null when absent or when `v` is not the container asked about.
const JsonValue* json_get(const JsonDoc& doc, const JsonValue& object, const char* key);
const JsonValue* json_at(const JsonDoc& doc, const JsonValue& array, u32 index);
const JsonValue* json_first(const JsonDoc& doc, const JsonValue& container);
const JsonValue* json_next(const JsonDoc& doc, const JsonValue& v);

// Typed reads with defaults: the value's number, or `dflt` when it is absent or not a number.
f64         json_number(const JsonValue* v, f64 dflt);
// Exact integers: an integer literal in range as written (a double would round past 2^53),
// any other number truncated and clamped; `dflt` for what is not a number.
u64         json_u64(const JsonValue* v, u64 dflt);
i64         json_i64(const JsonValue* v, i64 dflt);
bool        json_bool(const JsonValue* v, bool dflt);
const char* json_string(const JsonValue* v, const char* dflt);
// Up to `cap` numbers from an array into `out`; returns how many were there (not stored
// past cap). 0 for anything that is not an array.
u32         json_numbers(const JsonDoc& doc, const JsonValue* array, f32* out, u32 cap);

const char* json_type_name(JsonType t);

// Writes the tree as JSON text, appended to `out` (no NUL). What KBOR holds that JSON
// cannot say goes as docs/design/kbor.md ("JSON and back") has it: an integer as its
// digits; any other number with a fraction or an exponent always (1.0, not 1), in the
// fewest digits that read back to the same double; NaN and the infinities as the strings
// "nan", "inf" and "-inf"; Bytes as standard, padded base64. So text this writes reads
// back (json_parse) to a tree with the same numbers, integers and floats kept apart.
//
// KV_JSON_WRITE_PRETTY: a member or element per line, two spaces a level, and an array
// of nothing but scalars that are not strings on one line. Without it, no white space.
// A pretty text ends with a newline.
constexpr u32 KV_JSON_WRITE_PRETTY = 1u << 0;
void json_write(const JsonDoc& doc, Array<u8>& out, u32 flags = KV_JSON_WRITE_PRETTY);

// ---- building a tree -------------------------------------------------------------------------
// A JsonDoc made by hand, for whatever writes a format (kim's compiler, a tool) rather than
// reading one. Values are made first and attached after, in any order; finish() lays them
// out as a parse would (node 0 the root, each container's children side by side), so the
// tree walks, writes (json_write, kbor_write) and reads exactly like a parsed one. Members
// keep the order they were set in, and a key set twice is there twice: whoever wants it
// canonical sorts and dedupes before setting.
//
// Strings, keys and the finished nodes come from `alloc`, the document's allocator; the
// builder's own lists from the general allocator, until reset() or free(). A value is
// attached once; attaching one twice, or one to itself, is refused.
//
//   JsonBuilder b(&arena.a);
//   JsonRef o = b.object();
//   b.set(o, "kim", b.integer(1));
//   b.set(o, "file", b.string("ui/form.kim"));
//   JsonDoc doc;
//   b.finish(o, doc);
using JsonRef = u32;
constexpr JsonRef KV_JSON_NO_REF = ~0u;

struct JsonBuilder {
    struct Item {
        JsonValue v;
        u32       first = KV_JSON_NO_REF, last = KV_JSON_NO_REF, next = KV_JSON_NO_REF;
        u32       count = 0;
        bool      attached = false;
    };

    Allocator*  alloc = nullptr;
    Array<Item> items{};
    bool        failed = false; // an allocation failed or a value was attached twice

    explicit JsonBuilder(Allocator* a) : alloc(a) {}

    JsonRef null();
    JsonRef boolean(bool v);
    JsonRef integer(i64 v);
    JsonRef unsigned_integer(u64 v);
    JsonRef number(f64 v);                  // a float: written with a fraction, as 1.0
    JsonRef string(const char* s);
    JsonRef string(const char* s, u32 len); // the bytes copied, a NUL after them
    JsonRef bytes(const void* data, u32 len);
    JsonRef array();
    JsonRef object();

    // Appends `v` to an array, or to an object as a member named `key` (copied). False,
    // and nothing attached, when the container is not one or `v` is already attached.
    bool push(JsonRef array, JsonRef v);
    bool set(JsonRef object, const char* key, JsonRef v);
    // The line a value says it began on (JsonValue::line), for a builder with lines to keep.
    void line(JsonRef v, u32 line);

    u32  count(JsonRef container) const { return container < items.len ? items[container].count : 0; }
    JsonType type(JsonRef v) const { return v < items.len ? items[v].v.type : JsonType::Null; }

    // Lays out the tree under `root` into `doc`: its nodes one block from `alloc`, exactly
    // as many as are reachable. False (and doc a lone null, with doc.error) when anything
    // failed along the way.
    bool finish(JsonRef root, JsonDoc& doc);
    void reset() { items.clear(); failed = false; }
    void free() { items.free(); }

private:
    JsonRef make(JsonType t);
    const char* copy(const char* s, u32 len, bool with_len);
};

} // namespace kv
