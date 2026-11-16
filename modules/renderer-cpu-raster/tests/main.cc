// renderer-cpu-raster: every set of kernels draws the same bytes (docs/PRDs/CPU-Renderer-PRD.md,
// SR16). Thousands of random sprite quads (turned, scaled, both filters, every blend, tints
// above and below 1, textures of power-of-two sizes and not, uvs that wrap) drawn into tiles
// with each set this build has and this CPU runs, compared byte for byte with the baseline's.
// The tables need not be sRGB's: any tables do, so long as every set reads the same ones.
#include "korvid/renderer-cpu-raster/raster.h"
#include "korvid/core/test.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace kv;
using namespace kv::rb_cpu;

struct Random {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (u32)(s >> 11); }
    f32 unit() { return (f32)(next() & 0xFFFFFF) / 16777216.0f; }
    f32 range(f32 a, f32 b) { return a + (b - a) * unit(); }
};

// A quad the way renderer-cpu's prepare() makes one: corners in 1/256 pixel, wound inside
// positive, its pixel bounds, and local coordinates at a pixel's centre.
bool make_quad(Random& rnd, const CpuTexture* textures, u32 ntex, CpuQuad& q) {
    memset(&q, 0, sizeof q);
    f32 cx = rnd.range(-20.0f, 84.0f), cy = rnd.range(-20.0f, 84.0f);
    f32 w = rnd.range(1.0f, 60.0f), h = rnd.range(1.0f, 60.0f), a = rnd.range(0.0f, 6.2831853f);
    if (rnd.next() % 3 == 0) a = 0.0f; // axis-aligned, edges on pixel centres
    f32 cs = cosf(a), sn = sinf(a);
    f32 ma = cs * w, mb = sn * w, mc = -sn * h, md = cs * h;
    auto to_px = [&](f32 x, f32 y, f32* ox, f32* oy) { *ox = ma * x + mc * y + cx; *oy = mb * x + md * y + cy; };
    static const f32 kCorner[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    f32 minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (u32 k = 0; k < 4; ++k) {
        f32 fx, fy;
        to_px(kCorner[k][0], kCorner[k][1], &fx, &fy);
        q.vx[k] = (i32)floorf(fx * 256.0f + 0.5f);
        q.vy[k] = (i32)floorf(fy * 256.0f + 0.5f);
        minx = fminf(minx, fx); maxx = fmaxf(maxx, fx);
        miny = fminf(miny, fy); maxy = fmaxf(maxy, fy);
    }
    i64 area = 0;
    for (u32 k = 0; k < 4; ++k) area += (i64)q.vx[k] * q.vy[(k + 1) & 3] - (i64)q.vx[(k + 1) & 3] * q.vy[k];
    if (area == 0) return false;
    if (area < 0) {
        i32 t = q.vx[1]; q.vx[1] = q.vx[3]; q.vx[3] = t;
        t = q.vy[1]; q.vy[1] = q.vy[3]; q.vy[3] = t;
    }
    q.x0 = (i32)floorf(minx); q.x1 = (i32)ceilf(maxx) + 1;
    q.y0 = (i32)floorf(miny); q.y1 = (i32)ceilf(maxy) + 1;
    if (q.x0 < 0) q.x0 = 0;
    if (q.y0 < 0) q.y0 = 0;
    if (q.x1 > 64) q.x1 = 64;
    if (q.y1 > 64) q.y1 = 64;
    if (q.x0 >= q.x1 || q.y0 >= q.y1) return false;
    f32 ox, oy, axx, axy, ayx, ayy;
    to_px(0.0f, 0.0f, &ox, &oy);
    to_px(1.0f, 0.0f, &axx, &axy);
    to_px(0.0f, 1.0f, &ayx, &ayy);
    axx -= ox; axy -= oy; ayx -= ox; ayy -= oy;
    f32 det = axx * ayy - ayx * axy;
    if (!(fabsf(det) > 1e-12f)) return false;
    f32 i00 = ayy / det, i01 = -ayx / det, i10 = -axy / det, i11 = axx / det;
    q.ldx_x = i00; q.ldx_y = i10;
    q.ldy_x = i01; q.ldy_y = i11;
    q.l0x = i00 * (0.5f - ox) + i01 * (0.5f - oy) + 0.5f;
    q.l0y = i10 * (0.5f - ox) + i11 * (0.5f - oy) + 0.5f;
    // uvs: inside the texture, or scrolled and scaled past it (repeat).
    bool wraps = rnd.next() % 2;
    q.uvx = wraps ? rnd.range(-3.0f, 3.0f) : 0.0f;
    q.uvy = wraps ? rnd.range(-3.0f, 3.0f) : 0.0f;
    q.uvw = wraps ? rnd.range(-2.0f, 4.0f) : 1.0f;
    q.uvh = wraps ? rnd.range(-2.0f, 4.0f) : 1.0f;
    for (u32 k = 0; k < 4; ++k) {
        u32 r = rnd.next() % 8;
        q.tint[k] = r == 0 ? 65536 : r == 1 ? 98304 /* 1.5 */ : (rnd.next() % 65537);
    }
    q.tex = textures[rnd.next() % ntex];
    q.filter = (u8)(rnd.next() % 2);
    q.blend = (u8)(rnd.next() % 3);
    return true;
}

} // namespace

