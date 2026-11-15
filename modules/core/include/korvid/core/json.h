// Korvid core: a JSON reader. Parses a text into a flat tree of small plain nodes in an
// allocator of the caller's choosing (the scratch arena, usually: nothing here frees), so
// a file's structure can be walked and copied out without any allocation of its own.
// Strings are unescaped and NUL-terminated; numbers are doubles; every error names its
// byte offset and line, and every node knows the line it began on, so whoever gives the
// values meaning can say where a mistake is. Nothing is written: the engine reads formats, tools write them.
//
//   JsonDoc doc;
//   if (!json_parse(text, len, mem_scratch(), doc)) KV_ERROR("...", "%s at %u", doc.error, doc.error_at);
//   const JsonValue* tint = json_get(doc, doc.root(), "tint");
//   for (const JsonValue* v = json_first(doc, *tint); v; v = json_next(doc, *v)) ...
#pragma once
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

enum class JsonType : u8 { Null, Bool, Number, String, Array, Object };

// 24 bytes. What a value holds depends on its type, so it is read through the accessors
// below, each of which answers for any type (0, null or false when it does not apply).
struct JsonValue {
    union {
        f64         num;                 // Number that is not an exact integer; Bool as 0 or 1
        u64         bits;                // an integer literal (two's complement when negative)
        const char* text;                // String: unescaped, its u32 length just before it
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

    // The Number was written without fraction or exponent and fits 64 bits.
    bool        integer() const;
    // Number, and Bool as 0 or 1.
    f64         number() const;
    // An integer literal's exact value (two's complement when negative).
    u64         bits() const { return integer() ? as.bits : 0; }
    // String: the unescaped text, and its length in bytes.
    const char* str() const { return type == JsonType::String ? as.text : nullptr; }
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
    if (type != JsonType::String) return 0;
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

} // namespace kv
