#include "korvid/core/sha256.h"
#include <string.h>

namespace kv {

namespace {

constexpr u32 kShaK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 sha_rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

void sha_block(u32 h[8], const u8* p) {
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (u32 i = 16; i < 64; ++i) {
        u32 s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (u32 i = 0; i < 64; ++i) {
        u32 t1 = hh + (sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25)) + ((e & f) ^ (~e & g)) + kShaK[i] + w[i];
        u32 t2 = (sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

} // namespace

void Sha256::init() {
    static const u32 start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, start, sizeof h);
    fill  = 0;
    total = 0;
}

void Sha256::update(const void* data, usize size) {
    const u8* p = (const u8*)data;
    total += size;
    if (fill) {
        usize take = 64 - fill < size ? 64 - fill : size;
        memcpy(buf + fill, p, take);
        fill += (u32)take;
        p += take;
        size -= take;
        if (fill < 64) return;
        sha_block(h, buf);
        fill = 0;
    }
    for (; size >= 64; p += 64, size -= 64) sha_block(h, p);
    if (size) { memcpy(buf, p, size); fill = (u32)size; }
}

void Sha256::finish(u8 out[32]) {
    u8 tail[128] = {};
    memcpy(tail, buf, fill);
    tail[fill] = 0x80;
    usize len = fill + 1 + 8 <= 64 ? 64 : 128;
    u64 bits = total * 8;
    for (u32 i = 0; i < 8; ++i) tail[len - 1 - i] = (u8)(bits >> (8 * i));
    sha_block(h, tail);
    if (len == 128) sha_block(h, tail + 64);
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < 4; ++j) out[i * 4 + j] = (u8)(h[i] >> (24 - 8 * j));
}

void sha256(const void* data, usize size, u8 out[32]) {
    Sha256 s;
    s.init();
    s.update(data, size);
    s.finish(out);
}

void sha256_hex(const u8 digest[32], char out[65]) {
    for (u32 i = 0; i < 32; ++i) {
        out[i * 2]     = "0123456789abcdef"[digest[i] >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[digest[i] & 15];
    }
    out[64] = 0;
}

} // namespace kv
