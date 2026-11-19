// tinfl and tdefl, once, and the Korvid API over them. The decompressor, the compressor
// (for writers: PNG) and mz_crc32 are built: everything that allocates, touches files or
// knows zip archives is configured out, so nothing of miniz reaches the rest of the engine
// but this file.
#include "korvid/inflate/inflate.h"
#include "korvid/core/bytes.h"
#include <string.h>

// miniz is vendored, not ours: its warnings are silenced for its own lines only.
#if KV_COMPILER_MSVC
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wtype-limits"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wclass-memaccess" // gcc's alone; clang errors on a group it does not know
#endif
#endif
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_MALLOC
#include "vendor/miniz.c"
#if KV_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace kv {

static_assert(sizeof(tinfl_decompressor) <= KV_INFLATE_STATE_BYTES, "KV_INFLATE_STATE_BYTES is too small for tinfl");

namespace {

InflateResult from_status(tinfl_status st) {
    switch (st) {
    case TINFL_STATUS_DONE:                        return InflateResult::Ok;
    case TINFL_STATUS_HAS_MORE_OUTPUT:             return InflateResult::Overflow;
    case TINFL_STATUS_NEEDS_MORE_INPUT:
    case TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS: return InflateResult::Truncated;
    case TINFL_STATUS_ADLER32_MISMATCH:            return InflateResult::BadChecksum;
    default:                                       return InflateResult::Corrupt;
    }
}

// One call over the whole input into the whole output.
InflateOut run(const u8* src, usize src_len, u8* dst, usize dst_cap, u32 flags) {
    tinfl_decompressor d;
    tinfl_init(&d);
    usize in = src_len, out = dst_cap;
    tinfl_status st = tinfl_decompress(&d, src, &in, dst, dst, &out, flags | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    InflateOut r;
    r.result   = from_status(st);
    r.consumed = in;
    r.produced = out;
    // A full output buffer with the stream not yet ended reads as "more output".
    if (r.result == InflateResult::Truncated && out == dst_cap && dst_cap) r.result = InflateResult::Overflow;
    return r;
}

bool zlib_header_ok(const u8* p, usize n) {
    if (n < 2) return false;
    u32 cmf = p[0], flg = p[1];
    return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20); // deflate, window <= 32K, no preset dictionary
}

} // namespace

InflateOut inflate_raw(const void* src, usize src_len, void* dst, usize dst_cap) {
    return run((const u8*)src, src_len, (u8*)dst, dst_cap, 0);
}

InflateOut inflate_zlib(const void* src, usize src_len, void* dst, usize dst_cap) {
    if (!zlib_header_ok((const u8*)src, src_len)) { InflateOut r; r.result = src_len < 2 ? InflateResult::Truncated : InflateResult::BadHeader; return r; }
    return run((const u8*)src, src_len, (u8*)dst, dst_cap, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32);
}

InflateOut inflate_gzip(const void* src, usize src_len, void* dst, usize dst_cap) {
    const u8* p = (const u8*)src;
    InflateOut r;
    if (src_len < 18) { r.result = InflateResult::Truncated; return r; }
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8 || (p[3] & 0xE0)) { r.result = InflateResult::BadHeader; return r; }
    u8 flg = p[3];
    usize at = 10;
    if (flg & 0x04) { // FEXTRA
        if (at + 2 > src_len) { r.result = InflateResult::Truncated; return r; }
        at += 2 + ((usize)p[at] | (usize)p[at + 1] << 8);
    }
    const u8 strings[] = {0x08, 0x10}; // FNAME, FCOMMENT: NUL-terminated
    for (u8 bit : strings) {
        if (!(flg & bit)) continue;
        while (at < src_len && p[at]) ++at;
        ++at;
    }
    if (flg & 0x02) at += 2; // FHCRC
    if (at + 8 > src_len) { r.result = InflateResult::Truncated; return r; }
    r = run(p + at, src_len - at - 8, (u8*)dst, dst_cap, 0);
    r.consumed += at;
    if (r.result != InflateResult::Ok) return r;
    if (r.consumed + 8 > src_len) { r.result = InflateResult::Truncated; return r; }
    const u8* trailer = p + r.consumed;
    if (load_le32(trailer) != crc32(0, dst, r.produced) || load_le32(trailer + 4) != (u32)r.produced) r.result = InflateResult::BadChecksum;
    r.consumed += 8;
    return r;
}

const char* inflate_result_name(InflateResult r) {
    static const char* const names[] = {"ok", "truncated", "output overflow", "corrupt", "bad header", "bad checksum"};
    return (u32)r < KV_COUNTOF(names) ? names[(u32)r] : "?";
}

u32 crc32(u32 crc, const void* data, usize len) {
    return (u32)mz_crc32(crc, (const unsigned char*)data, len);
}

void inflate_stream_init(InflateStream& s, bool zlib, void* dst, usize dst_cap) {
    tinfl_init((tinfl_decompressor*)s.state);
    s.out      = (u8*)dst;
    s.out_cap  = dst_cap;
    s.produced = 0;
    s.flags    = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32 : 0);
    s.done     = false;
}

InflateOut inflate_stream(InflateStream& s, const void* src, usize src_len, bool more) {
    InflateOut r;
    if (s.done) { r.result = InflateResult::Ok; return r; }
    usize in = src_len, out = s.out_cap - s.produced;
    tinfl_status st = tinfl_decompress((tinfl_decompressor*)s.state, (const u8*)src, &in, s.out, s.out + s.produced, &out,
                                       s.flags | (more ? TINFL_FLAG_HAS_MORE_INPUT : 0));
    s.produced += out;
    r.result   = from_status(st);
    r.consumed = in;
    r.produced = out;
    if (r.result == InflateResult::Ok) s.done = true;
    return r;
}

// ---- deflate -------------------------------------------------------------------------------

static_assert(sizeof(tdefl_compressor) <= KV_DEFLATE_STATE_BYTES, "KV_DEFLATE_STATE_BYTES is too small for tdefl");

namespace {

struct DeflateSink {
    u8*   dst;
    usize cap;
    usize len;
    bool  overflow;
};

mz_bool deflate_put(const void* buf, int len, void* user) {
    DeflateSink& s = *(DeflateSink*)user;
    if (len < 0 || s.len + (usize)len > s.cap) { s.overflow = true; return MZ_FALSE; }
    memcpy(s.dst + s.len, buf, (usize)len);
    s.len += (usize)len;
    return MZ_TRUE;
}

} // namespace

// Stored blocks are the worst case: five bytes of header per 65,535 of data, plus the
// zlib header and trailer; the slack covers tdefl's own block boundaries.
usize deflate_bound(usize src_len) { return src_len + (src_len / 16383 + 1) * 5 + 64; }

usize deflate_zlib(const void* src, usize src_len, void* dst, usize dst_cap, void* state, int level) {
    if (!state || (!src && src_len)) return 0;
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    DeflateSink sink{(u8*)dst, dst_cap, 0, false};
    // zlib framing (a 15-bit window, the default strategy), as inflate_zlib reads it.
    int flags = (int)tdefl_create_comp_flags_from_zip_params(level, 15, MZ_DEFAULT_STRATEGY);
    tdefl_compressor* d = (tdefl_compressor*)state;
    if (tdefl_init(d, deflate_put, &sink, flags) != TDEFL_STATUS_OKAY) return 0;
    tdefl_status st = tdefl_compress_buffer(d, src, src_len, TDEFL_FINISH);
    if (st != TDEFL_STATUS_DONE || sink.overflow) return 0;
    return sink.len;
}

} // namespace kv
