// renderer-cpu's pixel kernels (raster.h). What decides a picture is integer arithmetic:
// coverage by edge functions in 1/256 pixel with the top-left rule, sampling weights in 8
// bits, blends on 16-bit linear light, the sRGB tables. Float is for the quads' local
// coordinates only. No engine header but types: this file is to be built fast.
#include "raster.h"
#include <string.h>

namespace kv { namespace rb_cpu {

namespace {

i32 floor_i(f32 v) {
    i32 i = (i32)v;
    return (f32)i > v ? i - 1 : i;
}

u32 wrap(i32 v, u32 n) {
    i32 m = v % (i32)n;
    return (u32)(m < 0 ? m + (i32)n : m);
}

// A texel as 16-bit linear R, G, B and alpha.
inline void texel(const CpuTexture& t, const CpuLuts& lut, u32 x, u32 y, u32 out[4]) {
    const u8* p = t.pixels + ((usize)y * t.width + x) * 4;
    if (t.srgb) { out[0] = lut.decode[p[0]]; out[1] = lut.decode[p[1]]; out[2] = lut.decode[p[2]]; }
    else { out[0] = p[0] * 257u; out[1] = p[1] * 257u; out[2] = p[2] * 257u; }
    out[3] = p[3] * 257u;
}

inline void sample(const CpuQuad& q, const CpuLuts& lut, f32 u, f32 v, u32 out[4]) {
    const CpuTexture& t = q.tex;
    if (!t.pixels || !t.width || !t.height) { out[0] = out[1] = out[2] = out[3] = 65535; return; }
    f32 fu = u * (f32)t.width, fv = v * (f32)t.height;
    if (q.filter == 0) {
        texel(t, lut, wrap(floor_i(fu), t.width), wrap(floor_i(fv), t.height), out);
        return;
    }
    // Linear: the four texels around the point, weighted in 8 bits.
    fu -= 0.5f;
    fv -= 0.5f;
    i32 ix = floor_i(fu), iy = floor_i(fv);
    u32 wx = (u32)((fu - (f32)ix) * 256.0f + 0.5f), wy = (u32)((fv - (f32)iy) * 256.0f + 0.5f);
    u32 x0 = wrap(ix, t.width), x1 = wrap(ix + 1, t.width), y0 = wrap(iy, t.height), y1 = wrap(iy + 1, t.height);
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

void draw_quad(const CpuQuad& q, const CpuLuts& lut, u16* pixels, u32 tx, u32 ty, u32 tw, u32 th, bool quantize8) {
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
    for (i32 y = y0; y < y1; ++y) {
        i64 e[4] = {e_row[0], e_row[1], e_row[2], e_row[3]};
        f32 lx = q.l0x + (f32)x0 * q.ldx_x + (f32)y * q.ldy_x;
        f32 ly = q.l0y + (f32)x0 * q.ldx_y + (f32)y * q.ldy_y;
        u16* row = pixels + ((usize)(y - (i32)ty) * kTile + (u32)(x0 - (i32)tx)) * 4;
        for (i32 x = x0; x < x1; ++x, row += 4) {
            if ((e[0] | e[1] | e[2] | e[3]) >= 0) {
                f32 u = q.uvx + lx * q.uvw, v = q.uvy + (1.0f - ly) * q.uvh;
                u32 t[4];
                sample(q, lut, u, v, t);
                u32 src[4] = {(u32)(((u64)t[0] * q.tint[0]) >> 16), (u32)(((u64)t[1] * q.tint[1]) >> 16),
                              (u32)(((u64)t[2] * q.tint[2]) >> 16), (u32)(((u64)t[3] * q.tint[3]) >> 16)};
                blend(row, src, q.blend);
                if (quantize8) quantize(row, lut);
            }
            e[0] += e_dx[0]; e[1] += e_dx[1]; e[2] += e_dx[2]; e[3] += e_dx[3];
            lx += q.ldx_x;
            ly += q.ldx_y;
        }
        for (u32 k = 0; k < 4; ++k) e_row[k] += e_dy[k];
    }
}

} // namespace

void cpu_raster_tile(const CpuQuad* quads, const u32* items, u32 count, u16* pixels,
                     u32 tx, u32 ty, u32 tw, u32 th, const u16 clear[3], bool quantize8) {
    const CpuLuts& lut = cpu_luts();
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x) {
            u16* p = pixels + ((usize)y * kTile + x) * 4;
            p[0] = clear[0]; p[1] = clear[1]; p[2] = clear[2]; p[3] = 0;
        }
    for (u32 i = 0; i < count; ++i) draw_quad(quads[items[i]], lut, pixels, tx, ty, tw, th, quantize8);
}

void cpu_encode_tile(const u16* pixels, u32 tw, u32 th, u8* target, u32 pitch, bool rgba) {
    const CpuLuts& lut = cpu_luts();
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

} } // namespace kv::rb_cpu
