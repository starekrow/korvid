// renderer-cpu-raster's kernels (raster.h). What decides a picture is integer arithmetic:
// coverage by edge functions in 1/256 pixel with the top-left rule, sampling weights in 8
// bits, blends on 16-bit linear light, the sRGB tables. Float is for the quads' local
// coordinates only. No engine header but types: this file is to be built fast.
#include "korvid/renderer-cpu-raster/raster.h"
#include <string.h>
#if defined(KV_ISA_AVX2)
#include <immintrin.h>
#endif

// Built once for each instruction set (isa_sources): the namespace says which.
#if defined(KV_ISA_AVX2)
#define KV_RASTER_SET avx2
#else
#define KV_RASTER_SET base
#endif

namespace kv { namespace rb_cpu { namespace KV_RASTER_SET {

namespace {

i32 floor_i(f32 v) {
    i32 i = (i32)v;
    return (f32)i > v ? i - 1 : i;
}

u32 wrap(i32 v, u32 n) {
    i32 m = v % (i32)n;
    return (u32)(m < 0 ? m + (i32)n : m);
}

u32 clamp_to(i32 v, u32 n) { return v < 0 ? 0u : (u32)v >= n ? n - 1 : (u32)v; }

// A texel as 16-bit linear R, G, B and alpha.
inline void texel(const CpuTexture& t, const CpuLuts& lut, u32 x, u32 y, u32 out[4]) {
    const u8* p = t.pixels + ((usize)y * t.width + x) * 4;
    if (t.srgb) { out[0] = lut.decode[p[0]]; out[1] = lut.decode[p[1]]; out[2] = lut.decode[p[2]]; }
    else { out[0] = p[0] * 257u; out[1] = p[1] * 257u; out[2] = p[2] * 257u; }
    out[3] = p[3] * 257u;
}

inline void sample(const CpuTexture& t, u8 filter, const CpuLuts& lut, f32 u, f32 v, u32 out[4], bool clamp = false) {
    if (!t.pixels || !t.width || !t.height) { out[0] = out[1] = out[2] = out[3] = 65535; return; }
    f32 fu = u * (f32)t.width, fv = v * (f32)t.height;
    if (!(fu > -1e9f && fu < 1e9f && fv > -1e9f && fv < 1e9f)) { fu = 0.0f; fv = 0.0f; } // not a number, or past the ints
    auto at = [&](i32 i, u32 n) { return clamp ? clamp_to(i, n) : wrap(i, n); };
    if (filter == 0) {
        texel(t, lut, at(floor_i(fu), t.width), at(floor_i(fv), t.height), out);
        return;
    }
    // Linear: the four texels around the point, weighted in 8 bits.
    fu -= 0.5f;
    fv -= 0.5f;
    i32 ix = floor_i(fu), iy = floor_i(fv);
    u32 wx = (u32)((fu - (f32)ix) * 256.0f + 0.5f), wy = (u32)((fv - (f32)iy) * 256.0f + 0.5f);
    u32 x0 = at(ix, t.width), x1 = at(ix + 1, t.width), y0 = at(iy, t.height), y1 = at(iy + 1, t.height);
    u32 a[4], b[4], c[4], d[4];
    texel(t, lut, x0, y0, a);
    texel(t, lut, x1, y0, b);
    texel(t, lut, x0, y1, c);
    texel(t, lut, x1, y1, d);
    for (u32 k = 0; k < 4; ++k) {
        u32 top = a[k] * (256 - wx) + b[k] * wx, bottom = c[k] * (256 - wx) + d[k] * wx; // 24 bits each
        out[k] = (u32)(((u64)top * (256 - wy) + (u64)bottom * wy + 32768) >> 16);
    }
}

// One pixel's source over the destination, by the quad's blend. src is linear, 16.16-tinted
// (and so may pass 65535 where a tint is above 1: clamped before blending).
inline void blend(u16* d, const u32 src[4], u8 mode) {
    u32 s0 = src[0] > 65535 ? 65535 : src[0], s1 = src[1] > 65535 ? 65535 : src[1], s2 = src[2] > 65535 ? 65535 : src[2];
    u32 a = src[3] > 65535 ? 65535 : src[3];
    if (mode == kCpuOpaque) { d[0] = (u16)s0; d[1] = (u16)s1; d[2] = (u16)s2; return; }
    if (mode == kCpuAlpha) {
        u32 ia = 65535 - a;
        d[0] = (u16)((s0 * a + d[0] * ia + 32767) / 65535);
        d[1] = (u16)((s1 * a + d[1] * ia + 32767) / 65535);
        d[2] = (u16)((s2 * a + d[2] * ia + 32767) / 65535);
        return;
    }
    u32 r = d[0] + (s0 * a + 32767) / 65535, g = d[1] + (s1 * a + 32767) / 65535, b = d[2] + (s2 * a + 32767) / 65535;
    d[0] = (u16)(r > 65535 ? 65535 : r);
    d[1] = (u16)(g > 65535 ? 65535 : g);
    d[2] = (u16)(b > 65535 ? 65535 : b);
}

// The GPU's target is 8-bit sRGB, each draw reading back the bytes the last wrote; a pixel
// rounded through it, to measure what keeping that costs (KV_CPU_TARGET8).
inline void quantize(u16* d, const CpuLuts& lut) {
    for (u32 k = 0; k < 3; ++k) d[k] = lut.decode[lut.encode[d[k]]];
}

// A game shader's colors over a run of a row's pixels, blended as the quad's blend says.
void shade_run(const CpuQuad& q, const CpuShaderCall& base, const CpuLuts& lut, u16* row, i32 x0, i32 count, i32 y, bool quantize8) {
    f32 out[kTile][4];
    CpuShaderCall c = base;
    c.kind = CpuShaderKind::Sprite;
    c.count = (u32)count;
    c.x0 = (f32)x0 + 0.5f;
    c.y = (f32)y + 0.5f;
    f32 lx = q.l0x + (f32)x0 * q.ldx_x + (f32)y * q.ldy_x, ly = q.l0y + (f32)x0 * q.ldx_y + (f32)y * q.ldy_y;
    c.local0[0] = lx; c.local0[1] = ly;
    c.dlocal[0] = q.ldx_x; c.dlocal[1] = q.ldx_y;
    c.uv0[0] = q.uvx + lx * q.uvw; c.uv0[1] = q.uvy + (1.0f - ly) * q.uvh;
    c.duv[0] = q.ldx_x * q.uvw; c.duv[1] = -q.ldx_y * q.uvh;
    c.world0[0] = q.w0x + lx * q.wlx_x + ly * q.wly_x;
    c.world0[1] = q.w0y + lx * q.wlx_y + ly * q.wly_y;
    c.dworld[0] = q.ldx_x * q.wlx_x + q.ldx_y * q.wly_x;
    c.dworld[1] = q.ldx_x * q.wlx_y + q.ldx_y * q.wly_y;
    for (u32 k = 0; k < 4; ++k) c.tint[k] = q.tintf[k];
    c.tex = q.tex_slot;
    c.filter = q.filter;
    c.material = q.material;
    c.out = out;
    memset(out, 0, sizeof(f32) * 4 * (usize)count);
    q.shader(c);
    for (i32 i = 0; i < count; ++i, row += 4) {
        if (cpu_shader_discarded(out[i])) continue;
        u32 src[4];
        for (u32 k = 0; k < 4; ++k) {
            f32 v = out[i][k];
            src[k] = !(v > 0.0f) ? 0u : v >= 1.0f ? 65535u : (u32)(v * 65535.0f + 0.5f);
        }
        blend(row, src, q.blend);
        if (quantize8) quantize(row, lut);
    }
}

// One pixel of a sprite quad without a game shader: its local coordinates at the pixel's
// centre (l0 + x ldx + y ldy, in that order: the AVX2 span computes them lane by lane the same
// way), the texture's texel, the tint, the blend. `ylx`, `yly` are y ldy.
inline void sprite_pixel(const CpuQuad& q, const CpuLuts& lut, u16* d, i32 x, f32 ylx, f32 yly, bool quantize8) {
    f32 fx = (f32)x;
    f32 lx = q.l0x + fx * q.ldx_x + ylx, ly = q.l0y + fx * q.ldx_y + yly;
    f32 u = q.uvx + lx * q.uvw, v = q.uvy + (1.0f - ly) * q.uvh;
    u32 t[4];
    sample(q.tex, q.filter, lut, u, v, t);
    u32 src[4] = {(u32)(((u64)t[0] * q.tint[0]) >> 16), (u32)(((u64)t[1] * q.tint[1]) >> 16),
                  (u32)(((u64)t[2] * q.tint[2]) >> 16), (u32)(((u64)t[3] * q.tint[3]) >> 16)};
    blend(d, src, q.blend);
    if (quantize8) quantize(d, lut);
}

// A run of a row's covered pixels, `count` of them from x0, into `row` (x0's pixel).
void sprite_span(const CpuQuad& q, const CpuLuts& lut, u16* row, i32 x0, i32 count, i32 y, bool quantize8);

void draw_quad(const CpuQuad& q, const CpuShaderCall* shaders, const CpuLuts& lut, u16* pixels, u32 tx, u32 ty, u32 tw, u32 th, bool quantize8) {
    i32 x0 = q.x0 > (i32)tx ? q.x0 : (i32)tx, x1 = q.x1 < (i32)(tx + tw) ? q.x1 : (i32)(tx + tw);
    i32 y0 = q.y0 > (i32)ty ? q.y0 : (i32)ty, y1 = q.y1 < (i32)(ty + th) ? q.y1 : (i32)(ty + th);
    if (x0 >= x1 || y0 >= y1) return;
    // The edges: E(p) = (b - a) x (p - a), positive inside; a pixel exactly on an edge is in
    // when the edge is a top or a left one (y down: going up, or level and going right).
    i64 e_row[4], e_dx[4], e_dy[4];
    for (u32 k = 0; k < 4; ++k) {
        i64 ax = q.vx[k], ay = q.vy[k], bx = q.vx[(k + 1) & 3], by = q.vy[(k + 1) & 3];
        i64 dx = bx - ax, dy = by - ay;
        bool top_left = dy < 0 || (dy == 0 && dx > 0);
        i64 px = (i64)x0 * 256 + 128, py = (i64)y0 * 256 + 128;
        e_row[k] = dx * (py - ay) - dy * (px - ax) + (top_left ? 0 : -1);
        e_dx[k] = -dy * 256;
        e_dy[k] = dx * 256;
    }
    bool shaded = q.shader && shaders;
    for (i32 y = y0; y < y1; ++y) {
        // The row's covered pixels: a quad is convex, so they are one run, [lo, hi). Each
        // edge bounds it on one side, solved exactly from its function at x0 and its step:
        // pixel x0 + n is inside the edge when e + n de >= 0.
        i64 lo = x0, hi = x1;
        for (u32 k = 0; k < 4 && lo < hi; ++k) {
            i64 e = e_row[k], de = e_dx[k];
            if (de == 0) { if (e < 0) hi = lo; }
            else if (de > 0) { if (e < 0) { i64 n = (-e + de - 1) / de; if (x0 + n > lo) lo = x0 + n; } }
            else if (e < 0) hi = lo;
            else { i64 n = e / -de; if (x0 + n + 1 < hi) hi = x0 + n + 1; }
        }
        if (lo < hi) {
            u16* at = pixels + ((usize)(y - (i32)ty) * kTile + (u32)((i32)lo - (i32)tx)) * 4;
            if (shaded) shade_run(q, *shaders, lut, at, (i32)lo, (i32)(hi - lo), y, quantize8);
            else sprite_span(q, lut, at, (i32)lo, (i32)(hi - lo), y, quantize8);
        }
        for (u32 k = 0; k < 4; ++k) e_row[k] += e_dy[k];
    }
}

void sprite_span_scalar(const CpuQuad& q, const CpuLuts& lut, u16* row, i32 x0, i32 count, i32 y, bool quantize8) {
    f32 ylx = (f32)y * q.ldy_x, yly = (f32)y * q.ldy_y;
    for (i32 i = 0; i < count; ++i, row += 4) sprite_pixel(q, lut, row, x0 + i, ylx, yly, quantize8);
}

#if !defined(KV_ISA_AVX2)
void sprite_span(const CpuQuad& q, const CpuLuts& lut, u16* row, i32 x0, i32 count, i32 y, bool quantize8) {
    sprite_span_scalar(q, lut, row, x0, count, y, quantize8);
}
#else
// ---- the AVX2 span: eight pixels at once, the scalar path's arithmetic lane by lane ---------
// Every step is the scalar's: the float coordinates in the same order (no FMA in this copy),
// floor and truncation as floor_i and the casts do them, the integer sampling, tint and
// blends exactly (x / 65535 as (x * 0x80008001) >> 47, which holds for every u32). What it
// cannot do at once (a texture whose width or height is not a power of two, read past its
// edge; an 8-bit target; a tint above 1) goes to sprite_pixel. The gallery's test draws
// with both paths and compares them exactly.

inline __m256i div65535(__m256i x) {
    const __m256i m = _mm256_set1_epi64x(0x80008001);
    __m256i even = _mm256_srli_epi64(_mm256_mul_epu32(x, m), 47);
    __m256i odd = _mm256_srli_epi64(_mm256_mul_epu32(_mm256_srli_epi64(x, 32), m), 47);
    return _mm256_or_si256(even, _mm256_slli_epi64(odd, 32));
}

// A coordinate's texel index along an axis of n texels: wrapped by mask for a power of two;
// else false when any lane is outside [0, n - 1 - extra] (the caller draws those pixels one by one).
inline bool wrap8(__m256i& i, u32 n, bool pow2, i32 extra) {
    if (pow2) { i = _mm256_and_si256(i, _mm256_set1_epi32((i32)n - 1)); return true; }
    __m256i bad = _mm256_or_si256(_mm256_cmpgt_epi32(_mm256_setzero_si256(), i), _mm256_cmpgt_epi32(i, _mm256_set1_epi32((i32)n - 1 - extra)));
    return _mm256_testz_si256(bad, bad) != 0;
}

// Eight texels as 16-bit linear R, G, B and alpha.
inline void texel8(const CpuTexture& t, const CpuLuts& lut, __m256i ix, __m256i iy, __m256i out[4]) {
    __m256i idx = _mm256_add_epi32(_mm256_mullo_epi32(iy, _mm256_set1_epi32((i32)t.width)), ix);
    __m256i p = _mm256_i32gather_epi32((const int*)t.pixels, idx, 4);
    const __m256i byte = _mm256_set1_epi32(255);
    __m256i c[3] = {_mm256_and_si256(p, byte), _mm256_and_si256(_mm256_srli_epi32(p, 8), byte), _mm256_and_si256(_mm256_srli_epi32(p, 16), byte)};
    for (u32 k = 0; k < 3; ++k) {
        if (t.srgb) out[k] = _mm256_and_si256(_mm256_i32gather_epi32((const int*)lut.decode, c[k], 2), _mm256_set1_epi32(0xFFFF));
        else out[k] = _mm256_mullo_epi32(c[k], _mm256_set1_epi32(257));
    }
    out[3] = _mm256_mullo_epi32(_mm256_srli_epi32(p, 24), _mm256_set1_epi32(257));
}

// Eight pixels' source colors (tinted, clamped), or false: draw them one by one.
inline bool source8(const CpuQuad& q, const CpuLuts& lut, i32 x, f32 ylx, f32 yly, __m256i src[4]) {
    const CpuTexture& t = q.tex;
    __m256 fx = _mm256_cvtepi32_ps(_mm256_add_epi32(_mm256_set1_epi32(x), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7)));
    __m256 lx = _mm256_add_ps(_mm256_add_ps(_mm256_set1_ps(q.l0x), _mm256_mul_ps(fx, _mm256_set1_ps(q.ldx_x))), _mm256_set1_ps(ylx));
    __m256 ly = _mm256_add_ps(_mm256_add_ps(_mm256_set1_ps(q.l0y), _mm256_mul_ps(fx, _mm256_set1_ps(q.ldx_y))), _mm256_set1_ps(yly));
    __m256 u = _mm256_add_ps(_mm256_set1_ps(q.uvx), _mm256_mul_ps(lx, _mm256_set1_ps(q.uvw)));
    __m256 v = _mm256_add_ps(_mm256_set1_ps(q.uvy), _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(1.0f), ly), _mm256_set1_ps(q.uvh)));
    __m256 fu = _mm256_mul_ps(u, _mm256_set1_ps((f32)t.width)), fv = _mm256_mul_ps(v, _mm256_set1_ps((f32)t.height));
    const __m256 lo = _mm256_set1_ps(-1e9f), hi = _mm256_set1_ps(1e9f);
    __m256 ok = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(fu, lo, _CMP_GT_OQ), _mm256_cmp_ps(fu, hi, _CMP_LT_OQ)),
                              _mm256_and_ps(_mm256_cmp_ps(fv, lo, _CMP_GT_OQ), _mm256_cmp_ps(fv, hi, _CMP_LT_OQ)));
    fu = _mm256_and_ps(fu, ok);
    fv = _mm256_and_ps(fv, ok);
    bool pw = (t.width & (t.width - 1)) == 0, ph = (t.height & (t.height - 1)) == 0;
    __m256i tc[4];
    if (q.filter == 0) {
        __m256i ix = _mm256_cvttps_epi32(_mm256_floor_ps(fu)), iy = _mm256_cvttps_epi32(_mm256_floor_ps(fv));
        if (!wrap8(ix, t.width, pw, 0) || !wrap8(iy, t.height, ph, 0)) return false;
        texel8(t, lut, ix, iy, tc);
    } else {
        fu = _mm256_sub_ps(fu, _mm256_set1_ps(0.5f));
        fv = _mm256_sub_ps(fv, _mm256_set1_ps(0.5f));
        __m256i ix = _mm256_cvttps_epi32(_mm256_floor_ps(fu)), iy = _mm256_cvttps_epi32(_mm256_floor_ps(fv));
        const __m256 k256 = _mm256_set1_ps(256.0f), half = _mm256_set1_ps(0.5f);
        __m256i wx = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(fu, _mm256_cvtepi32_ps(ix)), k256), half));
        __m256i wy = _mm256_cvttps_epi32(_mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(fv, _mm256_cvtepi32_ps(iy)), k256), half));
        __m256i ix1 = _mm256_add_epi32(ix, _mm256_set1_epi32(1)), iy1 = _mm256_add_epi32(iy, _mm256_set1_epi32(1));
        if (pw) { wrap8(ix, t.width, true, 0); wrap8(ix1, t.width, true, 0); }
        else if (!wrap8(ix, t.width, false, 1)) return false;
        if (ph) { wrap8(iy, t.height, true, 0); wrap8(iy1, t.height, true, 0); }
        else if (!wrap8(iy, t.height, false, 1)) return false;
        __m256i a[4], b[4], c[4], d[4];
        texel8(t, lut, ix, iy, a);
        texel8(t, lut, ix1, iy, b);
        texel8(t, lut, ix, iy1, c);
        texel8(t, lut, ix1, iy1, d);
        const __m256i k = _mm256_set1_epi32(256);
        __m256i iwx = _mm256_sub_epi32(k, wx), iwy = _mm256_sub_epi32(k, wy);
        for (u32 ch = 0; ch < 4; ++ch) {
            __m256i top = _mm256_add_epi32(_mm256_mullo_epi32(a[ch], iwx), _mm256_mullo_epi32(b[ch], wx));
            __m256i bottom = _mm256_add_epi32(_mm256_mullo_epi32(c[ch], iwx), _mm256_mullo_epi32(d[ch], wx));
            __m256i sum = _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(top, iwy), _mm256_mullo_epi32(bottom, wy)), _mm256_set1_epi32(32768));
            tc[ch] = _mm256_srli_epi32(sum, 16);
        }
    }
    const __m256i max = _mm256_set1_epi32(65535);
    for (u32 ch = 0; ch < 4; ++ch)
        src[ch] = _mm256_min_epu32(_mm256_srli_epi32(_mm256_mullo_epi32(tc[ch], _mm256_set1_epi32((i32)q.tint[ch])), 16), max);
    return true;
}

