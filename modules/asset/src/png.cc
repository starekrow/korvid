#include "korvid/asset/png.h"
#include "korvid/core/arena.h"
#include <string.h>
#include <zlib.h>

namespace kv {

namespace {

u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

u8 paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (u8)(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}

bool fail(const char** error, const char* why) { if (error) *error = why; return false; }

} // namespace

bool png_decode(const void* data, usize size, Allocator* pixels, PngImage& out, const char** error) {
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
    if (depth != 8) return fail(error, "only 8-bit samples are supported");
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

    usize stride = (usize)w * channels;
    usize raw_len = (stride + 1) * h;
    u8* raw = arena_push_n<u8>(scratch, (u32)raw_len);
    if (!raw) return fail(error, "out of scratch");
    uLongf got = (uLongf)raw_len;
    int z = uncompress(raw, &got, idat, (uLong)idat_total);
    if (z != Z_OK || got != raw_len) return fail(error, "corrupt image data");

    // Unfilter in place, one row at a time, against the previous unfiltered row.
    u32 bpp = channels;
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
    for (u32 y = 0; y < h; ++y) {
        const u8* src = raw + y * (stride + 1) + 1;
        u8* dst = rgba + (usize)y * w * 4;
        for (u32 x = 0; x < w; ++x, dst += 4, src += channels) {
            switch (ctype) {
            case 0: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; break;
            case 2: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; break;
            case 3: {
                u32 i = src[0];
                if (i >= plte_n) { free_in(pa, rgba, out_len); return fail(error, "palette index out of range"); }
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
    return true;
}

} // namespace kv
