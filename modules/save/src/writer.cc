#include "korvid/save/writer.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

void SaveWriter::init(Allocator* a, SaveFormat f, bool pretty_print) {
    out.alloc = a;
    format = f;
    pretty = pretty_print;
    reset();
}

void SaveWriter::free() { out.free(); }

void SaveWriter::reset() {
    out.clear();
    failed = false;
    depth = 0;
    after_key = false;
    top_values = 0;
}

void SaveWriter::put(const char* s, u32 n) {
    if (n) memcpy(out.push_n(n), s, n);
}

void SaveWriter::put(const char* s) { put(s, (u32)strlen(s)); }

void SaveWriter::newline(u32 indent) {
    put("\n", 1);
    for (u32 i = 0; i < indent; ++i) put("  ", 2);
}

void SaveWriter::fail(const char* why) {
    if (!failed) KV_ERROR("save", "writer: %s", why);
    failed = true;
}

// Separators and indentation before a value; false if a value cannot go here.
bool SaveWriter::before_value() {
    if (depth == 0) {
        if (top_values++) { fail("a second value at the top level"); return false; }
        return true;
    }
    Frame& f = frames[depth - 1];
    if (!f.array) {
        if (!after_key) { fail("a value in an object without a key"); return false; }
        after_key = false;
        ++f.count;
        return true;
    }
    if (f.count) put(",", 1);
    if (pretty) { if (f.compact) { if (f.count) put(" ", 1); } else newline(depth); }
    ++f.count;
    return true;
}

void SaveWriter::begin(bool array, bool compact) {
    if (!before_value()) return;
    if (depth == KV_SAVE_MAX_DEPTH) { fail("nesting too deep"); return; }
    // A compact container keeps its children compact.
    bool inherit = depth && frames[depth - 1].compact;
    frames[depth++] = Frame{array, compact || inherit, 0};
    put(array ? "[" : "{", 1);
}

void SaveWriter::end(bool array) {
    if (depth == 0 || frames[depth - 1].array != array) { fail(array ? "end_array without its begin" : "end_object without its begin"); return; }
    if (after_key) { fail("a key without a value"); after_key = false; }
    Frame f = frames[--depth];
    if (pretty && f.count && !f.compact) newline(depth);
    put(array ? "]" : "}", 1);
}

void SaveWriter::begin_object(bool compact) { begin(false, compact); }
void SaveWriter::end_object() { end(false); }
void SaveWriter::begin_array(bool compact) { begin(true, compact); }
void SaveWriter::end_array() { end(true); }

void SaveWriter::key(const char* k) {
    if (depth == 0 || frames[depth - 1].array) { fail("a key outside an object"); return; }
    if (after_key) { fail("two keys in a row"); return; }
    Frame& f = frames[depth - 1];
    if (f.count) put(",", 1);
    if (pretty) { if (f.compact) { if (f.count) put(" ", 1); } else newline(depth); }
    json_write_string(out, k ? k : "", k ? strlen(k) : 0);
    put(pretty ? ": " : ":");
    after_key = true;
}

void SaveWriter::null_value() { if (before_value()) put("null"); }
void SaveWriter::bool_value(bool v) { if (before_value()) put(v ? "true" : "false"); }

void SaveWriter::int_value(i64 v) {
    char b[32];
    kv::format(b, sizeof b, "%lld", (long long)v);
    if (before_value()) put(b);
}

void SaveWriter::uint_value(u64 v) {
    char b[32];
    kv::format(b, sizeof b, "%llu", (unsigned long long)v);
    if (before_value()) put(b);
}

void SaveWriter::f64_value(f64 v) { if (before_value()) json_write_f64(out, v); }
void SaveWriter::f32_value(f32 v) { if (before_value()) json_write_f32(out, v); }

void SaveWriter::string_value(const char* s) { string_value(s ? s : "", s ? (u32)strlen(s) : 0); }

void SaveWriter::string_value(const char* s, u32 len) {
    if (before_value()) json_write_string(out, s, len);
}

bool SaveWriter::finish() {
    if (depth) fail("a container left open");
    if (top_values != 1) fail("nothing written");
    if (!failed && pretty) put("\n", 1);
    return !failed;
}

bool save_parse(SaveFormat f, const void* data, usize size, Allocator* alloc, JsonDoc& doc) {
    switch (f) {
    case SaveFormat::Json: return json_parse((const char*)data, size, alloc, doc);
    }
    doc.error = "unknown save format";
    doc.error_at = 0;
    return false;
}

} // namespace kv