// Two pixels (one in each 128-bit half, R, G, B, A in 32-bit lanes) over two destination pixels.
inline __m256i blend2(__m256i s, __m256i d, u8 mode) {
    __m256i res;
    if (mode == kCpuOpaque) res = s;
    else {
        __m256i a = _mm256_shuffle_epi32(s, 0xFF);
        __m256i sa = _mm256_mullo_epi32(s, a);
        if (mode == kCpuAlpha) {
            __m256i ia = _mm256_sub_epi32(_mm256_set1_epi32(65535), a);
            res = div65535(_mm256_add_epi32(_mm256_add_epi32(sa, _mm256_mullo_epi32(d, ia)), _mm256_set1_epi32(32767)));
        } else {
            res = _mm256_min_epu32(_mm256_add_epi32(d, div65535(_mm256_add_epi32(sa, _mm256_set1_epi32(32767)))), _mm256_set1_epi32(65535));
        }
    }
    return _mm256_blend_epi32(res, d, 0x88); // the fourth channel stays the target's
}

// Eight pixels' sources over the row's eight pixels at `row`.
inline void blend8(u16* row, const __m256i src[4], u8 mode) {
    // The sources by pixel: {0, 4}, {1, 5}, {2, 6}, {3, 7}, a pixel to each 128-bit half.
    __m256i rg_lo = _mm256_unpacklo_epi32(src[0], src[1]), rg_hi = _mm256_unpackhi_epi32(src[0], src[1]);
    __m256i ba_lo = _mm256_unpacklo_epi32(src[2], src[3]), ba_hi = _mm256_unpackhi_epi32(src[2], src[3]);
    __m256i s04 = _mm256_unpacklo_epi64(rg_lo, ba_lo), s15 = _mm256_unpackhi_epi64(rg_lo, ba_lo);
    __m256i s26 = _mm256_unpacklo_epi64(rg_hi, ba_hi), s37 = _mm256_unpackhi_epi64(rg_hi, ba_hi);
    // The targets the same way: pixels 0, 1 and 4, 5; then 2, 3 and 6, 7.
    const __m256i z = _mm256_setzero_si256();
    __m256i d0 = _mm256_set_m128i(_mm_loadu_si128((const __m128i*)(row + 16)), _mm_loadu_si128((const __m128i*)row));
    __m256i d1 = _mm256_set_m128i(_mm_loadu_si128((const __m128i*)(row + 24)), _mm_loadu_si128((const __m128i*)(row + 8)));
    __m256i r04 = blend2(s04, _mm256_unpacklo_epi16(d0, z), mode), r15 = blend2(s15, _mm256_unpackhi_epi16(d0, z), mode);
    __m256i r26 = blend2(s26, _mm256_unpacklo_epi16(d1, z), mode), r37 = blend2(s37, _mm256_unpackhi_epi16(d1, z), mode);
    __m256i o0 = _mm256_packus_epi32(r04, r15), o1 = _mm256_packus_epi32(r26, r37);
    _mm_storeu_si128((__m128i*)row, _mm256_castsi256_si128(o0));
    _mm_storeu_si128((__m128i*)(row + 16), _mm256_extracti128_si256(o0, 1));
    _mm_storeu_si128((__m128i*)(row + 8), _mm256_castsi256_si128(o1));
    _mm_storeu_si128((__m128i*)(row + 24), _mm256_extracti128_si256(o1, 1));
}