int main() {
    TestCtx c;
    c.begin("every kernel set draws the baseline's bytes");
    static CpuLuts luts;
    for (u32 i = 0; i < 256; ++i) luts.decode[i] = (u16)((i * i * 257u) / 255u); // any curve
    for (u32 i = 0; i < 65536; ++i) luts.encode[i] = (u8)(i >> 8);
    // Textures: powers of two, not, one texel, sRGB and not.
    static const u32 sizes[][2] = {{16, 16}, {64, 32}, {1, 1}, {13, 7}, {5, 64}, {128, 3}};
    CpuTexture textures[2 * KV_COUNTOF(sizes)];
    static u8 pixels[2 * KV_COUNTOF(sizes)][128 * 64 * 4];
    Random rnd;
    u32 ntex = 0;
    for (u32 srgb = 0; srgb < 2; ++srgb)
        for (const auto& sz : sizes) {
            u8* p = pixels[ntex];
            for (u32 i = 0; i < sz[0] * sz[1] * 4; ++i) p[i] = (u8)rnd.next();
            textures[ntex++] = CpuTexture{p, sz[0], sz[1], 1, srgb != 0};
        }
    const CpuRasterPath* paths[4];
    u32 n = cpu_raster_paths(paths, 4);
    printf("kernel sets: %u (", n);
    for (u32 i = 0; i < n; ++i) printf("%s%s", i ? ", " : "", paths[i]->name);
    printf(")\n");
    static CpuQuad quads[64];
    static u16 base_px[kTile * kTile * 4], other_px[kTile * kTile * 4];
    u32 tiles = 0, mismatched = 0, drawn = 0;
    for (u32 round = 0; round < 3000; ++round) {
        u32 count = 0;
        u32 want = 1 + rnd.next() % 12;
        while (count < want) if (make_quad(rnd, textures, ntex, quads[count])) ++count;
        u32 items[64];
        for (u32 i = 0; i < count; ++i) items[i] = i;
        CpuFrame fr;
        fr.luts = &luts;
        fr.quads = quads;
        fr.clear[0] = (u16)rnd.next(); fr.clear[1] = (u16)rnd.next(); fr.clear[2] = (u16)rnd.next();
        // Tiles of every size a window's edge makes, at an offset or not.
        u32 tw = rnd.next() % 4 == 0 ? 1 + rnd.next() % 64 : 64, th = rnd.next() % 4 == 0 ? 1 + rnd.next() % 64 : 64;
        paths[0]->raster_tile(fr, items, count, base_px, 0, 0, tw, th);
        for (u32 p = 1; p < n; ++p) {
            memset(other_px, 0xAB, sizeof other_px);
            paths[p]->raster_tile(fr, items, count, other_px, 0, 0, tw, th);
            bool same = true;
            for (u32 y = 0; y < th && same; ++y) same = memcmp(base_px + (usize)y * kTile * 4, other_px + (usize)y * kTile * 4, (usize)tw * 4 * sizeof(u16)) == 0;
            if (!same && mismatched++ < 3) printf("round %u: %s differs from base\n", round, paths[p]->name);
        }
        ++tiles;
        drawn += count;
    }
    printf("%u tiles, %u quads\n", tiles, drawn);
    KV_CHECK(c, mismatched == 0);
    return test_report(c, "renderer_cpu_raster_tests");
}
