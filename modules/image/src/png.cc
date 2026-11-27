#include "korvid/image/png.h"
#include "korvid/core/arena.h"
#include "korvid/inflate/inflate.h"
#include <string.h>

namespace kv {

namespace {

u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

u8 paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (u8)(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}

bool fail(const char** error, const char* why) { if (error) *error = why; return false; }

} // namespace

bool png_decode(const void* data, usize size, Allocator* pixels, PngImage& out, const char** error, bool indexes) {
    const u8* p = (const u8*)data;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (size < 8 + 25 || memcmp(p, sig, 8) != 0) return fail(error, "not a PNG");

    // First pass over the chunks: header, palette, transparency, total IDAT bytes.
    u32 w = 0, h = 0; u8 depth = 0, ctype = 0, interlace = 0;
    const u8* plte = nullptr; u32 plte_n = 0;
    const u8* trns = nullptr; u32 trns_n = 0;
    usize idat_total = 0;
    bool seen_ihdr = false, seen_iend = false;
    for (usize at = 8; at + 8 <= size && !seen_iend;) {
        u32 len = be32(p + at);
        const u8* tag = p + at + 4;
        const u8* body = p + at + 8;
        if (len > size - at - 8 || len > (1u << 30)) return fail(error, "chunk runs past the end");
        if (memcmp(tag, "IHDR", 4) == 0) {
            if (len != 13) return fail(error, "bad IHDR");
            w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12];
            seen_ihdr = true;
        } else if (memcmp(tag, "PLTE", 4) == 0) { plte = body; plte_n = len / 3; }
        else if (memcmp(tag, "tRNS", 4) == 0) { trns = body; trns_n = len; }
        else if (memcmp(tag, "IDAT", 4) == 0) idat_total += len;
        else if (memcmp(tag, "IEND", 4) == 0) seen_iend = true;
        at += 12 + (usize)len; // length, tag, body, crc (not verified)
    }
    if (!seen_ihdr || !seen_iend) return fail(error, "missing IHDR or IEND");
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return fail(error, "unsupported size");
    // 8-bit samples, or 1, 2 and 4 bits for gray and palette images (packed many to a
    // byte, most significant first): what pixel-art packs save as.
    bool packed = depth == 1 || depth == 2 || depth == 4;
    if (depth != 8 && !(packed && (ctype == 0 || ctype == 3))) return fail(error, "only 8-bit samples (or 1, 2 and 4 bits of gray or palette) are supported");
    if (interlace) return fail(error, "interlaced PNGs are not supported");
    u32 channels;
    switch (ctype) {
    case 0: channels = 1; break; // gray
    case 2: channels = 3; break; // rgb
    case 3: channels = 1; if (!plte) return fail(error, "palette image without PLTE"); break;
    case 4: channels = 2; break; // gray + alpha
    case 6: channels = 4; break; // rgba
    default: return fail(error, "unknown color type");
    }
    if (idat_total == 0) return fail(error, "no image data");

    MemScope scope;
    Arena& scratch = *scope.arena;
    u8* idat = arena_push_n<u8>(scratch, (u32)idat_total);
    if (!idat) return fail(error, "out of scratch");
    usize off = 0;
    for (usize at = 8; at + 8 <= size;) {
        u32 len = be32(p + at);
        if (memcmp(p + at + 4, "IDAT", 4) == 0) { memcpy(idat + off, p + at + 8, len); off += len; }
        if (memcmp(p + at + 4, "IEND", 4) == 0) break;
        at += 12 + (usize)len;
    }

    usize stride = packed ? ((usize)w * depth + 7) / 8 : (usize)w * channels;
    usize raw_len = (stride + 1) * h;
    u8* raw = arena_push_n<u8>(scratch, (u32)raw_len);
    if (!raw) return fail(error, "out of scratch");
    InflateOut z = inflate_zlib(idat, idat_total, raw, raw_len);
    if (z.result != InflateResult::Ok || z.produced != raw_len) return fail(error, "corrupt image data");

    // Unfilter in place, one row at a time, against the previous unfiltered row. Packed
    // samples filter by the byte.
    u32 bpp = packed ? 1 : channels;
    for (u32 y = 0; y < h; ++y) {
        u8* row = raw + y * (stride + 1);
        u8 filter = row[0];
        u8* cur = row + 1;
        const u8* prev = y ? raw + (y - 1) * (stride + 1) + 1 : nullptr;
        for (usize i = 0; i < stride; ++i) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[i] = (u8)(cur[i] + a); break;
            case 2: cur[i] = (u8)(cur[i] + b); break;
            case 3: cur[i] = (u8)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (u8)(cur[i] + paeth(a, b, c)); break;
            default: return fail(error, "bad filter type");
            }
        }
    }

    Allocator* pa = pixels ? pixels : mem_general();
    usize out_len = (usize)w * h * 4;
    u8* rgba = (u8*)alloc_in(pa, out_len);
    if (!rgba) return fail(error, "out of memory");
    usize index_len = (usize)w * h;
    u8* index = indexes && ctype == 3 ? (u8*)alloc_in(pa, index_len) : nullptr;
    if (indexes && ctype == 3 && !index) { free_in(pa, rgba, out_len); return fail(error, "out of memory"); }
    auto bad_index = [&] { free_in(pa, rgba, out_len); if (index) free_in(pa, index, index_len); return fail(error, "palette index out of range"); };
    for (u32 y = 0; y < h; ++y) {
        const u8* src = raw + y * (stride + 1) + 1;
        u8* dst = rgba + (usize)y * w * 4;
        if (packed) {
            u32 per = 8 / depth, mask = (1u << depth) - 1;
            for (u32 x = 0; x < w; ++x, dst += 4) {
                u32 v = (src[x / per] >> (8 - depth * (x % per + 1))) & mask;
                if (ctype == 0) { dst[0] = dst[1] = dst[2] = (u8)(v * 255 / mask); dst[3] = 255; continue; }
                if (v >= plte_n) return bad_index();
                if (index) index[(usize)y * w + x] = (u8)v;
                dst[0] = plte[v * 3]; dst[1] = plte[v * 3 + 1]; dst[2] = plte[v * 3 + 2];
                dst[3] = v < trns_n ? trns[v] : 255;
            }
            continue;
        }
        for (u32 x = 0; x < w; ++x, dst += 4, src += channels) {
            switch (ctype) {
            case 0: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; break;
            case 2: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; break;
            case 3: {
                u32 i = src[0];
                if (i >= plte_n) return bad_index();
                if (index) index[(usize)y * w + x] = (u8)i;
                dst[0] = plte[i * 3]; dst[1] = plte[i * 3 + 1]; dst[2] = plte[i * 3 + 2];
                dst[3] = i < trns_n ? trns[i] : 255;
                break;
            }
            case 4: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; break;
            default: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; break;
            }
        }
    }
    out.width  = w;
    out.height = h;
    out.rgba   = rgba;
    out.index  = index;
    return true;
}