void sprite_span(const CpuQuad& q, const CpuLuts& lut, u16* row, i32 x0, i32 count, i32 y, bool quantize8) {
    const CpuTexture& t = q.tex;
    bool wide = !quantize8 && t.pixels && t.width && t.height && (u64)t.width * t.height < (1ull << 31) && q.blend <= kCpuAdditive &&
                q.tint[0] <= 65536 && q.tint[1] <= 65536 && q.tint[2] <= 65536 && q.tint[3] <= 65536;
    if (!wide) { sprite_span_scalar(q, lut, row, x0, count, y, quantize8); return; }
    f32 ylx = (f32)y * q.ldy_x, yly = (f32)y * q.ldy_y;
    i32 i = 0;
    for (; i + 8 <= count; i += 8, row += 32) {
        __m256i src[4];
        if (source8(q, lut, x0 + i, ylx, yly, src)) blend8(row, src, q.blend);
        else for (i32 k = 0; k < 8; ++k) sprite_pixel(q, lut, row + 4 * k, x0 + i + k, ylx, yly, false);
    }
    for (; i < count; ++i, row += 4) sprite_pixel(q, lut, row, x0 + i, ylx, yly, false);
}
#endif

// ---- the UI primitive (ui.slang's fs_ui, line for line in float; the blend in 16 bits) ----

