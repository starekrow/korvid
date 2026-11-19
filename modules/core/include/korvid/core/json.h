// Korvid core: JSON, read into a tree of plain nodes, written back as text, or built by
// hand.
//
// Most of the engine's data files are JSON: materials, sprite sheets, levels, records.
// json_parse turns a text into a JsonDoc, an array of small JsonValue nodes: node 0 is the
// root, and each array's or object's children sit side by side further on. A loader walks
// the tree from the root, reads the values it knows with defaults for the rest, and copies
// out what it keeps. Two things to hold while doing it:
//
//   The tree lives in the allocator you pass. Nodes and strings come from it and are
//   never freed one by one, so pass an arena: usually the scratch arena inside a MemScope,
//   which drops the whole tree when the loader returns. A string kept past that is copied
//   out first, or interned (name.h).
//
//   Every node knows its line. JsonValue::line, and doc.error_line for a parse error, let
//   the code that gives the values meaning say where in the file a mistake is.
//
// Strings are unescaped (a \u escape becomes UTF-8) and NUL-terminated; other bytes pass
// through unchecked. Numbers are doubles, and an integer literal also keeps its exact
// 64-bit value. KBOR (kbor.h), the engine's binary form of the same data, reads into this
// same tree and adds one type JSON cannot say: Bytes. json_write turns a tree back into
// text (tools/kbor decodes KBOR with it), and JsonBuilder makes a tree from code, for
// whatever writes a format rather than reading one.
//
//   MemScope scope;                 // the tree goes when the scope does
//   JsonDoc doc;
//   if (!json_parse(text, len, mem_scratch(), doc)) {
//       KV_ERROR("records", "%s: %s at line %u", path, doc.error, doc.error_line);
//       return false;
//   }
//   const JsonValue& root = doc.root();
//   u64 version = json_u64(json_get(doc, root, "version"), 1);
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

// ---- the tree --------------------------------------------------------------------------

// What a JsonValue is: Null, Bool, Number, String, Array, Object, or Bytes. Bytes comes
// only from KBOR (or a JsonBuilder): a byte string, which JSON writes as base64 text.
enum class JsonType : u8 { Null, Bool, Number, String, Array, Object, Bytes };

// One node: a value, and its name when it is an object's member. 24 bytes. What it holds
// depends on its type, so it is read through the accessors below, each of which answers
// for any type (0, null or false when it does not apply).
struct JsonValue {
    union {
        f64         num;                 // Number that is not an exact integer; Bool as 0 or 1
        u64         bits;                // an integer literal (two's complement when negative)
        const char* text;                // String, Bytes: unescaped, NUL after it, its u32 length just before it
        struct { u32 first, count; } kids; // Array, Object: the children, side by side in doc.nodes
    } as = {0.0};
    const char* key   = nullptr; // an object member's name; null for anything else
    u32         line  = 0;       // 1-based line the value starts on (a member's: its name's); 0 from KBOR
    JsonType    type  = JsonType::Null;
    u8          flags = 0;       // KV_JSON_VALUE_*

    // The type tests.
    bool is_null()   const { return type == JsonType::Null; }
    bool is_bool()   const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array()  const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }
    bool is_bytes()  const { return type == JsonType::Bytes; }

    // The Number is an integer literal (no fraction or exponent; from KBOR, an integer)
    // that fits 64 bits: u64, or i64 when negative. bits() then holds it exactly.
    bool        integer() const;
    // The value as a double: a Number (an integer literal rounded as strtod would round
    // it), a Bool as 0 or 1; 0 for any other type.
    f64         number() const;
    // An integer literal's exact value (two's complement when negative); 0 when integer()
    // is false.
    u64         bits() const { return integer() ? as.bits : 0; }
    // String: the unescaped text, NUL-terminated; null for any other type. A `\u0000` in
    // the text is a NUL inside the string, so len() is the length to trust.
    const char* str() const { return type == JsonType::String ? as.text : nullptr; }
    // Bytes: the bytes; null for any other type.
    const u8*   bytes() const { return type == JsonType::Bytes ? (const u8*)as.text : nullptr; }
    // String or Bytes: the length in bytes, the NUL after a string not counted; 0 for any
    // other type.
    u32         len() const;
    // Array or Object: how many children; 0 for any other type.
    u32         count() const { return type == JsonType::Array || type == JsonType::Object ? as.kids.count : 0; }
};

// JsonValue::flags, set by the parsers and the builder.
constexpr u8 KV_JSON_VALUE_INTEGER  = 1 << 0; // as.bits holds the value
constexpr u8 KV_JSON_VALUE_NEGATIVE = 1 << 1; // the integer literal began with '-'
constexpr u8 KV_JSON_VALUE_LAST     = 1 << 2; // no sibling follows

// The accessors that read the flags.
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

// A document: every node in one block, and the error when a parse failed. Its nodes and
// strings are valid for as long as the allocator's memory they came from.
struct JsonDoc {
    JsonValue*  nodes    = nullptr; // node 0 is the root; a container's children are consecutive
    u32         count    = 0;       // nodes in the block
    const char* error    = nullptr; // static text; null when the parse succeeded
    u32         error_at = 0;       // byte offset
    u32         error_line = 0;     // and its 1-based line (0 from KBOR, which has no lines)

