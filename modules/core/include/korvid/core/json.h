// Korvid core: a JSON reader. Parses a text into a flat tree of plain nodes in an
// allocator of the caller's choosing (the scratch arena, usually: nothing here frees), so
// a file's structure can be walked and copied out without any allocation of its own.
// Strings are unescaped and NUL-terminated; numbers are doubles; every error names its
// byte offset. Nothing is written: the engine reads formats, tools write them.
//
//   JsonDoc doc;
//   if (!json_parse(text, len, mem_scratch(), doc)) KV_ERROR("...", "%s at %u", doc.error, doc.error_at);
//   const JsonValue* tint = json_get(doc, doc.root(), "tint");
//   for (const JsonValue* v = json_first(doc, *tint); v; v = json_next(doc, *v)) ...
#pragma once
#include "korvid/core/memory.h"

namespace kv {

enum class JsonType : u8 { Null, Bool, Number, String, Array, Object };

struct JsonValue {
    JsonType    type   = JsonType::Null;
    u32         count  = 0;       // children of an array or object
    u32         first  = 0;       // node index of the first child; 0 is never a child
    u32         next   = 0;       // node index of the next sibling; 0 ends the list
    const char* key    = nullptr; // an object member's name
    const char* str    = nullptr; // String: the unescaped text
    u32         len    = 0;       // its length in bytes
    f64         number = 0.0;     // Number, and Bool as 0 or 1
    u64         bits   = 0;       // an integer literal's exact value (two's complement when negative)
    bool        integer = false;  // the Number was written without fraction or exponent and fits 64 bits

    bool is_null()   const { return type == JsonType::Null; }
    bool is_bool()   const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array()  const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }
};

struct JsonDoc {
    JsonValue*  nodes    = nullptr; // node 0 is the root
    u32         count    = 0;
    const char* error    = nullptr; // static text; null when the parse succeeded
    u32         error_at = 0;       // byte offset

    const JsonValue& root() const { return nodes[0]; }
};

constexpr u32 KV_JSON_MAX_DEPTH = 64;

// Parses [text, text + len). Nodes and strings come from `alloc`; the caller owns them
// (and frees nothing when the allocator is an arena). False with doc.error set on failure.
bool json_parse(const char* text, usize len, Allocator* alloc, JsonDoc& doc);

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