f32 sat(f32 v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
f32 min_f(f32 a, f32 b) { return a < b ? a : b; }
f32 max_f(f32 a, f32 b) { return a > b ? a : b; }
f32 abs_f(f32 a) { return a < 0.0f ? -a : a; }

// sd_round_box: the signed distance from a rounded rectangle at the origin, half size (bx,
// by), a radius a corner (tl, tr, br, bl; y down), negative inside.
inline f32 sd_round_box(f32 px, f32 py, f32 bx, f32 by, const f32 r[4]) {
    f32 rr = px > 0.0f ? (py > 0.0f ? r[2] : r[1]) : (py > 0.0f ? r[3] : r[0]);
    rr = min_f(rr, min_f(bx, by));
    f32 qx = abs_f(px) - bx + rr, qy = abs_f(py) - by + rr;
    f32 mx = max_f(qx, 0.0f), my = max_f(qy, 0.0f);
    return min_f(max_f(qx, qy), 0.0f) + __builtin_sqrtf(mx * mx + my * my) - rr;
}

// 0.5 (1 - erf(x)), from the table.
inline f32 shade_of(const CpuLuts& lut, f32 x) {
    f32 at = (x + 4.0f) * 128.0f;
    if (!(at > 0.0f)) return lut.shade[0];
    if (at >= 1024.0f) return lut.shade[1024];
    u32 i = (u32)at;
    f32 w = at - (f32)i;
    return lut.shade[i] + (lut.shade[i + 1] - lut.shade[i]) * w;
}

inline u32 to16(f32 v) { return !(v > 0.0f) ? 0u : v >= 1.0f ? 65535u : (u32)(v * 65535.0f + 0.5f); }

// Premultiplied source over the destination: d = s + d (1 - a).
inline void over(u16* d, const u32 s[4]) {
    u32 ia = 65535 - s[3];
    for (u32 k = 0; k < 3; ++k) {
        u32 v = s[k] + (d[k] * ia + 32767) / 65535;
        d[k] = (u16)(v > 65535 ? 65535 : v);
    }
}

// A box's color at UI pixel (x, y), premultiplied (fs_ui's box half).
inline void box_color(const CpuUi& u, const CpuLuts& lut, f32 x, f32 y, f32 out[4]) {
    f32 px = x - u.cx, py = y - u.cy;
    f32 d = sd_round_box(px, py, u.hx, u.hy, u.radii);
    f32 outer = sat(0.5f - d);
    f32 inner = u.border > 0.0f ? sat(0.5f - (d + u.border)) : outer;
    f32 fill[4] = {u.fill[0], u.fill[1], u.fill[2], u.fill[3]};
    if (u.gradient) {
        f32 t = u.reach > 0.0f ? sat((px * u.dir[0] + py * u.dir[1]) / u.reach * 0.5f + 0.5f) : 0.0f;
        for (u32 k = 0; k < 4; ++k) fill[k] = fill[k] + (u.fill2[k] - fill[k]) * t;
    }
    if (u.textured) {
        f32 ax = (x - u.rx) / max_f(u.rw, 1e-6f), ay = (y - u.ry) / max_f(u.rh, 1e-6f);
        if (u.tiled) {
            ax = (x - u.rx) / u.tile[0];
            ay = (y - u.ry) / u.tile[1];
            ax -= (f32)floor_i(ax); // frac
            ay -= (f32)floor_i(ay);
        }
        f32 su = min_f(max_f(u.uv0[0] + u.uvd[0] * ax, u.uvlo[0]), u.uvhi[0]);
        f32 sv = min_f(max_f(u.uv0[1] + u.uvd[1] * ay, u.uvlo[1]), u.uvhi[1]);
        u32 t[4];
        sample(u.tex, u.nearest ? 0 : 1, lut, su, sv, t);
        f32 ta = (f32)t[3] / 65535.0f;
        fill[0] = fill[0] * ((f32)t[0] / 65535.0f) * ta;
        fill[1] = fill[1] * ((f32)t[1] / 65535.0f) * ta;
        fill[2] = fill[2] * ((f32)t[2] / 65535.0f) * ta;
        fill[3] = fill[3] * ta;
    }
    for (u32 k = 0; k < 4; ++k) out[k] = fill[k] * inner + u.edge[k] * (outer - inner);
    if (u.shadow) {
        f32 ds = sd_round_box(px - u.so[0], py - u.so[1], u.shx, u.shy, u.sradii);
        f32 a = shade_of(lut, ds * u.inv_sigma) * (1.0f - out[3]);
        for (u32 k = 0; k < 4; ++k) out[k] = out[k] + u.shade[k] * a;
    }
}

void draw_ui(const CpuUi& u, const CpuLuts& lut, u16* pixels, u32 tx, u32 ty, u32 tw, u32 th, bool quantize8) {
    i32 x0 = u.x0 > (i32)tx ? u.x0 : (i32)tx, x1 = u.x1 < (i32)(tx + tw) ? u.x1 : (i32)(tx + tw);
    i32 y0 = u.y0 > (i32)ty ? u.y0 : (i32)ty, y1 = u.y1 < (i32)(ty + th) ? u.y1 : (i32)(ty + th);
    if (x0 >= x1 || y0 >= y1) return;
    for (i32 y = y0; y < y1; ++y) {
        f32 rx = u.l0x + (f32)y * u.ldy_x, ry = u.l0y + (f32)y * u.ldy_y;
        u16* row = pixels + ((usize)(y - (i32)ty) * kTile + (u32)(x0 - (i32)tx)) * 4;
        for (i32 x = x0; x < x1; ++x, row += 4) {
            f32 px = rx + (f32)x * u.ldx_x, py = ry + (f32)x * u.ldx_y;
            if (!(px >= u.lo[0] && px < u.hi[0] && py >= u.lo[1] && py < u.hi[1])) continue;
            u32 s[4];
            if (u.kind == 1) {
                // A glyph: its coverage curved by its ink, scaling the ink and the texel.
                f32 ax = (px - u.rx) / u.rw, ay = (py - u.ry) / u.rh;
                u32 t[4];
                sample(u.tex, u.nearest ? 0 : 1, lut, u.uv0[0] + u.uvd[0] * ax, u.uv0[1] + u.uvd[1] * ay, t);
                u32 at = (t[3] * 256u * 256u + 32767u) / 65535u; // 0 to 65536: the table's place in 8.8
                u32 i = at >> 8, w = at & 255u;
                const u16* c = lut.cover[u.gamma];
                u32 a = i >= 256 ? c[256] : (c[i] * (256u - w) + c[i + 1] * w + 128u) >> 8;
                for (u32 k = 0; k < 3; ++k) s[k] = (u32)(((u64)u.flat[k] * t[k] / 65535u * a + 32767u) / 65535u);
                s[3] = (u.flat[3] * a + 32767u) / 65535u;
            } else if (px >= u.core[0] && px <= u.core[2] && py >= u.core[1] && py <= u.core[3]) {
                s[0] = u.flat[0]; s[1] = u.flat[1]; s[2] = u.flat[2]; s[3] = u.flat[3];
            } else {
                f32 c[4];
                box_color(u, lut, px, py, c);
                s[0] = to16(c[0]); s[1] = to16(c[1]); s[2] = to16(c[2]); s[3] = to16(c[3]);
                if (s[0] > s[3]) s[0] = s[3]; // premultiplied stays so through rounding
                if (s[1] > s[3]) s[1] = s[3];
                if (s[2] > s[3]) s[2] = s[3];
            }
            if (!s[3] && !s[0] && !s[1] && !s[2]) continue;
            over(row, s);
            if (quantize8) quantize(row, lut);
        }
    }
}

} // namespace

