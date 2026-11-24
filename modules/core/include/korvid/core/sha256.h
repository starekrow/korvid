// Korvid core: SHA-256 (FIPS 180-4). For content: what a file is, whether two are the
// same, what a cooked thing was made from. Not the engine's table hash (hash.h), which is
// fast and 64 bits; this one is for identity that has to hold across machines and years.
// Plain code with no state of its own: any thread may hash, which is the point.
//
// sha256() hashes one buffer. A Sha256 hashes data that arrives in pieces, a file read a
// block at a time, and gives the same digest as sha256() over the pieces joined:
//
//   Sha256 h;
//   h.init();
//   h.update(first, first_size);    // once for each piece, in order
//   h.update(second, second_size);
//   h.finish(digest);               // u8 digest[32]
//
// modules/platform/src/io.cc (io_copy_file) hashes a file this way as it copies it.
#pragma once
#include "korvid/core/types.h"

namespace kv {

// A hash in progress. Plain data with no constructor: init() before the first update().
struct Sha256 {
    u32 h[8];     // the state so far
    u8  buf[64];  // bytes waiting for a whole 64-byte block
    u32 fill;     // how many of buf are waiting
    u64 total;    // bytes hashed, the pieces together

    // Starts (or restarts) a hash.
    void init();
    // Hashes `size` more bytes at `data`.
    void update(const void* data, usize size);
    // Writes the digest, 32 bytes, to `out`.
    void finish(u8 out[32]); // the object is spent afterwards: init() to reuse it
};

// The digest of `size` bytes at `data`, as 32 bytes.
void sha256(const void* data, usize size, u8 out[32]);
// 64 lowercase hex digits and a terminator.
void sha256_hex(const u8 digest[32], char out[65]);

} // namespace kv