    // Node 0. After a failed parse it is a lone null, so a loader can still ask it
    // questions.
    const JsonValue& root() const { return nodes[0]; }
};

// How deep arrays and objects may nest. One level deeper fails the parse, JSON or KBOR,
// with "nesting too deep".
constexpr u32 KV_JSON_MAX_DEPTH = 64;

// ---- reading ---------------------------------------------------------------------------

// json_parse's flag: also pass over `//` to the end of the line wherever white space may
// stand. That is not JSON, and nothing of the engine's own is written that way; it is
// there for the formats that choose to let their authors do it (.berg).
constexpr u32 KV_JSON_LINE_COMMENTS = 1u << 0;
// Parses [text, text + len), which need not be NUL-terminated, into `doc`. False on
// failure, with doc.error (static text, such as "trailing comma"), doc.error_at and
// doc.error_line set, and the root then a lone null; it does not log.
//
// Nodes and strings come from `alloc` (null: the general allocator); the caller owns them,
// and with an arena frees nothing. The nodes are one block of exactly doc.count: the text
// is scanned once to count them, then parsed into the block, so nothing grows and nothing
// is left behind. A number literal of 64 characters or more is refused.
bool json_parse(const char* text, usize len, Allocator* alloc, JsonDoc& doc, u32 flags = 0);

// The member of `object` named `key`, the first one if the text has it twice. Null when
// there is none, or `object` is not an object. A linear scan of the members.
const JsonValue* json_get(const JsonDoc& doc, const JsonValue& object, const char* key);
// Element `index` of `array`. Null past the end, or when `array` is not an array.
const JsonValue* json_at(const JsonDoc& doc, const JsonValue& array, u32 index);
// The first child of an array or object. Null when it is empty or not a container. With
// json_next, the way to visit every child in order (from modules/asset/src/material.cc):
//
//   for (const JsonValue* m = json_first(doc, root); m; m = json_next(doc, *m)) {
//       if (strcmp(m->key, "tint") == 0) n = json_numbers(doc, m, t, 4);
//       else if (strcmp(m->key, "filter") == 0) filter = json_string(m, "");
//   }
const JsonValue* json_first(const JsonDoc& doc, const JsonValue& container);
// The sibling after `v`. Null after the last child, and for the root.
const JsonValue* json_next(const JsonDoc& doc, const JsonValue& v);

// ---- typed reads -----------------------------------------------------------------------
// Each takes a pointer that may be null and returns `dflt` when it is, or when the value is
// not of the type asked for, so a lookup passes straight in:
//
//   f.file = json_string(json_get(doc, root, "file"), "");

// The value's number, or `dflt` when it is absent or not a number (a Bool is not one).
f64         json_number(const JsonValue* v, f64 dflt);
// Exact integers: an integer literal in range as written (a double would round past 2^53),
// any other number truncated and clamped to the type's range; `dflt` for what is not a
// number.
u64         json_u64(const JsonValue* v, u64 dflt);
i64         json_i64(const JsonValue* v, i64 dflt);
// A Bool's value, or `dflt` when it is absent or not a bool.
bool        json_bool(const JsonValue* v, bool dflt);
// A String's text, or `dflt` when it is absent or not a string (Bytes included). The text
// is the document's: copy it to keep it past the allocator's memory.
const char* json_string(const JsonValue* v, const char* dflt);
// Up to `cap` numbers from an array into `out`, an element that is not a number stored as
// 0. Returns how many elements the array has, so a result above `cap` means some were not
// stored. 0 for anything that is not an array.
u32         json_numbers(const JsonDoc& doc, const JsonValue* array, f32* out, u32 cap);

// The type's name in lowercase ("object", "bytes"), for error messages.
const char* json_type_name(JsonType t);

// ---- writing ---------------------------------------------------------------------------

// json_write's flag: a member or element per line, two spaces a level, and an array of
// nothing but numbers, bools and nulls on one line. Without it, no white space. A pretty
// text ends with a newline.
constexpr u32 KV_JSON_WRITE_PRETTY = 1u << 0;
// Writes the tree as JSON text, appended to `out` (no NUL); nothing for a doc with no
// nodes. What KBOR holds that JSON cannot say goes as docs/design/kbor.md ("JSON and back")
// has it: an integer as its digits; any other number with a fraction or an exponent always
// (1.0, not 1), in the fewest digits that read back to the same double; NaN and the
// infinities as the strings "nan", "inf" and "-inf"; Bytes as standard, padded base64. So
// text this writes reads back (json_parse) to a tree with the same numbers, integers and
// floats kept apart.
void json_write(const JsonDoc& doc, Array<u8>& out, u32 flags = KV_JSON_WRITE_PRETTY);