void cpu_raster_tile(const CpuFrame& fr, const u32* items, u32 count, u16* pixels, u32 tx, u32 ty, u32 tw, u32 th) {
    const CpuLuts& lut = *fr.luts;
    if (fr.post && fr.shaders) {
        // The post shader's picture: a row of the tile a call; written, not blended.
        f32 out[kTile][4];
        for (u32 y = 0; y < th; ++y) {
            CpuShaderCall c = *fr.shaders;
            c.kind = CpuShaderKind::Post;
            c.count = tw;
            c.x0 = (f32)tx + 0.5f;
            c.y = (f32)(ty + y) + 0.5f;
            c.out = out;
            memset(out, 0, sizeof out);
            fr.post(c);
            u16* p = pixels + (usize)y * kTile * 4;
            for (u32 x = 0; x < tw; ++x, p += 4) {
                for (u32 k = 0; k < 3; ++k) {
                    f32 v = out[x][k];
                    p[k] = (u16)(!(v > 0.0f) ? 0u : v >= 1.0f ? 65535u : (u32)(v * 65535.0f + 0.5f));
                }
                p[3] = 0;
                if (fr.quantize8) quantize(p, lut);
            }
        }
    } else {
        for (u32 y = 0; y < th; ++y)
            for (u32 x = 0; x < tw; ++x) {
                u16* p = pixels + ((usize)y * kTile + x) * 4;
                p[0] = fr.clear[0]; p[1] = fr.clear[1]; p[2] = fr.clear[2]; p[3] = 0;
            }
    }
    for (u32 i = 0; i < count; ++i) {
        u32 it = items[i];
        if (it & kCpuDrawUi) draw_ui(fr.ui[it & ~kCpuDrawUi], lut, pixels, tx, ty, tw, th, fr.quantize8);
        else draw_quad(fr.quads[it], fr.shaders, lut, pixels, tx, ty, tw, th, fr.quantize8);
    }
}

