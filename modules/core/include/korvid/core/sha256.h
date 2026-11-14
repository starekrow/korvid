// Korvid core: SHA-256 (FIPS 180-4). For content: what a file is, whether two are the
// same, what a cooked thing was made from. Not the engine's table hash (hash.h), which is
// fast and 64 bits; this one is for identity that has to hold across machines and years.
// Plain code with no state of its own: any thread may hash, which is the point.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Sha256 {
    u32 h[8];
    u8  buf[64];
    u32 fill;
    u64 total;

    void init();
    void update(const void* data, usize size);
    void finish(u8 out[32]); // the object is spent afterwards: init() to reuse it
};

void sha256(const void* data, usize size, u8 out[32]);
// 64 lowercase hex digits and a terminator.
void sha256_hex(const u8 digest[32], char out[65]);

} // namespace kv
