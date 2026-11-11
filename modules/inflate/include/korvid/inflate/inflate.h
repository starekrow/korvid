// Korvid inflate: deflate decompression and CRC-32, over miniz's tinfl (vendored, MIT).
// Nothing here allocates: the caller supplies the output buffer (sized from the format's
// own record of the uncompressed size: a zip entry's, a PNG's dimensions, a gzip trailer)
// and, for the streaming form, the decompressor state.
//
//   InflateOut r = inflate_raw(entry_bytes, compressed_size, out, uncompressed_size);
//   if (r.result != InflateResult::Ok || r.produced != uncompressed_size) ... corrupt
//
// Three framings of the same stream: raw deflate (RFC 1951, what a zip entry holds), zlib
// (RFC 1950: a two-byte header and an Adler-32 trailer; PNG's IDAT, Tiled's "zlib"
// layers) and gzip (RFC 1952: a header with optional name and comment, a CRC-32 and
// length trailer; Tiled's "gzip" layers). The checksums are checked.
#pragma once
#include "korvid/core/types.h"

namespace kv {

enum class InflateResult : u8 {
    Ok,          // the stream ended and everything checked out
    Truncated,   // the input ended before the stream did
    Overflow,    // the output buffer filled before the stream ended
    Corrupt,     // the deflate data is invalid
    BadHeader,   // a zlib or gzip header that is not one
    BadChecksum, // the data decoded but the trailer's checksum or length disagrees
    Count
};

struct InflateOut {
    InflateResult result   = InflateResult::Corrupt;
    usize         consumed = 0; // input bytes used, the trailer included
    usize         produced = 0; // output bytes written
};

InflateOut  inflate_raw(const void* src, usize src_len, void* dst, usize dst_cap);
InflateOut  inflate_zlib(const void* src, usize src_len, void* dst, usize dst_cap);
InflateOut  inflate_gzip(const void* src, usize src_len, void* dst, usize dst_cap);
// "ok", "truncated", ...
const char* inflate_result_name(InflateResult r);

// CRC-32 (the zip, gzip and PNG one: polynomial 0xEDB88320, reflected). Start with 0 and
// feed the previous result back to continue: crc32(crc32(0, a, n), b, m) == crc32(0, ab, n + m).
u32 crc32(u32 crc, const void* data, usize len);

// Streaming: the input arrives in pieces (chunked reads), the output is one buffer the
// size of the whole result. Init, then feed each piece with `more` true until the last;
// a result of Truncated with `more` true means "give me the next piece". Raw or zlib
// framing.
constexpr usize KV_INFLATE_STATE_BYTES = 11264; // tinfl_decompressor, checked at compile time
struct InflateStream {
    alignas(8) u8 state[KV_INFLATE_STATE_BYTES];
    u8*   out      = nullptr;
    usize out_cap  = 0;
    usize produced = 0;
    u32   flags    = 0;
    bool  done     = false;
};
void       inflate_stream_init(InflateStream& s, bool zlib, void* dst, usize dst_cap);
InflateOut inflate_stream(InflateStream& s, const void* src, usize src_len, bool more);

// Compression, for writers (a PNG's IDAT): miniz's tdefl, zlib framing (RFC 1950: what
// inflate_zlib reads). As above, nothing is allocated here: the caller supplies the
// compressor's state, KV_DEFLATE_STATE_BYTES (about 320 KB: from an allocator, not the
// stack), and the output, which deflate_bound(src_len) bytes always hold. `level` is 0
// (stored blocks) to 10 (smallest); 6 is zlib's default. Returns the bytes written, or 0
// when they did not fit in dst_cap.
constexpr usize KV_DEFLATE_STATE_BYTES = 320 * 1024; // tdefl_compressor, checked at compile time
usize deflate_bound(usize src_len);
usize deflate_zlib(const void* src, usize src_len, void* dst, usize dst_cap, void* state, int level = 6);

} // namespace kv