void cpu_sample_texture(const CpuLuts& luts, const CpuTexture& t, u32 sampler, f32 u, f32 v, f32 out[4]) {
    u32 c[4];
    sample(t, sampler == 0 ? 0 : 1, luts, u, v, c, sampler == 2);
    for (u32 k = 0; k < 4; ++k) out[k] = (f32)c[k] / 65535.0f;
}

void cpu_sample_world(const CpuWorld& w, f32 u, f32 v, f32 out[4]) {
    if (!w.pixels || !w.width || !w.height) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }
    f32 fu = u * (f32)w.width - 0.5f, fv = v * (f32)w.height - 0.5f;
    if (!(fu > -1e9f && fu < 1e9f && fv > -1e9f && fv < 1e9f)) { fu = 0.0f; fv = 0.0f; }
    i32 ix = floor_i(fu), iy = floor_i(fv);
    u32 wx = (u32)((fu - (f32)ix) * 256.0f + 0.5f), wy = (u32)((fv - (f32)iy) * 256.0f + 0.5f);
    u32 x0 = clamp_to(ix, w.width), x1 = clamp_to(ix + 1, w.width), y0 = clamp_to(iy, w.height), y1 = clamp_to(iy + 1, w.height);
    const u16* a = w.pixels + ((usize)y0 * w.width + x0) * 4;
    const u16* b = w.pixels + ((usize)y0 * w.width + x1) * 4;
    const u16* c = w.pixels + ((usize)y1 * w.width + x0) * 4;
    const u16* d = w.pixels + ((usize)y1 * w.width + x1) * 4;
    for (u32 k = 0; k < 3; ++k) {
        u32 top = a[k] * (256 - wx) + b[k] * wx, bottom = c[k] * (256 - wx) + d[k] * wx;
        out[k] = (f32)(((u64)top * (256 - wy) + (u64)bottom * wy + 32768) >> 16) / 65535.0f;
    }
    out[3] = 1.0f;
}

void cpu_encode_tile(const CpuLuts& lut, const u16* pixels, u32 tw, u32 th, u8* target, u32 pitch, bool rgba) {
    for (u32 y = 0; y < th; ++y) {
        const u16* p = pixels + (usize)y * kTile * 4;
        u8* o = target + (usize)y * pitch;
        for (u32 x = 0; x < tw; ++x, p += 4, o += 4) {
            u8 r = lut.encode[p[0]], g = lut.encode[p[1]], b = lut.encode[p[2]];
            if (rgba) { o[0] = r; o[1] = g; o[2] = b; } else { o[0] = b; o[1] = g; o[2] = r; }
            o[3] = 255;
        }
    }
}

} } } // namespace kv::rb_cpu::KV_RASTER_SET
