// Korvid save: the writer. Objects, arrays and scalars, written in order into a byte
// array, in one of the formats a save can take. The calls are the data model, not the
// syntax: a JSON object and a CBOR map are both begin_object, key, value, ..., end_object,
// so code that writes a save never knows which it is making. JSON is the one built;
// CBOR (indefinite-length maps and arrays, which stream from these same calls) is the
// one planned.
//
//   SaveWriter w;
//   w.init(mem_general());
//   w.begin_object();
//   w.key("level"); w.string_value("village");
//   w.key("hp");    w.int_value(3);
//   w.end_object();
//   if (w.finish()) write w.out.data, w.out.len
//   w.free();
//
// Floats are written so they read back to the same bits: the fewest digits that do. Not
// a number and the infinities, which JSON cannot say, are the strings "nan", "inf" and
// "-inf"; the loaders read them back. Integers are exact to 64 bits either way.
//
// Mistakes (a value where a key belongs, an end with nothing open, nesting past
// KV_SAVE_MAX_DEPTH) set `failed`, log once, and are otherwise ignored, so a save can be
// written straight through and checked once at finish().
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/json.h"

namespace kv {

enum class SaveFormat : u8 { Json };

constexpr u32 KV_SAVE_MAX_DEPTH = 32;

struct SaveWriter {
    Array<u8>  out;                        // the bytes so far
    SaveFormat format = SaveFormat::Json;
    bool       pretty = true;              // JSON: a member or element per line, indented
    bool       failed = false;

    void init(Allocator* a, SaveFormat f = SaveFormat::Json, bool pretty_print = true);
    void free();
    void reset(); // empty again, for the next save; keeps the buffer

    // Containers. A compact one stays on one line when pretty (a vector's lanes, a pair).
    void begin_object(bool compact = false);
    void end_object();
    void begin_array(bool compact = false);
    void end_array();
    // In an object, before each value.
    void key(const char* k);

    void null_value();
    void bool_value(bool v);
    void int_value(i64 v);
    void uint_value(u64 v);
    void f64_value(f64 v);
    void f32_value(f32 v);
    void string_value(const char* s);           // UTF-8, NUL-terminated
    void string_value(const char* s, u32 len);  // UTF-8, `len` bytes

    // True when exactly one value was written, every container closed and nothing failed.
    bool finish();

private:
    struct Frame { bool array; bool compact; u32 count; };
    Frame frames[KV_SAVE_MAX_DEPTH];
    u32   depth = 0;
    bool  after_key = false; // an object member's key is out, its value is next
    u32   top_values = 0;

    void put(const char* s, u32 n);
    void put(const char* s);
    void newline(u32 indent);
    bool before_value();
    void begin(bool array, bool compact);
    void end(bool array);
    void fail(const char* why);
    void number_text(const char* text);
    void put_string(const char* s, u32 len);
};

// Reads a save's bytes into the document tree the loaders walk (the JSON reader's tree:
// a CBOR reader would build the same one). False with doc.error set when they do not parse.
bool save_parse(SaveFormat f, const void* data, usize size, Allocator* alloc, JsonDoc& doc);

} // namespace kv
