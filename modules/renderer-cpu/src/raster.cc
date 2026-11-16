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
    for (i32 y = y0; y < y1; ++y) {
        i64 e[4] = {e_row[0], e_row[1], e_row[2], e_row[3]};
        f32 lx = q.l0x + (f32)x0 * q.ldx_x + (f32)y * q.ldy_x;
        f32 ly = q.l0y + (f32)x0 * q.ldx_y + (f32)y * q.ldy_y;
        u16* row = pixels + ((usize)(y - (i32)ty) * kTile + (u32)(x0 - (i32)tx)) * 4;
        if (q.shader && shaders) {
            // The row's runs of covered pixels, each a call of the shader.
            i32 run = -1;
            for (i32 x = x0; x <= x1; ++x) {
                bool in = x < x1 && (e[0] | e[1] | e[2] | e[3]) >= 0;
                if (in && run < 0) run = x;
                if (!in && run >= 0) {
                    shade_run(q, *shaders, lut, pixels + ((usize)(y - (i32)ty) * kTile + (u32)(run - (i32)tx)) * 4, run, x - run, y, quantize8);
                    run = -1;
                }
                if (x < x1) { e[0] += e_dx[0]; e[1] += e_dx[1]; e[2] += e_dx[2]; e[3] += e_dx[3]; }
            }
            for (u32 k = 0; k < 4; ++k) e_row[k] += e_dy[k];
            continue;
        }
        for (i32 x = x0; x < x1; ++x, row += 4) {
            if ((e[0] | e[1] | e[2] | e[3]) >= 0) {
                f32 u = q.uvx + lx * q.uvw, v = q.uvy + (1.0f - ly) * q.uvh;
                u32 t[4];
                sample(q.tex, q.filter, lut, u, v, t);
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
        u32 t[4];
        sample(u.tex, 1, lut, u.uv0[0] + u.uvd[0] * ax, u.uv0[1] + u.uvd[1] * ay, t);
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
                sample(u.tex, 1, lut, u.uv0[0] + u.uvd[0] * ax, u.uv0[1] + u.uvd[1] * ay, t);
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
    const CpuLuts& lut = cpu_luts();
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

void cpu_sample_texture(const CpuTexture& t, u32 sampler, f32 u, f32 v, f32 out[4]) {
    u32 c[4];
    sample(t, sampler == 0 ? 0 : 1, cpu_luts(), u, v, c, sampler == 2);
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