// ---- encoding ------------------------------------------------------------------------------

namespace {

void put_be32(Array<u8>& out, u32 v) {
    u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
    for (u8 x : b) out.push(x);
}

// One chunk: length, tag, body, and the CRC-32 of tag and body.
void put_chunk(Array<u8>& out, const char* tag, const u8* body, u32 len) {
    put_be32(out, len);
    for (u32 i = 0; i < 4; ++i) out.push((u8)tag[i]);
    if (len) { u8* at = out.push_n(len); memcpy(at, body, len); }
    put_be32(out, crc32(crc32(0, tag, 4), body, len));
}

// Row `line` filtered by `type` into `dst`; `prev` is the row above (zeros for the first).
void filter_row(u8 type, const u8* line, const u8* prev, u32 stride, u32 bpp, u8* dst) {
    for (u32 i = 0; i < stride; ++i) {
        int a = i >= bpp ? line[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        int pred = 0;
        switch (type) {
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) >> 1; break;
        case 4: pred = paeth(a, b, c); break;
        default: break;
        }
        dst[i] = (u8)(line[i] - pred);
    }
}

} // namespace

bool png_encode(const u8* pixels, u32 width, u32 height, u32 channels, Array<u8>& out, const char** error) {
    if (channels != 3 && channels != 4) return fail(error, "only RGB or RGBA");
    if (width == 0 || height == 0 || width > (1u << 24) || height > (1u << 24)) return fail(error, "bad size");
    u64 stride64 = (u64)width * channels, raw64 = (stride64 + 1) * height;
    if (raw64 > (1ull << 31)) return fail(error, "too large");
    u32 stride = (u32)stride64;
    usize raw_len = (usize)raw64;

    MemScope scope;
    Arena& scratch = *scope.arena;
    u8* raw = arena_push_n<u8>(scratch, (u32)raw_len);
    u8* trial = arena_push_n<u8>(scratch, stride * 5);
    u8* zeros = arena_push_n<u8>(scratch, stride);
    void* state = arena_alloc(scratch, KV_DEFLATE_STATE_BYTES, 8);
    usize cap = deflate_bound(raw_len);
    u8* packed = (u8*)arena_alloc(scratch, cap, 8);
    if (!raw || !trial || !zeros || !state || !packed) return fail(error, "out of scratch");
    memset(zeros, 0, stride);

    // Each row by whichever filter leaves the smallest sum of absolute (signed) bytes.
    for (u32 y = 0; y < height; ++y) {
        const u8* line = pixels + (usize)y * stride;
        const u8* prev = y ? line - stride : zeros;
        u32 best = 0;
        u64 best_sum = ~0ull;
        for (u8 t = 0; t < 5; ++t) {
            u8* dst = trial + (usize)t * stride;
            filter_row(t, line, prev, stride, channels, dst);
            u64 sum = 0;
            for (u32 i = 0; i < stride; ++i) sum += dst[i] < 128 ? dst[i] : 256 - dst[i];
            if (sum < best_sum) { best_sum = sum; best = t; }
        }
        u8* row = raw + (usize)y * (stride + 1);
        row[0] = (u8)best;
        memcpy(row + 1, trial + (usize)best * stride, stride);
    }
    usize n = deflate_zlib(raw, raw_len, packed, cap, state, 10);
    if (n == 0 || n > 0x7FFFFFFFu) return fail(error, "deflate failed");

    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    u8 ihdr[13] = {(u8)(width >> 24), (u8)(width >> 16), (u8)(width >> 8), (u8)width,
                   (u8)(height >> 24), (u8)(height >> 16), (u8)(height >> 8), (u8)height,
                   8, (u8)(channels == 4 ? 6 : 2), 0, 0, 0};
    out.reserve(out.len + 8 + 25 + 12 + (u32)n + 12);
    u8* at = out.push_n(8);
    memcpy(at, sig, 8);
    put_chunk(out, "IHDR", ihdr, 13);
    put_chunk(out, "IDAT", packed, (u32)n);
    put_chunk(out, "IEND", nullptr, 0);
    return true;
}

} // namespace kv
