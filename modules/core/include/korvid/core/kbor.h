// Korvid core: KBOR, the engine's binary data format (docs/design/kbor.md). CBOR holding
// what JSON can say, where a map key that is an unsigned integer stands for the text key
// at that index in a table of every text key met so far, front to back. A normalized file
// says up front how big it is, in a summary.
//
// The reader builds the JSON reader's tree (json.h), so whatever loads from JSON loads
// from KBOR unchanged. A file with the summary is read in one pass, every allocation made
// once at the size the summary gives and checked as it fills; any other file (no summary,
// indefinite lengths, a foreign encoder's) is counted first in a walk over the item heads
// and then read the same way. Each distinct key is stored once however often its text
// appears.
//
//   JsonDoc doc;
//   if (!kbor_parse(bytes, size, &arena.a, doc)) KV_ERROR("...", "%s at byte %u", doc.error, doc.error_at);
//
// The writer writes a tree as normalized KBOR: definite lengths, an index for every
// repeated key, the summary in front; integers exact, a float as a single when that holds
// it exactly and a double otherwise. JSON to KBOR is json_parse and kbor_write; KBOR to
// JSON is kbor_parse and json_write; normalizing is kbor_parse and kbor_write.
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

struct KborInfo {
    bool        summarized = false; // the file carried the summary tag (and so was normalized)
    bool        indefinite = false; // some array or map had an indefinite length
    KborSummary counts;             // what the document held
    u64         index_keys = 0;     // keys written as an index into the table
    u64         names      = 0;     // distinct key names stored
};

// Parses a KBOR file into `doc`, as json_parse does a text: nodes and strings from
// `alloc`, which the caller owns (an arena, usually); the reader's own bookkeeping is
// freed before it returns. False with doc.error and doc.error_at (a byte offset; there are
// no lines) on failure, and the root then a lone null. Nodes have no line (0).
bool kbor_parse(const void* data, usize size, Allocator* alloc, JsonDoc& doc, KborInfo* info = nullptr);

// Appends the tree as normalized KBOR to `out`. The summary it wrote, when asked for.
void kbor_write(const JsonDoc& doc, Array<u8>& out, KborSummary* summary = nullptr);

} // namespace kv
