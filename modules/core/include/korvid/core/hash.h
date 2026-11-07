// Korvid core: hashing. One fixed, unseeded 64-bit hash, identical on every platform,
// so cooked data and replays agree with the running engine. Never randomized: a seeded
// hash is a reproducibility bug here, whatever it buys elsewhere.
//
// Map<K, V> calls hash_key(key); overload it for your own plain-data key types.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/core/types.h"

namespace kv {

// Integer mixer (splitmix64 finalizer): spreads structured bits such as a handle's.
inline u64 hash_mix(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

// Bytes, little-endian 8 at a time. Platform independent by construction.
u64 hash_bytes(const void* data, usize size);
u64 hash_str(const char* s); // strlen, then hash_bytes

inline u64 hash_key(u64 k)    { return hash_mix(k); }
inline u64 hash_key(u32 k)    { return hash_mix(k); }
inline u64 hash_key(i64 k)    { return hash_mix((u64)k); }
inline u64 hash_key(i32 k)    { return hash_mix((u64)(u32)k); }
inline u64 hash_key(Handle h) { return hash_mix(h.bits); }

} // namespace kv
