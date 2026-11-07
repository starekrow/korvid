#include "korvid/core/hash.h"
#include <string.h>

namespace kv {

namespace {

u64 read_le64(const u8* p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i) v |= (u64)p[i] << (8 * i);
    return v;
}

} // namespace

u64 hash_bytes(const void* data, usize size) {
    const u8* p = (const u8*)data;
    u64 h = 0x9e3779b97f4a7c15ull ^ (size * 0xff51afd7ed558ccdull);
    while (size >= 8) {
        h = hash_mix(h ^ read_le64(p));
        p += 8;
        size -= 8;
    }
    if (size) {
        u8 tail[8] = {};
        memcpy(tail, p, size);
        h = hash_mix(h ^ read_le64(tail) ^ ((u64)size << 56));
    }
    return hash_mix(h);
}

u64 hash_str(const char* s) { return hash_bytes(s, s ? strlen(s) : 0); }

} // namespace kv
