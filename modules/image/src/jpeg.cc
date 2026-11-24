// JPEG decoding (jpeg.h). The file's markers are read into a frame (components, tables);
// each scan's entropy-coded data is decoded into a buffer of every block's coefficients,
// which progressive scans refine; then each block goes through the inverse DCT at the
// scale asked for into its component's plane, the planes are upsampled to the image's size,
// and the colour is converted. The inverse DCTs (libjpeg's jidctint.c and jidctred.c), the
// upsampling (jdsample.c) and the colour conversion (jdcolor.c) follow libjpeg's integer
// arithmetic, so the pixels are libjpeg-turbo's to within rounding.
#include "korvid/image/jpeg.h"
#include <string.h>

namespace kv {

// The decoder's own, in a namespace of its own: the module is built as one unity file, and
// png.cc and ppm.cc have their own helpers.
namespace jpg {

// Zigzag order to natural (row-major) order, with sixteen extra entries so that a
// corrupt run cannot index past the block.
constexpr u8 kNatural[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

bool fail(const char** error, const char* why) {
    if (error) *error = why;
    return false;
}

u8 clamp255(i32 v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

// ---- Huffman ------------------------------------------------------------------------------

constexpr u32 kFastBits = 9;

struct Huffman {
    bool present = false;
    u8   fast[1 << kFastBits];   // the symbol index for a code of up to kFastBits bits, 255: longer
    u8   size[257];              // code length by symbol index
    u16  code[256];
    u8   values[256];
    u32  maxcode[18];            // the largest code of each length, left-justified in 16 bits, +1
    i32  delta[17];              // symbol index minus code, by length
    // An AC code and its value together, when both fit in kFastBits: value << 8 | run << 4 |
    // the bits they take; 0 where they do not.
    i16  fast_ac[1 << kFastBits];
};

bool build_huffman(Huffman& h, const u8* counts, const u8* vals, u32 nvals) {
    u32 k = 0;
    for (u32 len = 1; len <= 16; ++len)
        for (u32 i = 0; i < counts[len - 1]; ++i) {
            if (k >= 256) return false;
            h.size[k++] = (u8)len;
        }
    if (k != nvals) return false;
    h.size[k] = 0;
    u32 code = 0;
    k = 0;
    for (u32 len = 1; len <= 16; ++len) {
        h.delta[len] = (i32)k - (i32)code;
        if (h.size[k] == len) {
            while (h.size[k] == len) h.code[k++] = (u16)code++;
            if (code - 1 >= (1u << len)) return false; // more codes than the length holds
        }
        h.maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    h.maxcode[17] = 0xffffffffu;
    memset(h.fast, 255, sizeof h.fast);
    for (u32 i = 0; i < k; ++i) {
        u32 s = h.size[i];
        if (s > kFastBits) continue;
        u32 c = (u32)h.code[i] << (kFastBits - s);
        u32 m = 1u << (kFastBits - s);
        for (u32 j = 0; j < m; ++j) h.fast[c + j] = (u8)i;
    }
    memcpy(h.values, vals, nvals);
    // The combined lookup (stb_image's idea): a run and a small magnitude in one step.
    for (u32 i = 0; i < (1u << kFastBits); ++i) {
        h.fast_ac[i] = 0;
        u32 k2 = h.fast[i];
        if (k2 == 255) continue;
        u32 rs = h.values[k2], run = rs >> 4, sz = rs & 15, len = h.size[k2];
        if (!sz || len + sz > kFastBits) continue;
        u32 bits = (i >> (kFastBits - len - sz)) & ((1u << sz) - 1);
        i32 v = bits < (1u << (sz - 1)) ? (i32)bits - (1 << sz) + 1 : (i32)bits;
        if (v < -128 || v > 127) continue;
        h.fast_ac[i] = (i16)((v * 256) + (i32)(run << 4) + (i32)(len + sz));
    }
    h.present = true;
    return true;
}

// ---- the bit reader ---------------------------------------------------------------------
// Bits MSB first from the entropy-coded data, a stuffed 0xFF00 read as 0xFF. At a marker,
// or past the end, it reads zeros and says so, leaving `p` at the marker.
struct Bits {
    const u8* p = nullptr;
    const u8* end = nullptr;
    u64  buf = 0;      // `count` bits, left-justified
    i32  count = 0;
    bool stopped = false; // a marker or the end was reached
    bool ended = false;   // the end, specifically: the file is truncated

    void fill() {
        while (count <= 56) {
            u32 b = 0;
            if (!stopped) {
                if (p >= end) { stopped = ended = true; }
                else if (*p == 0xFF) {
                    if (p + 1 >= end) { stopped = ended = true; p = end; }
                    else if (p[1] == 0x00) { b = 0xFF; p += 2; }
                    else stopped = true; // a marker: left for the scan's end
                } else b = *p++;
            }
            buf |= (u64)b << (56 - count);
            count += 8;
        }
    }
    u32 peek16() { if (count < 16) fill(); return (u32)(buf >> 48); }
    void skip(i32 n) { buf <<= n; count -= n; }
    u32 get(i32 n) { // n in [1, 16]
        if (count < n) fill();
        u32 v = (u32)(buf >> (64 - n));
        skip(n);
        return v;
    }
    u32 bit() { return get(1); }
    void reset() { buf = 0; count = 0; }
};

// A Huffman symbol, or -1 for a code no table has.
i32 decode(Bits& b, const Huffman& h) {
    u32 c = b.peek16();
    u32 k = h.fast[c >> (16 - kFastBits)];
    if (k < 255) {
        b.skip(h.size[k]);
        return h.values[k];
    }
    u32 len = kFastBits + 1;
    while (len <= 16 && c >= h.maxcode[len]) ++len;
    if (len > 16) { b.skip(16); return -1; }
    i32 idx = (i32)(c >> (16 - len)) + h.delta[len];
    if (idx < 0 || idx >= 256 || h.size[idx] != len) { b.skip((i32)len); return -1; }
    b.skip((i32)len);
    return h.values[idx];
}

// The value of an `s`-bit magnitude category.
i32 extend(u32 v, i32 s) { return v < (1u << (s - 1)) ? (i32)v - (1 << s) + 1 : (i32)v; }
i32 receive(Bits& b, i32 s) { return s ? extend(b.get(s), s) : 0; }

// ---- the frame ------------------------------------------------------------------------------

struct Component {
    u8   id = 0, h = 1, v = 1, tq = 0;
    u8   td = 0, ta = 0;           // this scan's tables
    u32  bw = 0, bh = 0;           // blocks, padded to whole MCUs
    u32  real_bw = 0, real_bh = 0; // blocks that hold image: a non-interleaved scan's extent
    i16* coef = nullptr;           // bw * bh * 64, natural order
    i32  dc = 0;
    // The plane after the inverse DCT: (bw * bs) x (bh * bs), and the part of it that is
    // image (libjpeg's downsampled_width and height at this scale).
    u8*  plane = nullptr;
    u32  pw = 0, ph = 0, dw = 0, dh = 0;
    u32  bs = 8;                   // its inverse DCT's size: the scale's, or larger for chroma (see jpeg_decode)
    // A 1x1 inverse DCT needs only each block's DC: a block keeps that one coefficient, a
    // sequential scan's AC coefficients are decoded and dropped, and a progressive file's AC
    // scans of it are not decoded at all.
    bool dc_only = false;
    u32  stride = 64;              // coefficients kept a block
};

struct Frame {
    u32  width = 0, height = 0;
    u32  ncomp = 0;
    bool progressive = false;
    u32  hmax = 1, vmax = 1, mcux = 0, mcuy = 0;
    Component comp[4];
    u16  quant[4][64];             // natural order
    bool quant_set[4] = {};
    Huffman dc[4], ac[4];
    u32  restart = 0;              // MCUs between restart markers; 0: none
    bool jfif = false, adobe = false;
    u8   adobe_transform = 0;
    u8   orientation = 1;
    u32  eobrun = 0;

    u32  rows_done = ~0u;          // sequential: the fewest MCU rows a spoiled scan decoded
    bool spoiled = false;          // a bad code, or the data ended
    bool frame_seen = false;
};

u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

// EXIF's orientation from an APP1 segment, if it is EXIF and has one.
void read_exif(Frame& f, const u8* p, u32 n) {
    if (n < 14 || memcmp(p, "Exif\0\0", 6) != 0) return;
    const u8* t = p + 6;
    u32 tn = n - 6;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;
    auto u16at = [&](u32 at) -> u32 { return at + 2 > tn ? 0 : le ? (u32)(t[at] | (t[at + 1] << 8)) : (u32)((t[at] << 8) | t[at + 1]); };
    auto u32at = [&](u32 at) -> u32 {
        if (at + 4 > tn) return 0;
        return le ? (u32)t[at] | ((u32)t[at + 1] << 8) | ((u32)t[at + 2] << 16) | ((u32)t[at + 3] << 24)
                  : ((u32)t[at] << 24) | ((u32)t[at + 1] << 16) | ((u32)t[at + 2] << 8) | (u32)t[at + 3];
    };
    if (u16at(2) != 42) return;
    u32 ifd = u32at(4);
    if (ifd + 2 > tn) return;
    u32 count = u16at(ifd);
    for (u32 i = 0; i < count && ifd + 2 + (i + 1) * 12 <= tn; ++i) {
        u32 e = ifd + 2 + i * 12;
        if (u16at(e) == 0x0112) {
            u32 v = u16at(e + 8);
            if (v >= 1 && v <= 8) f.orientation = (u8)v;
            return;
        }
    }
}

// The segment after a SOFn marker.
bool read_frame(Frame& f, const u8* p, u32 n, u8 marker, const char** error) {
    if (f.frame_seen) return fail(error, "more than one frame");
    if (n < 6) return fail(error, "corrupt frame header");
    if (p[0] != 8) return fail(error, "only 8-bit samples are supported");
    f.height = be16(p + 1);
    f.width  = be16(p + 3);
    f.ncomp  = p[5];
    f.progressive = marker == 0xC2;
    if (!f.width) return fail(error, "zero width");
    if (!f.height) return fail(error, "a height given later (DNL) is not supported");
    if (f.ncomp != 1 && f.ncomp != 3 && f.ncomp != 4) return fail(error, "unsupported component count");
    if (n < 6 + f.ncomp * 3) return fail(error, "corrupt frame header");
    for (u32 i = 0; i < f.ncomp; ++i) {
        Component& c = f.comp[i];
        c.id = p[6 + i * 3];
        c.h  = p[7 + i * 3] >> 4;
        c.v  = p[7 + i * 3] & 15;
        c.tq = p[8 + i * 3];
        if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return fail(error, "bad sampling factors");
        if (c.tq > 3) return fail(error, "bad quantization table");
        for (u32 j = 0; j < i; ++j) if (f.comp[j].id == c.id) return fail(error, "two components with one id");
        if (c.h > f.hmax) f.hmax = c.h;
        if (c.v > f.vmax) f.vmax = c.v;
    }
    for (u32 i = 0; i < f.ncomp; ++i)
        if (f.hmax % f.comp[i].h || f.vmax % f.comp[i].v) return fail(error, "sampling factors that do not divide the largest");
    if (f.ncomp == 1) { f.comp[0].h = f.comp[0].v = 1; f.hmax = f.vmax = 1; } // one component: no MCU of blocks
    f.mcux = (f.width + 8 * f.hmax - 1) / (8 * f.hmax);
    f.mcuy = (f.height + 8 * f.vmax - 1) / (8 * f.vmax);
    for (u32 i = 0; i < f.ncomp; ++i) {
        Component& c = f.comp[i];
        c.bw = f.mcux * c.h;
        c.bh = f.mcuy * c.v;
        c.real_bw = ((f.width * c.h + f.hmax - 1) / f.hmax + 7) / 8;
        c.real_bh = ((f.height * c.v + f.vmax - 1) / f.vmax + 7) / 8;
    }
    f.frame_seen = true;
    return true;
}

bool read_dqt(Frame& f, const u8* p, u32 n, const char** error) {
    while (n) {
        u32 pq = p[0] >> 4, tq = p[0] & 15;
        if (tq > 3 || pq > 1) return fail(error, "bad quantization table");
        u32 need = 1 + 64 * (pq + 1);
        if (n < need) return fail(error, "corrupt quantization table");
        for (u32 k = 0; k < 64; ++k) f.quant[tq][kNatural[k]] = pq ? be16(p + 1 + k * 2) : p[1 + k];
        f.quant_set[tq] = true;
        p += need;
        n -= need;
    }
    return true;
}

bool read_dht(Frame& f, const u8* p, u32 n, const char** error) {
    while (n) {
        if (n < 17) return fail(error, "corrupt Huffman table");
        u32 tc = p[0] >> 4, th = p[0] & 15;
        if (tc > 1 || th > 3) return fail(error, "bad Huffman table");
        u32 total = 0;
        for (u32 i = 0; i < 16; ++i) total += p[1 + i];
        if (total > 256 || n < 17 + total) return fail(error, "corrupt Huffman table");
        if (!build_huffman(tc ? f.ac[th] : f.dc[th], p + 1, p + 17, total)) return fail(error, "corrupt Huffman table");
        p += 17 + total;
        n -= 17 + total;
    }
    return true;
}

// ---- scans ----------------------------------------------------------------------------------

struct Scan {
    u32 n = 0;
    Component* c[4] = {};
    u32 ss = 0, se = 63, ah = 0, al = 0;
};

bool decode_block(Frame& f, Bits& b, Component& c, const Scan& s, i16* blk) {
    if (!f.progressive) {
        i32 t = decode(b, f.dc[c.td]);
        if (t < 0 || t > 15) return false;
        c.dc = (i32)((i64)c.dc + receive(b, t)); // a hostile file's sum wraps, and is only wrong
        blk[0] = (i16)c.dc;
        const Huffman& ac = f.ac[c.ta];
        bool keep = !c.dc_only;
        for (u32 k = 1; k < 64;) {
            u32 peek = b.peek16() >> (16 - kFastBits);
            i32 fa = ac.fast_ac[peek];
            if (fa) {
                k += ((u32)fa >> 4) & 15;
                b.skip(fa & 15);
                if (k > 63) return false;
                if (keep) blk[kNatural[k]] = (i16)(fa >> 8);
                ++k;
                continue;
            }
            i32 rs = decode(b, ac);
            if (rs < 0) return false;
            u32 r = (u32)rs >> 4, sz = (u32)rs & 15;
            if (!sz) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            i32 v = receive(b, (i32)sz);
            if (keep) blk[kNatural[k]] = (i16)v;
            ++k;
        }
        return true;
    }
    if (s.ss == 0) { // DC: first, or a refinement bit
        if (s.ah == 0) {
            i32 t = decode(b, f.dc[c.td]);
            if (t < 0 || t > 15) return false;
            c.dc = (i32)((i64)c.dc + receive(b, t));
            blk[0] = (i16)((i64)c.dc * (1 << s.al));
        } else if (b.bit()) {
            blk[0] = (i16)(blk[0] | (1 << s.al));
        }
        return true;
    }
    const Huffman& ac = f.ac[c.ta];
    if (s.ah == 0) { // AC, first
        if (f.eobrun) { --f.eobrun; return true; }
        for (u32 k = s.ss; k <= s.se;) {
            i32 rs = decode(b, ac);
            if (rs < 0) return false;
            u32 r = (u32)rs >> 4, sz = (u32)rs & 15;
            if (!sz) {
                if (r < 15) {
                    f.eobrun = (1u << r) - 1;
                    if (r) f.eobrun += b.get((i32)r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return false;
            blk[kNatural[k]] = (i16)(receive(b, (i32)sz) * (1 << s.al));
            ++k;
        }
        return true;
    }
    // AC, refinement: a bit for each coefficient already nonzero, and new ones of +-1.
    i16 p1 = (i16)(1 << s.al), m1 = (i16)(-1 * (1 << s.al));
    u32 k = s.ss;
    if (!f.eobrun) {
        for (; k <= s.se; ++k) {
            i32 rs = decode(b, ac);
            if (rs < 0) return false;
            i32 r = rs >> 4, sz = rs & 15;
            i16 val = 0;
            if (sz) {
                if (sz != 1) return false;
                val = b.bit() ? p1 : m1;
            } else if (r != 15) {
                f.eobrun = 1u << r;
                if (r) f.eobrun += b.get(r);
                break;
            }
            while (k <= s.se) {
                i16* coef = &blk[kNatural[k]];
                if (*coef) {
                    if (b.bit() && !(*coef & p1)) *coef = (i16)(*coef + (*coef >= 0 ? p1 : m1));
                } else {
                    if (--r < 0) break;
                }
                ++k;
            }
            if (val && k <= s.se) blk[kNatural[k]] = val;
        }
    }
    if (f.eobrun) {
        for (; k <= s.se; ++k) {
            i16* coef = &blk[kNatural[k]];
            if (*coef && b.bit() && !(*coef & p1)) *coef = (i16)(*coef + (*coef >= 0 ? p1 : m1));
        }
        --f.eobrun;
    }
    return true;
}

// At a restart interval's end: the marker skipped (or searched for, in spoiled data), and
// the predictions and the run reset.
void restart(Frame& f, Bits& b, Scan& s) {
    b.reset();
    while (b.p + 1 < b.end && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) {
        if (b.p[0] == 0xFF && b.p[1] != 0 && b.p[1] != 0xFF) break; // another marker: the scan is over
        ++b.p;
    }
    if (b.p + 1 < b.end && b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7) { b.p += 2; b.stopped = false; }
    for (u32 i = 0; i < s.n; ++i) s.c[i]->dc = 0;
    f.eobrun = 0;
}

// The scan's entropy-coded data, from `p`; where it stopped (a marker, or the end).
const u8* decode_scan(Frame& f, Scan& s, const u8* p, const u8* end) {
    Bits b;
    b.p = p;
    b.end = end;
    for (u32 i = 0; i < s.n; ++i) s.c[i]->dc = 0;
    f.eobrun = 0;
    u32 mcus = 0, done = 0; // done: whole MCU rows decoded
    bool ok = true;
    if (s.n == 1) {
        // Non-interleaved: the component's own blocks, one an MCU.
        Component& c = *s.c[0];
        for (u32 by = 0; by < c.real_bh && ok; ++by) {
            for (u32 bx = 0; bx < c.real_bw && ok; ++bx) {
                if (f.restart && mcus && mcus % f.restart == 0) restart(f, b, s);
                ok = decode_block(f, b, c, s, c.coef + ((usize)by * c.bw + bx) * c.stride);
                ++mcus;
                if (b.ended) ok = false;
            }
            if (ok) done = (by + 1) / c.v;
        }
    } else {
        for (u32 my = 0; my < f.mcuy && ok; ++my) {
            for (u32 mx = 0; mx < f.mcux && ok; ++mx) {
                if (f.restart && mcus && mcus % f.restart == 0) restart(f, b, s);
                for (u32 i = 0; i < s.n && ok; ++i) {
                    Component& c = *s.c[i];
                    for (u32 y = 0; y < c.v && ok; ++y)
                        for (u32 x = 0; x < c.h && ok; ++x)
                            ok = decode_block(f, b, c, s, c.coef + ((usize)(my * c.v + y) * c.bw + mx * c.h + x) * c.stride);
                }
                ++mcus;
                if (b.ended) ok = false;
            }
            if (ok) done = my + 1;
        }
    }
    if (!ok) {
        f.spoiled = true;
        if (done < f.rows_done) f.rows_done = done;
    }
    // The scan's data ends at the next marker: a restart marker left there is skipped.
    const u8* q = b.p;
    while (q + 1 < end && !(q[0] == 0xFF && q[1] != 0 && !(q[1] >= 0xD0 && q[1] <= 0xD7))) ++q;
    return q + 1 < end ? q : end;
}

// ---- the inverse DCTs (libjpeg's integer ones) ----------------------------------------------

constexpr i32 kConstBits = 13, kPass1Bits = 2;
constexpr i32 descale(i64 x, i32 n) { return (i32)((x + ((i64)1 << (n - 1))) >> n); }

// 8x8: jidctint.c's jpeg_idct_islow.
void idct8(const i16* in, const u16* q, u8* out, u32 stride) {
    i32 ws[64];
    for (u32 c = 0; c < 8; ++c) {
        const i16* x = in + c;
        const u16* qq = q + c;
        if (!x[8] && !x[16] && !x[24] && !x[32] && !x[40] && !x[48] && !x[56]) {
            i32 dc = (i32)((i64)x[0] * qq[0] * (1 << kPass1Bits));
            for (u32 r = 0; r < 8; ++r) ws[r * 8 + c] = dc;
            continue;
        }
        i64 z2 = x[16] * qq[16], z3 = x[48] * qq[48];
        i64 z1 = (z2 + z3) * 4433;
        i64 tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        z2 = x[0] * qq[0];
        z3 = x[32] * qq[32];
        i64 tmp0 = (z2 + z3) * (1 << kConstBits), tmp1 = (z2 - z3) * (1 << kConstBits);
        i64 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = x[56] * qq[56]; tmp1 = x[40] * qq[40]; tmp2 = x[24] * qq[24]; tmp3 = x[8] * qq[8];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; i64 z4 = tmp1 + tmp3;
        i64 z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        constexpr i32 s = kConstBits - kPass1Bits;
        ws[0 * 8 + c] = descale(tmp10 + tmp3, s); ws[7 * 8 + c] = descale(tmp10 - tmp3, s);
        ws[1 * 8 + c] = descale(tmp11 + tmp2, s); ws[6 * 8 + c] = descale(tmp11 - tmp2, s);
        ws[2 * 8 + c] = descale(tmp12 + tmp1, s); ws[5 * 8 + c] = descale(tmp12 - tmp1, s);
        ws[3 * 8 + c] = descale(tmp13 + tmp0, s); ws[4 * 8 + c] = descale(tmp13 - tmp0, s);
    }
    for (u32 r = 0; r < 8; ++r) {
        const i32* w = ws + r * 8;
        u8* o = out + (usize)r * stride;
        constexpr i32 s = kConstBits + kPass1Bits + 3;
        i64 z2 = w[2], z3 = w[6];
        i64 z1 = (z2 + z3) * 4433;
        i64 tmp2 = z1 + z3 * -15137, tmp3 = z1 + z2 * 6270;
        i64 tmp0 = ((i64)w[0] + w[4]) * (1 << kConstBits), tmp1 = ((i64)w[0] - w[4]) * (1 << kConstBits);
        i64 tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = w[7]; tmp1 = w[5]; tmp2 = w[3]; tmp3 = w[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; i64 z4 = tmp1 + tmp3;
        i64 z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        o[0] = clamp255(descale(tmp10 + tmp3, s) + 128); o[7] = clamp255(descale(tmp10 - tmp3, s) + 128);
        o[1] = clamp255(descale(tmp11 + tmp2, s) + 128); o[6] = clamp255(descale(tmp11 - tmp2, s) + 128);
        o[2] = clamp255(descale(tmp12 + tmp1, s) + 128); o[5] = clamp255(descale(tmp12 - tmp1, s) + 128);
        o[3] = clamp255(descale(tmp13 + tmp0, s) + 128); o[4] = clamp255(descale(tmp13 - tmp0, s) + 128);
    }
}

// 4x4: jidctred.c's jpeg_idct_4x4 (row 4 and column 4 of the input unused).
void idct4(const i16* in, const u16* q, u8* out, u32 stride) {
    i32 ws[8 * 4];
    for (u32 c = 0; c < 8; ++c) {
        if (c == 4) continue;
        const i16* x = in + c;
        const u16* qq = q + c;
        if (!x[8] && !x[16] && !x[24] && !x[40] && !x[48] && !x[56]) {
            i32 dc = (i32)((i64)x[0] * qq[0] * (1 << kPass1Bits));
            for (u32 r = 0; r < 4; ++r) ws[r * 8 + c] = dc;
            continue;
        }
        i64 tmp0 = (i64)(x[0] * qq[0]) * (1 << (kConstBits + 1));
        i64 tmp2 = (i64)(x[16] * qq[16]) * 15137 + (i64)(x[48] * qq[48]) * -6270;
        i64 tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        i64 z1 = x[56] * qq[56], z2 = x[40] * qq[40], z3 = x[24] * qq[24], z4 = x[8] * qq[8];
        tmp0 = z1 * -1730 + z2 * 11893 + z3 * -17799 + z4 * 8697;
        tmp2 = z1 * -4176 + z2 * -4926 + z3 * 7373 + z4 * 20995;
        constexpr i32 s = kConstBits - kPass1Bits + 1;
        ws[0 * 8 + c] = descale(tmp10 + tmp2, s); ws[3 * 8 + c] = descale(tmp10 - tmp2, s);
        ws[1 * 8 + c] = descale(tmp12 + tmp0, s); ws[2 * 8 + c] = descale(tmp12 - tmp0, s);
    }
    for (u32 r = 0; r < 4; ++r) {
        const i32* w = ws + r * 8;
        u8* o = out + (usize)r * stride;
        constexpr i32 s = kConstBits + kPass1Bits + 3 + 1;
        i64 tmp0 = (i64)w[0] * (1 << (kConstBits + 1));
        i64 tmp2 = (i64)w[2] * 15137 + (i64)w[6] * -6270;
        i64 tmp10 = tmp0 + tmp2, tmp12 = tmp0 - tmp2;
        i64 z1 = w[7], z2 = w[5], z3 = w[3], z4 = w[1];
        tmp0 = z1 * -1730 + z2 * 11893 + z3 * -17799 + z4 * 8697;
        tmp2 = z1 * -4176 + z2 * -4926 + z3 * 7373 + z4 * 20995;
        o[0] = clamp255(descale(tmp10 + tmp2, s) + 128); o[3] = clamp255(descale(tmp10 - tmp2, s) + 128);
        o[1] = clamp255(descale(tmp12 + tmp0, s) + 128); o[2] = clamp255(descale(tmp12 - tmp0, s) + 128);
    }
}

// 2x2: jidctred.c's jpeg_idct_2x2 (only the odd rows and columns but the first used).
void idct2(const i16* in, const u16* q, u8* out, u32 stride) {
    i32 ws[8 * 2];
    for (u32 c = 0; c < 8; ++c) {
        if (c == 2 || c == 4 || c == 6) continue;
        const i16* x = in + c;
        const u16* qq = q + c;
        if (!x[8] && !x[24] && !x[40] && !x[56]) {
            i32 dc = (i32)((i64)x[0] * qq[0] * (1 << kPass1Bits));
            ws[c] = ws[8 + c] = dc;
            continue;
        }
        i64 tmp10 = (i64)(x[0] * qq[0]) * (1 << (kConstBits + 2));
        i64 tmp0 = (i64)(x[56] * qq[56]) * -5906 + (i64)(x[40] * qq[40]) * 6967 + (i64)(x[24] * qq[24]) * -10426 + (i64)(x[8] * qq[8]) * 29692;
        constexpr i32 s = kConstBits - kPass1Bits + 2;
        ws[c] = descale(tmp10 + tmp0, s);
        ws[8 + c] = descale(tmp10 - tmp0, s);
    }
    for (u32 r = 0; r < 2; ++r) {
        const i32* w = ws + r * 8;
        u8* o = out + (usize)r * stride;
        constexpr i32 s = kConstBits + kPass1Bits + 3 + 2;
        i64 tmp10 = (i64)w[0] * (1 << (kConstBits + 2));
        i64 tmp0 = (i64)w[7] * -5906 + (i64)w[5] * 6967 + (i64)w[3] * -10426 + (i64)w[1] * 29692;
        o[0] = clamp255(descale(tmp10 + tmp0, s) + 128);
        o[1] = clamp255(descale(tmp10 - tmp0, s) + 128);
    }
}

// 1x1: the DC alone.
void idct1(const i16* in, const u16* q, u8* out, u32) { out[0] = clamp255(descale((i64)in[0] * q[0], 3) + 128); }

// ---- upsampling (libjpeg-turbo's jdsample.c) ------------------------------------------------

// One component's plane to the image's size, `ow` x `oh`, into `dst`.
void upsample(const Component& c, u32 hf, u32 vf, bool fancy, u32 ow, u32 oh, u8* dst) {
    const u8* src = c.plane;
    u32 sw = c.pw, dw = c.dw ? c.dw : 1, dh = c.dh ? c.dh : 1;
    auto at = [&](u32 x, u32 y) -> i32 { return src[(usize)(y < dh ? y : dh - 1) * sw + (x < dw ? x : dw - 1)]; };
    if (hf == 1 && vf == 1) {
        for (u32 y = 0; y < oh; ++y) {
            const u8* row = src + (usize)(y < dh ? y : dh - 1) * sw;
            u32 n = ow < dw ? ow : dw;
            memcpy(dst + (usize)y * ow, row, n);
            for (u32 x = n; x < ow; ++x) dst[(usize)y * ow + x] = row[dw - 1];
        }
        return;
    }
    if (fancy && hf == 2 && vf == 2 && dw > 2) {
        for (u32 y = 0; y < oh; ++y) {
            u32 r = y / 2 < dh ? y / 2 : dh - 1;
            u32 other = (y & 1) ? (r + 1 < dh ? r + 1 : dh - 1) : (r ? r - 1 : 0);
            const u8* a0 = src + (usize)r * sw;
            const u8* a1 = src + (usize)other * sw;
            u8* o = dst + (usize)y * ow;
            // Column sums along the row, then the horizontal triangle: jdsample.c's h2v2.
            i32 last = a0[0] * 3 + a1[0], cur = last, next = a0[1] * 3 + a1[1];
            o[0] = (u8)((cur * 4 + 8) >> 4);
            if (ow > 1) o[1] = (u8)((cur * 3 + next + 7) >> 4);
            for (u32 i = 1; i + 1 < dw; ++i) {
                last = cur;
                cur = next;
                next = a0[i + 1] * 3 + a1[i + 1];
                u32 x = 2 * i;
                if (x + 1 < ow) { o[x] = (u8)((cur * 3 + last + 8) >> 4); o[x + 1] = (u8)((cur * 3 + next + 7) >> 4); }
                else if (x < ow) o[x] = (u8)((cur * 3 + last + 8) >> 4);
            }
            {
                u32 i = dw - 1, x = 2 * i;
                last = cur;
                cur = next;
                if (x < ow) o[x] = (u8)((cur * 3 + last + 8) >> 4);
                if (x + 1 < ow) o[x + 1] = (u8)((cur * 4 + 7) >> 4);
            }
            for (u32 x = 2 * dw; x < ow; ++x) o[x] = o[2 * dw - 1];
        }
        return;
    }
    if (fancy && hf == 2 && vf == 1 && dw > 2) {
        for (u32 y = 0; y < oh; ++y) {
            u8* o = dst + (usize)y * ow;
            for (u32 i = 0; i < dw && 2 * i < ow; ++i) {
                i32 t = at(i, y);
                i32 a = i ? (t * 3 + at(i - 1, y) + 1) >> 2 : t;
                i32 b = i + 1 < dw ? (t * 3 + at(i + 1, y) + 2) >> 2 : t;
                o[2 * i] = (u8)a;
                if (2 * i + 1 < ow) o[2 * i + 1] = (u8)b;
            }
            for (u32 x = 2 * dw; x < ow; ++x) o[x] = o[2 * dw - 1];
        }
        return;
    }
    if (fancy && hf == 1 && vf == 2) {
        for (u32 y = 0; y < oh; ++y) {
            u32 r = y / 2;
            bool lower = y & 1;
            u32 other = lower ? (r + 1 < dh ? r + 1 : dh - 1) : (r ? r - 1 : 0);
            u8* o = dst + (usize)y * ow;
            for (u32 x = 0; x < ow; ++x) o[x] = (u8)((at(x, r) * 3 + at(x, other) + (lower ? 2 : 1)) >> 2);
        }
        return;
    }
    for (u32 y = 0; y < oh; ++y) for (u32 x = 0; x < ow; ++x) dst[(usize)y * ow + x] = (u8)at(x / hf, y / vf);
}

// ---- colour (libjpeg's jdcolor.c, SCALEBITS 16) ---------------------------------------------

struct Ycc { i32 cr_r[256], cb_b[256], cr_g[256], cb_g[256]; };
void ycc_tables(Ycc& t) {
    constexpr i32 kHalf = 1 << 15;
    auto fix = [](f64 x) { return (i32)(x * 65536.0 + 0.5); };
    for (i32 i = 0; i < 256; ++i) {
        i32 x = i - 128;
        t.cr_r[i] = (fix(1.40200) * x + kHalf) >> 16;
        t.cb_b[i] = (fix(1.77200) * x + kHalf) >> 16;
        t.cr_g[i] = -fix(0.71414) * x;
        t.cb_g[i] = -fix(0.34414) * x + kHalf;
    }
}

// ---- the orientation -----------------------------------------------------------------------

// `src` (w x h, RGBA) turned by EXIF orientation `o` into `dst`, whose size is returned.
void orient(const u8* src, u32 w, u32 h, u8 o, u8* dst, u32* ow, u32* oh) {
    bool swap = o >= 5;
    *ow = swap ? h : w;
    *oh = swap ? w : h;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            u32 dx = x, dy = y;
            switch (o) {
            case 2: dx = w - 1 - x; break;
            case 3: dx = w - 1 - x; dy = h - 1 - y; break;
            case 4: dy = h - 1 - y; break;
            case 5: dx = y; dy = x; break;
            case 6: dx = h - 1 - y; dy = x; break;
            case 7: dx = h - 1 - y; dy = w - 1 - x; break;
            case 8: dx = y; dy = w - 1 - x; break;
            default: break;
            }
            memcpy(dst + ((usize)dy * *ow + dx) * 4, src + ((usize)y * w + x) * 4, 4);
        }
}

// ---- the file ------------------------------------------------------------------------------

// Reads the markers up to the frame (`scans` false) or through every scan.
bool parse(Frame& f, const u8* data, usize size, bool scans, const char** error) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail(error, "not a JPEG");
    const u8* p = data + 2;
    const u8* end = data + size;
    bool any_scan = false;
    while (p < end) {
        // A marker: 0xFF, perhaps more fill 0xFFs, then the code. Junk between is skipped.
        if (*p != 0xFF) { ++p; continue; }
        while (p < end && *p == 0xFF) ++p;
        if (p >= end) break;
        u8 m = *p++;
        if (m == 0xD9) break;                         // EOI
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue; // TEM, a stray RST
        if (p + 2 > end) { f.spoiled = true; break; }
        u32 len = be16(p);
        if (len < 2 || p + len > end) { if (any_scan) { f.spoiled = true; break; } return fail(error, "corrupt marker length"); }
        const u8* seg = p + 2;
        u32 n = len - 2;
        p += len;
        switch (m) {
        case 0xC0: case 0xC1: case 0xC2:
            if (!read_frame(f, seg, n, m, error)) return false;
            if (!scans) return true;
            break;
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: return fail(error, "lossless and hierarchical JPEG are not supported");
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF: return fail(error, "arithmetic coding is not supported");
        case 0xC4: if (!read_dht(f, seg, n, error)) return false; break;
        case 0xDB: if (!read_dqt(f, seg, n, error)) return false; break;
        case 0xDD: if (n < 2) return fail(error, "corrupt restart interval"); f.restart = be16(seg); break;
        case 0xE0: if (n >= 5 && !memcmp(seg, "JFIF\0", 5)) f.jfif = true; break;
        case 0xE1: read_exif(f, seg, n); break;
        case 0xEE: if (n >= 12 && !memcmp(seg, "Adobe", 5)) { f.adobe = true; f.adobe_transform = seg[11]; } break;
        case 0xDA: {
            if (!f.frame_seen) return fail(error, "a scan before the frame");
            if (n < 1) return fail(error, "corrupt scan header");
            Scan s;
            s.n = seg[0];
            if (s.n < 1 || s.n > f.ncomp || n < 4 + s.n * 2) return fail(error, "corrupt scan header");
            for (u32 i = 0; i < s.n; ++i) {
                u8 id = seg[1 + i * 2], tables = seg[2 + i * 2];
                Component* c = nullptr;
                for (u32 j = 0; j < f.ncomp; ++j) if (f.comp[j].id == id) c = &f.comp[j];
                if (!c) return fail(error, "a scan names no component of the frame");
                for (u32 j = 0; j < i; ++j) if (s.c[j] == c) return fail(error, "a scan names a component twice");
                c->td = tables >> 4;
                c->ta = tables & 15;
                if (c->td > 3 || c->ta > 3) return fail(error, "bad Huffman table");
                s.c[i] = c;
            }
            const u8* sp = seg + 1 + s.n * 2;
            s.ss = sp[0];
            s.se = sp[1];
            s.ah = sp[2] >> 4;
            s.al = sp[2] & 15;
            if (f.progressive) {
                if (s.ss > 63 || s.se > 63 || s.ss > s.se || s.ah > 13 || s.al > 13) return fail(error, "bad progressive scan");
                if (s.ss == 0 && s.se != 0) return fail(error, "bad progressive scan");
                if (s.ss > 0 && s.n != 1) return fail(error, "bad progressive scan");
            } else {
                s.ss = 0; s.se = 63; s.ah = s.al = 0;
            }
            // Its tables must be there.
            for (u32 i = 0; i < s.n; ++i) {
                bool dc_needed = !f.progressive || (s.ss == 0 && s.ah == 0);
                bool ac_needed = !f.progressive || s.ss > 0;
                if (dc_needed && !f.dc[s.c[i]->td].present) return fail(error, "a scan with no DC table");
                if (ac_needed && !f.ac[s.c[i]->ta].present) return fail(error, "a scan with no AC table");
            }
            any_scan = true;
            if (f.progressive && s.ss > 0 && s.c[0]->dc_only) { // an AC scan of a component that needs only its DC
                const u8* q = p;
                while (q + 1 < end && !(q[0] == 0xFF && q[1] != 0 && !(q[1] >= 0xD0 && q[1] <= 0xD7))) ++q;
                p = q + 1 < end ? q : end;
                break;
            }
            p = decode_scan(f, s, p, end);
            if (!f.progressive && s.n == f.ncomp && f.spoiled) return true; // a sequential file's one scan: what it had
            break;
        }
        default: break; // APPn, COM and the rest
        }
    }
    if (!any_scan && scans) return fail(error, "no image data");
    return true;
}

} // namespace jpg

bool jpeg_info(const void* data, usize size, JpegInfo* out, const char** error) {
    using jpg::Frame;
    using jpg::parse;
    using jpg::fail;
    Frame f;
    if (!data || !parse(f, (const u8*)data, size, false, error)) return false;
    if (!f.frame_seen) return fail(error, "no frame header");
    out->width = f.width;
    out->height = f.height;
    out->components = (u8)f.ncomp;
    out->progressive = f.progressive;
    out->orientation = f.orientation;
    return true;
}

bool jpeg_decode(const void* data, usize size, const JpegOptions& opt, Allocator* pixels, JpegImage& out, const char** error) {
    using namespace jpg;
    using jpg::fail;
    out = JpegImage{};
    if (!data) return fail(error, "not a JPEG");
    u32 scale = opt.scale;
    if (scale != 1 && scale != 2 && scale != 4 && scale != 8) return fail(error, "scale must be 1, 2, 4 or 8");
    Allocator* a = pixels ? pixels : mem_general();
    Frame f;
    // The header first: the size is checked before anything is allocated.
    if (!parse(f, (const u8*)data, size, false, error)) return false;
    if (!f.frame_seen) return fail(error, "no frame header");
    if ((u64)f.width * f.height > opt.max_pixels) return fail(error, "too large");
    // Each component's inverse DCT size. Scaled, a subsampled component's blocks are made
    // larger than the scale's, up to 8x8, while that halves what upsampling has to do
    // (libjpeg-turbo's jdmaster.c): at a half, 4:2:0's chroma is decoded 8x8 and not
    // upsampled at all.
    u32 bs = 8 / scale;
    for (u32 i = 0; i < f.ncomp; ++i) {
        Component& c = f.comp[i];
        u32 cbs = bs;
        while (cbs < 8 && (f.hmax * bs) % (c.h * cbs * 2) == 0 && (f.vmax * bs) % (c.v * cbs * 2) == 0) cbs *= 2;
        c.bs = cbs;
        c.dc_only = cbs == 1;
        c.stride = c.dc_only ? 1 : 64;
    }
    for (u32 i = 0; i < f.ncomp; ++i) {
        Component& c = f.comp[i];
        usize n = (usize)c.bw * c.bh * c.stride;
        c.coef = (i16*)alloc_in(a, n * sizeof(i16));
        if (!c.coef) {
            for (u32 j = 0; j < i; ++j) free_in(a, f.comp[j].coef, (usize)f.comp[j].bw * f.comp[j].bh * f.comp[j].stride * sizeof(i16));
            return fail(error, "out of memory");
        }
        memset(c.coef, 0, n * sizeof(i16));
    }
    auto free_all = [&]() {
        for (u32 i = 0; i < f.ncomp; ++i) {
            Component& c = f.comp[i];
            free_in(a, c.coef, (usize)c.bw * c.bh * c.stride * sizeof(i16));
            free_in(a, c.plane, (usize)c.pw * c.ph);
            c.coef = nullptr;
            c.plane = nullptr;
        }
    };
    // Every scan, from the top: the frame is read again on the way, to the same geometry,
    // which keeps the buffers.
    {
        const char* why = nullptr;
        f.frame_seen = false;
        f.restart = 0;
        if (!parse(f, (const u8*)data, size, true, &why)) { free_all(); return fail(error, why); }
    }
    for (u32 i = 0; i < f.ncomp; ++i)
        if (!f.quant_set[f.comp[i].tq]) { free_all(); return fail(error, "a component with no quantization table"); }
    // Rows: all of a progressive image (its later scans only sharpen it); a sequential one's
    // as far as its data went.
    u32 height = f.height;
    if (f.spoiled && !f.progressive) {
        u32 rows = f.rows_done * 8 * f.vmax;
        if (!rows) { free_all(); return fail(error, "the data ends before the first row"); }
        if (rows < height) height = rows;
    }
    u32 W = (f.width + scale - 1) / scale, H = (height + scale - 1) / scale;
    // Each component through the inverse DCT into its plane.
    for (u32 i = 0; i < f.ncomp; ++i) {
        Component& c = f.comp[i];
        u32 cbs = c.bs;
        c.pw = c.bw * cbs;
        c.ph = c.bh * cbs;
        c.dw = (u32)(((u64)f.width * c.h * cbs + (u64)f.hmax * 8 - 1) / ((u64)f.hmax * 8));
        c.dh = (u32)(((u64)height * c.v * cbs + (u64)f.vmax * 8 - 1) / ((u64)f.vmax * 8));
        c.plane = (u8*)alloc_in(a, (usize)c.pw * c.ph);
        if (!c.plane) { free_all(); return fail(error, "out of memory"); }
        const u16* q = f.quant[c.tq];
        u32 rows_needed = (c.dh + cbs - 1) / cbs;
        if (rows_needed > c.bh) rows_needed = c.bh;
        for (u32 by = 0; by < rows_needed; ++by)
            for (u32 bx = 0; bx < c.bw; ++bx) {
                const i16* blk = c.coef + ((usize)by * c.bw + bx) * c.stride;
                u8* o = c.plane + (usize)by * cbs * c.pw + bx * cbs;
                if (cbs > 1) {
                    // No AC at all, as much of a smooth picture is: the block is its DC (what
                    // either pass would make of it, rounded the same).
                    u64 any = 0;
                    const u8* raw = (const u8*)blk;
                    for (u32 w8 = 1; w8 < 16; ++w8) { u64 v; memcpy(&v, raw + w8 * 8, 8); any |= v; }
                    u64 first; memcpy(&first, raw, 8);
                    any |= first & ~(u64)0xffff; // coefficients 1 to 3
                    if (!any) {
                        i32 dc = (i32)((i64)blk[0] * q[0] * (1 << kPass1Bits)); // pass 1's column value
                        u8 v = clamp255(descale((i64)dc * (1 << kConstBits), kConstBits + kPass1Bits + 3) + 128);
                        if (cbs != 8) {
                            // The reduced IDCTs scale their DC as their own passes do.
                            i64 t1 = cbs == 4 ? descale((i64)blk[0] * q[0] * (1 << (kConstBits + 1)), kConstBits - kPass1Bits + 1)
                                              : descale((i64)blk[0] * q[0] * (1 << (kConstBits + 2)), kConstBits - kPass1Bits + 2);
                            v = cbs == 4 ? clamp255(descale(t1 * (1 << (kConstBits + 1)), kConstBits + kPass1Bits + 3 + 1) + 128)
                                         : clamp255(descale(t1 * (1 << (kConstBits + 2)), kConstBits + kPass1Bits + 3 + 2) + 128);
                        }
                        for (u32 r = 0; r < cbs; ++r) memset(o + (usize)r * c.pw, v, cbs);
                        continue;
                    }
                }
                switch (cbs) {
                case 8: idct8(blk, q, o, c.pw); break;
                case 4: idct4(blk, q, o, c.pw); break;
                case 2: idct2(blk, q, o, c.pw); break;
                default: idct1(blk, q, o, c.pw); break;
                }
            }
        free_in(a, c.coef, (usize)c.bw * c.bh * c.stride * sizeof(i16));
        c.coef = nullptr;
    }
    // Upsampled to the image's size, converted, and turned.
    usize plane_bytes = (usize)W * H;
    u8* full[4] = {};
    for (u32 i = 0; i < f.ncomp; ++i) {
        full[i] = (u8*)alloc_in(a, plane_bytes ? plane_bytes : 1);
        if (!full[i]) { for (u32 j = 0; j < i; ++j) free_in(a, full[j], plane_bytes ? plane_bytes : 1); free_all(); return fail(error, "out of memory"); }
        const Component& c = f.comp[i];
        // What is left to upsample, after the blocks' sizes (jdsample.c's h_in_group and
        // h_out_group); fancy only where libjpeg-turbo is: not at an eighth.
        upsample(c, (f.hmax * bs) / (c.h * c.bs), (f.vmax * bs) / (c.v * c.bs), bs > 1, W, H, full[i]);
    }
    usize rgba_bytes = plane_bytes * 4;
    u8* rgba = (u8*)alloc_in(a, rgba_bytes ? rgba_bytes : 1);
    if (!rgba) { for (u32 j = 0; j < f.ncomp; ++j) free_in(a, full[j], plane_bytes ? plane_bytes : 1); free_all(); return fail(error, "out of memory"); }
    bool rgb = f.ncomp == 3 && ((f.adobe && f.adobe_transform == 0) ||
                                (!f.jfif && !f.adobe && f.comp[0].id == 'R' && f.comp[1].id == 'G' && f.comp[2].id == 'B'));
    Ycc t;
    ycc_tables(t);
    // A loop for each kind, with nothing to decide inside.
    const u8* p0 = full[0];
    const u8* p1 = f.ncomp > 1 ? full[1] : nullptr;
    const u8* p2 = f.ncomp > 2 ? full[2] : nullptr;
    const u8* p3 = f.ncomp > 3 ? full[3] : nullptr;
    bool ycck = f.ncomp == 4 && f.adobe && f.adobe_transform == 2;
    u8* o = rgba;
    if (f.ncomp == 1) {
        for (usize k = 0; k < plane_bytes; ++k, o += 4) { o[0] = o[1] = o[2] = p0[k]; o[3] = 255; }
    } else if (f.ncomp == 3 && rgb) {
        for (usize k = 0; k < plane_bytes; ++k, o += 4) { o[0] = p0[k]; o[1] = p1[k]; o[2] = p2[k]; o[3] = 255; }
    } else if (f.ncomp == 3) {
        for (usize k = 0; k < plane_bytes; ++k, o += 4) {
            i32 y = p0[k], cb = p1[k], cr = p2[k];
            o[0] = clamp255(y + t.cr_r[cr]);
            o[1] = clamp255(y + ((t.cb_g[cb] + t.cr_g[cr]) >> 16));
            o[2] = clamp255(y + t.cb_b[cb]);
            o[3] = 255;
        }
    } else {
        // CMYK as Adobe writes it, inverted (YCCK's C, M and Y are 255 less the colour):
        // each channel times K.
        for (usize k = 0; k < plane_bytes; ++k, o += 4) {
            i32 r = p0[k], g = p1[k], b = p2[k], kk = p3[k];
            if (ycck) {
                i32 y = r, cb = g, cr = b;
                r = 255 - clamp255(y + t.cr_r[cr]);
                g = 255 - clamp255(y + ((t.cb_g[cb] + t.cr_g[cr]) >> 16));
                b = 255 - clamp255(y + t.cb_b[cb]);
            }
            o[0] = (u8)((r * kk + 127) / 255);
            o[1] = (u8)((g * kk + 127) / 255);
            o[2] = (u8)((b * kk + 127) / 255);
            o[3] = 255;
        }
    }
    for (u32 j = 0; j < f.ncomp; ++j) free_in(a, full[j], plane_bytes ? plane_bytes : 1);
    free_all();
    out.partial = f.spoiled;
    out.orientation = f.orientation;
    out.width = W;
    out.height = H;
    out.rgba = rgba;
    if (opt.orient && f.orientation > 1) {
        u8* turned = (u8*)alloc_in(a, rgba_bytes ? rgba_bytes : 1);
        if (turned) {
            orient(rgba, W, H, f.orientation, turned, &out.width, &out.height);
            free_in(a, rgba, rgba_bytes ? rgba_bytes : 1);
            out.rgba = turned;
            out.orientation = 1;
        }
    }
    return true;
}

} // namespace kv
