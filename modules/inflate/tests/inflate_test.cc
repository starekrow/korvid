// The three framings against Python's zlib; stored blocks; an empty stream; every way to
// fail (a cut stream, a small buffer, flipped bits, a bad header, a bad checksum); the
// streaming form fed one byte at a time and in pieces; CRC-32's check value.
#include "korvid/inflate/inflate.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include "streams.h"
#include <string.h>

namespace {

using namespace kv;

constexpr usize kPlainLen = 40 * 35 * 2 + 3000;
u8 g_plain[kPlainLen];

void test_plain() {
    usize at = 0;
    const char* s = "Korvid inflates what deflate made. ";
    for (u32 i = 0; i < 40; ++i) { memcpy(g_plain + at, s, 35); at += 35; }
    u32 x = 7;
    for (u32 i = 0; i < 3000; ++i) { x = (x * 1103515245u + 12345u) & 0x7FFFFFFFu; g_plain[at++] = (u8)((x >> 16) & 255); }
    for (u32 i = 0; i < 40; ++i) { memcpy(g_plain + at, s, 35); at += 35; }
}

u8 g_out[kPlainLen + 64];

} // namespace

void test_inflate(kv::TestCtx& c) {
    using namespace kv;
    c.begin("inflate");
    test_plain();

    InflateOut r = inflate_raw(k_raw, sizeof k_raw, g_out, sizeof g_out);
    KV_CHECK(c, r.result == InflateResult::Ok && r.produced == kPlainLen && r.consumed == sizeof k_raw && memcmp(g_out, g_plain, kPlainLen) == 0);
    memset(g_out, 0, sizeof g_out);
    r = inflate_raw(k_stored, sizeof k_stored, g_out, sizeof g_out);
    KV_CHECK(c, r.result == InflateResult::Ok && r.produced == kPlainLen && memcmp(g_out, g_plain, kPlainLen) == 0);
    memset(g_out, 0, sizeof g_out);
    r = inflate_zlib(k_zlib, sizeof k_zlib, g_out, sizeof g_out);
    KV_CHECK(c, r.result == InflateResult::Ok && r.produced == kPlainLen && r.consumed == sizeof k_zlib && memcmp(g_out, g_plain, kPlainLen) == 0);
    memset(g_out, 0, sizeof g_out);
    r = inflate_gzip(k_gzip, sizeof k_gzip, g_out, sizeof g_out);
    KV_CHECK(c, r.result == InflateResult::Ok && r.produced == kPlainLen && r.consumed == sizeof k_gzip && memcmp(g_out, g_plain, kPlainLen) == 0);
    r = inflate_raw(k_empty_raw, sizeof k_empty_raw, g_out, sizeof g_out);
    KV_CHECK(c, r.result == InflateResult::Ok && r.produced == 0);
    // An exact-size buffer is enough.
    KV_CHECK(c, inflate_raw(k_raw, sizeof k_raw, g_out, kPlainLen).result == InflateResult::Ok);

    // Failures.
    KV_CHECK(c, inflate_raw(k_raw, sizeof k_raw / 2, g_out, sizeof g_out).result == InflateResult::Truncated);
    KV_CHECK(c, inflate_raw(k_raw, sizeof k_raw, g_out, kPlainLen - 1).result == InflateResult::Overflow);
    KV_CHECK(c, inflate_zlib(k_raw, sizeof k_raw, g_out, sizeof g_out).result == InflateResult::BadHeader);
    KV_CHECK(c, inflate_gzip(k_zlib, sizeof k_zlib, g_out, sizeof g_out).result == InflateResult::BadHeader);
    KV_CHECK(c, inflate_zlib(k_zlib, 1, g_out, sizeof g_out).result == InflateResult::Truncated);
    static u8 bad[sizeof k_zlib];
    memcpy(bad, k_zlib, sizeof bad);
    bad[sizeof bad - 1] ^= 1; // the Adler-32
    KV_CHECK(c, inflate_zlib(bad, sizeof bad, g_out, sizeof g_out).result == InflateResult::BadChecksum);
    static u8 badg[sizeof k_gzip];
    memcpy(badg, k_gzip, sizeof badg);
    badg[sizeof badg - 5] ^= 1; // the CRC-32
    KV_CHECK(c, inflate_gzip(badg, sizeof badg, g_out, sizeof g_out).result == InflateResult::BadChecksum);
    // Garbage never reads or writes out of bounds, whatever it decodes to.
    static u8 junk[512];
    u32 x = 99;
    for (u32 trial = 0; trial < 200; ++trial) {
        for (u8& b : junk) { x = x * 1664525u + 1013904223u; b = (u8)(x >> 24); }
        InflateOut j = inflate_raw(junk, sizeof junk, g_out, 1024);
        KV_CHECK(c, j.produced <= 1024 && j.consumed <= sizeof junk);
    }

    // Streaming: one byte at a time, then in pieces of 100.
    InflateStream s;
    inflate_stream_init(s, false, g_out, sizeof g_out);
    InflateOut last;
    usize fed = 0;
    for (; fed < sizeof k_raw; ++fed) {
        last = inflate_stream(s, k_raw + fed, 1, fed + 1 < sizeof k_raw);
        KV_CHECK(c, last.consumed == 1 || last.result == InflateResult::Ok);
        if (last.result != InflateResult::Truncated) break;
    }
    KV_CHECK(c, last.result == InflateResult::Ok && s.produced == kPlainLen && memcmp(g_out, g_plain, kPlainLen) == 0);
    memset(g_out, 0, sizeof g_out);
    inflate_stream_init(s, true, g_out, sizeof g_out);
    for (usize at = 0; at < sizeof k_zlib; at += 100) {
        usize n = sizeof k_zlib - at < 100 ? sizeof k_zlib - at : 100;
        last = inflate_stream(s, k_zlib + at, n, at + n < sizeof k_zlib);
    }
    KV_CHECK(c, last.result == InflateResult::Ok && s.produced == kPlainLen && memcmp(g_out, g_plain, kPlainLen) == 0);
    KV_CHECK(c, inflate_stream(s, k_zlib, 1, false).result == InflateResult::Ok); // done stays done

    // CRC-32: the standard check value, and continuation.
    KV_CHECK(c, crc32(0, "123456789", 9) == 0xCBF43926u);
    KV_CHECK(c, crc32(crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u && crc32(0, "", 0) == 0);
    KV_CHECK(c, strcmp(inflate_result_name(InflateResult::BadChecksum), "bad checksum") == 0);

    // Deflate: what it writes, inflate_zlib reads back exactly, at every level; the text
    // runs shrink, stored blocks do not; a buffer too small says so with 0; the bound
    // holds the worst case (incompressible bytes, stored).
    void* state = alloc_in(mem_general(), KV_DEFLATE_STATE_BYTES, 8);
    usize cap = deflate_bound(kPlainLen);
    u8* packed = (u8*)alloc_in(mem_general(), cap, 8);
    KV_CHECK(c, state && packed);
    usize sizes[3] = {};
    const int levels[3] = {0, 6, 10};
    for (u32 l = 0; l < 3; ++l) {
        usize n = deflate_zlib(g_plain, kPlainLen, packed, cap, state, levels[l]);
        sizes[l] = n;
        memset(g_out, 0, sizeof g_out);
        InflateOut back = inflate_zlib(packed, n, g_out, kPlainLen);
        KV_CHECK(c, n > 0 && back.result == InflateResult::Ok && back.produced == kPlainLen && memcmp(g_out, g_plain, kPlainLen) == 0);
    }
    KV_CHECK(c, sizes[0] > kPlainLen && sizes[1] < kPlainLen && sizes[2] <= sizes[1]);
    KV_CHECK(c, deflate_zlib(g_plain, kPlainLen, packed, 100, state, 6) == 0);
    KV_CHECK(c, deflate_zlib(g_plain + 2800, 3000, packed, deflate_bound(3000), state, 0) > 3000); // the noise, stored
    usize empty = deflate_zlib(nullptr, 0, packed, cap, state, 6);
    KV_CHECK(c, empty > 0 && inflate_zlib(packed, empty, g_out, 1).result == InflateResult::Ok);
    free_in(mem_general(), packed, cap);
    free_in(mem_general(), state, KV_DEFLATE_STATE_BYTES);
}
