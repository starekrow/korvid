// Korvid core: KBOR, the engine's binary data format: the JSON tree (json.h) read from and
// written as compact bytes.
//
// JSON is the source format, readable and diffable; KBOR is the generated form of the same
// data, what a cooked file holds (a .kim compiles to a .kimb, an SVG to a .kshape). It is
// small without compression and read in one pass. It is CBOR (RFC 8949) holding what JSON
// can say, plus byte strings, with one rule that removes repeated key names: reading front
// to back, every text map key joins a table, and a map key that is an unsigned integer
// stands for the text key at that index in the table. A list of a thousand objects spells
// each key once. A normalized file says up front how big it is, in a summary.
//
// The reader builds the JSON reader's tree, so whatever loads from JSON loads from KBOR
// unchanged, and the same rule holds: the tree lives in the allocator you pass. A file with
// the summary is read in one pass, every allocation made once at the size the summary
// gives and checked as it fills; any other file (no summary, indefinite lengths, a foreign
// encoder's) is counted first in a walk over the item heads and then read the same way.
// Each distinct key is stored once however often its text appears.
//
// The writer writes a tree as normalized KBOR: definite lengths, an index for every
// repeated key, the summary in front; integers exact, a float as a single when that holds
// it exactly and a double otherwise. JSON to KBOR is json_parse and kbor_write; KBOR to
// JSON is kbor_parse and json_write; normalizing is kbor_parse and kbor_write. tools/kbor
// does each from the command line. docs/design/kbor.md has the format.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/json.h"

namespace kv {

// The summary's tag. From IANA's first-come range that takes three bytes (0xD9 0xCB 0x0B),
// unassigned there as of Nov 2026 and not yet registered.
constexpr u64 KV_KBOR_SUMMARY_TAG = 0xCB0B;

// What a document holds, as the summary counts it.
struct KborSummary {
    u64 nodes   = 0; // every value: each array, map and scalar, the document itself too; not keys
    u64 keys    = 0; // text-string map keys: the length the key table reaches
    u64 strings = 0; // text and byte strings that are values
    u64 bytes   = 0; // the bytes of every string, keys and values, as stored
};

// What kbor_parse found in a file, for a tool that reports on one (`kbor info`). Filled
// when asked for, after a failure too, with the counts up to where the read stopped.
struct KborInfo {
    bool        summarized = false; // the file began with the summary tag, as a normalized one does
    bool        indefinite = false; // some array or map had an indefinite length
    KborSummary counts;             // what the document held
    u64         index_keys = 0;     // keys written as an index into the table
    u64         names      = 0;     // distinct key names stored
};

// Parses a KBOR file into `doc`, as json_parse does a text: nodes and strings from `alloc`
// (null: the general allocator), which the caller owns (an arena, usually); the reader's
// own bookkeeping is freed before it returns. Text strings and keys must be UTF-8, and a
// key may not hold a NUL. False with doc.error and doc.error_at (a byte offset; there are
// no lines) on failure, and the root then a lone null; it does not log. Nodes have no line
// (0).
//
//   MemScope scope;
//   JsonDoc doc;
//   if (!kbor_parse(data, size, &scope.arena->a, doc)) { *why = doc.error; return false; }
bool kbor_parse(const void* data, usize size, Allocator* alloc, JsonDoc& doc, KborInfo* info = nullptr);

// Appends the tree as normalized KBOR to `out`; nothing for a doc with no nodes. The
// summary it wrote goes to `summary`, when given.
void kbor_write(const JsonDoc& doc, Array<u8>& out, KborSummary* summary = nullptr);

} // namespace kv