// The leaves as json_write spells them, for a writer that lays out its own structure
// (save's SaveWriter) and so writes the same text. Each appends to `out` (no NUL).
//
// `len` bytes as a JSON string: quoted, with `"`, `\` and the control bytes escaped (\n,
// \r, \t, \b and \f by name, the rest as \u00XX). Other bytes, UTF-8 included, pass
// through unchanged.
void json_write_string(Array<u8>& out, const char* s, usize len);
// A float as json_write writes one: the fewest of 15 to 17 digits that read back to the
// same double, always with a fraction or an exponent (1.0, not 1), so it reads back as a
// float and not an integer; NaN and the infinities as "nan", "inf" and "-inf".
void json_write_f64(Array<u8>& out, f64 v);
// The same for an f32, in the fewest of 6 to 9 digits that read back to the same f32:
// 0.15, where json_write_f64 of the same value takes 0.15000000596046448. json_parse reads
// that as the nearest double, which narrows to `v` again for every finite f32.
void json_write_f32(Array<u8>& out, f32 v);

// ---- building a tree -------------------------------------------------------------------
// A JsonDoc made by hand, for whatever writes a format (kim's compiler, a tool) rather than
// reading one. Values are made first and attached after, in any order; finish() lays them
// out as a parse would (node 0 the root, each container's children side by side), so the
// tree walks, writes (json_write, kbor_write) and reads exactly like a parsed one. Members
// keep the order they were set in, and a key set twice is there twice: whoever wants it
// canonical sorts and dedupes before setting.
//
// Strings, keys and the finished nodes come from `alloc`, the document's allocator; the
// builder's own list from the general allocator, which free() returns. A value is attached
// once; attaching one twice, or one to itself, is refused. Any failure along the way (an
// allocation, a refused push or set) marks the builder failed and makes finish() fail, so
// a writer can make every call and check once, at finish(). From
// modules/patherizer/src/kshape.cc:
//
//   MemScope scope;
//   JsonBuilder b(&scope.arena->a);
//   JsonRef root = b.object();
//   b.set(root, "kshape", b.integer(1));
//   JsonRef view = b.array();
//   for (u32 k = 0; k < 4; ++k) b.push(view, b.number(s.view[k]));
//   b.set(root, "view", view);
//   JsonDoc doc;
//   bool ok = b.finish(root, doc);
//   if (ok) kbor_write(doc, out);
//   b.free();

// A value in a builder: its index in the builder's list.
using JsonRef = u32;
constexpr JsonRef KV_JSON_NO_REF = ~0u; // no value: what a maker returns when out of memory

// Makes values, attaches them, and lays out the tree.
struct JsonBuilder {
    // One value in the working list, its children a linked list that finish() follows.
    struct Item {
        JsonValue v;        // the value, with its key once set
        u32       first = KV_JSON_NO_REF, last = KV_JSON_NO_REF, next = KV_JSON_NO_REF; // children, sibling
        u32       count = 0;          // children attached
        bool      attached = false;   // pushed or set into a container
    };

    Allocator*  alloc = nullptr; // the document's: strings, keys, the finished nodes
    Array<Item> items{};         // every value made, by JsonRef
    bool        failed = false;  // an allocation failed or a push or set was refused

    // A builder whose document lives in `a`, which must not be null.
    explicit JsonBuilder(Allocator* a) : alloc(a) {}

    // The makers. Each makes an unattached value and returns its ref, or KV_JSON_NO_REF
    // (and marks the builder failed) when out of memory.
    JsonRef null();
    JsonRef boolean(bool v);
    JsonRef integer(i64 v);
    JsonRef unsigned_integer(u64 v);        // for integers past i64's range
    JsonRef number(f64 v);                  // a float: written with a fraction, as 1.0
    JsonRef string(const char* s);          // copied; null makes ""
    JsonRef string(const char* s, u32 len); // the bytes copied, a NUL after them
    JsonRef bytes(const void* data, u32 len); // copied
    JsonRef array();
    JsonRef object();

    // Appends `v` to an array, or to an object as a member named `key` (copied). False,
    // nothing attached and the builder marked failed, when the container is not one,
    // `v` is already attached or is the container itself, a ref is not this builder's, or
    // `key` is null.
    bool push(JsonRef array, JsonRef v);
    bool set(JsonRef object, const char* key, JsonRef v);
    // The line a value says it began on (JsonValue::line), for a builder with lines to keep.
    void line(JsonRef v, u32 line);

    // How many children `container` has so far; 0 for a ref that is not this builder's.
    u32  count(JsonRef container) const { return container < items.len ? items[container].count : 0; }
    // What `v` is; Null for a ref that is not this builder's.
    JsonType type(JsonRef v) const { return v < items.len ? items[v].v.type : JsonType::Null; }

    // Lays out the tree under `root` into `doc`: its nodes one block from `alloc`, exactly
    // as many as are reachable (values never attached under `root` are left out). False
    // (and doc a lone null, with doc.error) when anything failed along the way. The builder
    // keeps its values; reset() or free() after.
    bool finish(JsonRef root, JsonDoc& doc);
    // Forgets every value and the failure, for another document; keeps the list's memory.
    void reset() { items.clear(); failed = false; }
    // Returns the list's memory. The finished document is `alloc`'s and stays.
    void free() { items.free(); }

private:
    JsonRef make(JsonType t);
    const char* copy(const char* s, u32 len, bool with_len);
};

} // namespace kv
