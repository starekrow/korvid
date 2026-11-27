// renderer-cpu's frame: the sort and the cull as the GPU's compute passes do them, the draw
// runs made into quads in the window's pixels, and a pass's quads binned into tiles and
// drawn as a job batch (docs/PRDs/CPU-Renderer-PRD.md, milestone 2). The placement follows
// sprite_common.slangh's row_placement and sprite.slang's vs_main line for line.
#include "renderer_internal.h"
#include "korvid/core/job.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/math/scalar.h"
#include <stddef.h>
#include <string.h>

namespace kv { namespace KV_RB_NS {

namespace {

const GpuSprite* sprites_of(const Renderer* r) { return (const GpuSprite*)r->tables.sprites.data; }
const GpuMaterial* materials_of(const Renderer* r) { return (const GpuMaterial*)r->material_table.data; }
u32* totals_of(Renderer* r) { return (u32*)r->sort_meta.data; }
u32* starts_of(Renderer* r) { return (u32*)r->sort_meta.data + kBucketCount; }

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// A 64-bit hash of `n` bytes (a tile's draws are compared by theirs). Not core's hash_bytes:
// it runs on every quad and UI item every frame, is never stored, and is 4x faster (18 ns a
// CpuQuad against 77).
u64 frame_hash(const void* p, usize n) {
    const u8* b = (const u8*)p;
    u64 h = 0x9E3779B97F4A7C15ull;
    for (; n >= 8; n -= 8, b += 8) {
        u64 w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; n; --n, ++b) h = (h ^ *b) * 0x100000001B3ull;
    h ^= h >> 29;
    return h * 0xC4CEB9FE1A85EC53ull;
}

// Folds `v` into `h`: a tile's draws' hashes, in order.
u64 frame_hash_mix(u64 h, u64 v) {
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 31);
}

// A texture slot as the rasterizer reads it: the slot's, else the default (slot 0) while it
// has no pixels yet.
CpuTexture texture_of(const Renderer* r, u32 slot) {
    if (!(slot < r->textures.len && r->textures.is_alive(slot) && r->textures.items[slot].resident)) slot = 0;
    const Texture& t = r->textures.items[slot];
    CpuTexture out;
    out.pixels = t.pixels.data;
    out.width = t.width;
    out.height = t.height;
    out.version = t.version;
    out.srgb = t.format == TextureFormat::RGBA8_SRGB;
    return out;
}

// A row's position, rotation and scale at the frame's interpolation point (row_placement).
void place(const Renderer* r, const GpuSprite& s, f32 pos[2], f32* rot, f32 scale[2]) {
    const PushData& p = r->push;
    f32 t = s.stamp == p.tick ? p.alpha : 1.0f;
    pos[0] = lerp(s.prev[0], s.cur[0], t);
    pos[1] = lerp(s.prev[1], s.cur[1], t);
    if (s.flags & kSpriteScreen) {
        u32 ax = (s.flags >> kSpriteAnchorShift) & 3u, ay = (s.flags >> (kSpriteAnchorShift + 2)) & 3u;
        pos[0] += (ax == 1 ? -1.0f : ax == 2 ? 1.0f : 0.0f) * p.screen_cull[2] * 0.5f;
        pos[1] += (ay == 1 ? -1.0f : ay == 2 ? 1.0f : 0.0f) * p.screen_cull[3] * 0.5f;
    } else if (r->layers) {
        i32 layer = s.layer < kLayerMin ? kLayerMin : s.layer > kLayerMax ? kLayerMax : s.layer;
        const f32* l = r->layers + (u32)(layer - kLayerMin) * 4;
        pos[0] += lerp(l[0], l[2], p.alpha);
        pos[1] += lerp(l[1], l[3], p.alpha);
    }
    *rot = lerp(s.prev[2], s.cur[2], t);
    scale[0] = lerp(s.prev[3], s.cur[3], t);
    scale[1] = lerp(s.scale_y[0], s.scale_y[1], t);
}

u32 bucket_of_row(const Renderer* r, const GpuSprite& s) {
    const GpuMaterial& m = materials_of(r)[s.material < kMaxMaterials ? s.material : 0];
    i32 layer = s.layer < kLayerMin ? kLayerMin : s.layer > kLayerMax ? kLayerMax : s.layer;
    return (u32)(layer - kLayerMin) * kSlotCount + (m.slot < kSlotCount ? m.slot : kSlotCount - 1);
}

// The sort key as an unsigned int that orders as the float does (-0 before +0).
u32 orderable(f32 key) {
    u32 u;
    memcpy(&u, &key, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

// A keyed bucket: its first kKeySortMax rows by (key, row), stable (they arrive in row order).
void sort_keyed(Renderer* r, u32 b) {
    u32 n = totals_of(r)[b];
    if (n < 2) return;
    if (n > kKeySortMax) n = kKeySortMax;
    u32* rows = (u32*)r->tables.sorted.data + starts_of(r)[b];
    MemScope scope;
    u64* keyed = arena_push_n<u64>(*scope.arena, n);
    u64* tmp   = arena_push_n<u64>(*scope.arena, n);
    for (u32 i = 0; i < n; ++i) {
        const GpuSprite& s = sprites_of(r)[rows[i]];
        u32 mode = (s.flags >> kSpriteSortShift) & 3u;
        f32 key = 0.0f;
        if (mode == 1) key = s.order;
        else if (mode == 2) { f32 pos[2], rot, sc[2]; place(r, s, pos, &rot, sc); key = -(pos[1] + s.order); }
        keyed[i] = (u64)orderable(key) << 32 | rows[i];
    }
    // A bottom-up merge sort: (key, row) in one word, so stable needs nothing more.
    for (u32 w = 1; w < n; w *= 2) {
        for (u32 lo = 0; lo < n; lo += 2 * w) {
            u32 mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, a = lo, c = mid, o = lo;
            while (a < mid && c < hi) tmp[o++] = keyed[a] <= keyed[c] ? keyed[a++] : keyed[c++];
            while (a < mid) tmp[o++] = keyed[a++];
            while (c < hi) tmp[o++] = keyed[c++];
        }
        u64* t = keyed; keyed = tmp; tmp = t;
    }
    for (u32 i = 0; i < n; ++i) rows[i] = (u32)keyed[i];
}

// A row's quad in the window's pixels; false when it covers none.
bool prepare(Renderer* r, u32 row, CpuQuad& q) {
    memset(&q, 0, sizeof q); // the padding too: the quad is hashed as bytes
    const GpuSprite& s = sprites_of(r)[row];
    const GpuMaterial& m = materials_of(r)[s.material < kMaxMaterials ? s.material : 0];
    f32 pos[2], rot, sc[2];
    place(r, s, pos, &rot, sc);
    sc[0] *= s.size[0];
    sc[1] *= s.size[1];
    f32 cs = kv::cos(rot), sn = kv::sin(rot);
    f32 ma = cs * sc[0], mb = sn * sc[0], mc = -sn * sc[1], md = cs * sc[1];
    const PushData& p = r->push;
    bool screen = (s.flags & kSpriteScreen) != 0;
    const f32* v = screen ? p.screen : p.view;
    const f32* vt = screen ? p.screen_trans : p.view_trans;
    f32 W = (f32)r->target_width, H = (f32)r->target_height;
    auto to_px = [&](f32 cx, f32 cy, f32* ox, f32* oy) {
        f32 wx = ma * cx + mc * cy + pos[0], wy = mb * cx + md * cy + pos[1];
        f32 x = v[0] * wx + v[2] * wy + vt[0], y = v[1] * wx + v[3] * wy + vt[1];
        *ox = (x + 1.0f) * 0.5f * W;
        *oy = (y + 1.0f) * 0.5f * H;
    };
    static const f32 kCorner[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    f32 fx[4], fy[4];
    f32 minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (u32 k = 0; k < 4; ++k) {
        to_px(kCorner[k][0], kCorner[k][1], &fx[k], &fy[k]);
        if (!(fx[k] > -1e6f && fx[k] < 1e6f && fy[k] > -1e6f && fy[k] < 1e6f)) return false; // off any sane screen, or not a number
        q.vx[k] = (i32)kv::floor(fx[k] * 256.0f + 0.5f);
        q.vy[k] = (i32)kv::floor(fy[k] * 256.0f + 0.5f);
        minx = fx[k] < minx ? fx[k] : minx; maxx = fx[k] > maxx ? fx[k] : maxx;
        miny = fy[k] < miny ? fy[k] : miny; maxy = fy[k] > maxy ? fy[k] : maxy;
    }
    // Wound so that the inside is positive (raster.cc's edges).
    i64 area = 0;
    for (u32 k = 0; k < 4; ++k) area += (i64)q.vx[k] * q.vy[(k + 1) & 3] - (i64)q.vx[(k + 1) & 3] * q.vy[k];
    if (area == 0) return false;
    if (area < 0) {
        i32 t = q.vx[1]; q.vx[1] = q.vx[3]; q.vx[3] = t;
        t = q.vy[1]; q.vy[1] = q.vy[3]; q.vy[3] = t;
    }
    q.x0 = (i32)kv::floor(minx); q.x1 = (i32)kv::ceil(maxx) + 1;
    q.y0 = (i32)kv::floor(miny); q.y1 = (i32)kv::ceil(maxy) + 1;
    if (q.x0 < 0) q.x0 = 0;
    if (q.y0 < 0) q.y0 = 0;
    if (q.x1 > (i32)r->target_width) q.x1 = (i32)r->target_width;
    if (q.y1 > (i32)r->target_height) q.y1 = (i32)r->target_height;
    if (q.x0 >= q.x1 || q.y0 >= q.y1) return false;
    // The local coordinates at a pixel's centre: the inverse of the corner-to-pixel map.
    f32 ox, oy, axx, axy, ayx, ayy;
    to_px(0.0f, 0.0f, &ox, &oy);
    to_px(1.0f, 0.0f, &axx, &axy);
    to_px(0.0f, 1.0f, &ayx, &ayy);
    axx -= ox; axy -= oy; ayx -= ox; ayy -= oy;
    f32 det = axx * ayy - ayx * axy;
    if (!(kv::fabs(det) > 1e-12f)) return false;
    f32 i00 = ayy / det, i01 = -ayx / det, i10 = -axy / det, i11 = axx / det;
    q.ldx_x = i00; q.ldx_y = i10;
    q.ldy_x = i01; q.ldy_y = i11;
    q.l0x = i00 * (0.5f - ox) + i01 * (0.5f - oy) + 0.5f;
    q.l0y = i10 * (0.5f - ox) + i11 * (0.5f - oy) + 0.5f;
    q.uvx = s.uv[0] + m.params[0];
    q.uvy = s.uv[1] + m.params[1];
    q.uvw = s.uv[2];
    q.uvh = s.uv[3];
    for (u32 k = 0; k < 4; ++k) {
        f32 t = s.tint[k] * m.tint[k];
        q.tint[k] = !(t > 0.0f) ? 0u : t >= 65535.0f ? 0xFFFFFFFFu : (u32)(t * 65536.0f + 0.5f);
    }
    q.tex = texture_of(r, m.texture);
    q.filter = m.filter ? 1 : 0;
    q.blend = (u8)(m.blend < 3 ? m.blend : 1);
    // A game shader's slot: its CPU form, and what its varyings need.
    u32 gi = m.slot >= kBlendCount ? m.slot - kBlendCount : kGameSlots;
    if (gi < kGameSlots && r->game_pipes[gi].fn && !r->game_pipes[gi].failed) {
        q.shader = r->game_pipes[gi].fn;
        q.w0x = pos[0] - 0.5f * (ma + mc);
        q.w0y = pos[1] - 0.5f * (mb + md);
        q.wlx_x = ma; q.wlx_y = mb;
        q.wly_x = mc; q.wly_y = md;
        for (u32 k = 0; k < 4; ++k) q.tintf[k] = s.tint[k] * m.tint[k];
        q.tex_slot = m.texture;
        q.material = s.material < kMaxMaterials ? s.material : 0;
        q.time = p.time;
        q.tick = p.tick;
    }
    q.hash = frame_hash(&q, offsetof(CpuQuad, hash));
    return true;
}

// ---- the UI primitive: a list's items decoded (ui.slang's layout) -----------------------------

u32 word_at(const u8* p) { u32 w; memcpy(&w, p, 4); return w; }
f32 float_at(const u8* p) { f32 v; memcpy(&v, p, 4); return v; }

// RGBA8 sRGB, straight alpha, to linear premultiplied (ui_color).
void ui_color(u32 w, f32 out[4]) {
    const CpuLuts& lut = cpu_luts();
    f32 a = (f32)(w >> 24) / 255.0f;
    out[0] = (f32)lut.decode[w & 255u] / 65535.0f * a;
    out[1] = (f32)lut.decode[(w >> 8) & 255u] / 65535.0f * a;
    out[2] = (f32)lut.decode[(w >> 16) & 255u] / 65535.0f * a;
    out[3] = a;
}

void ui_uv(u32 w, f32 out[2]) {
    out[0] = (f32)(w & 0xFFFFu) / 65535.0f;
    out[1] = (f32)(w >> 16) / 65535.0f;
}

u32 to16(f32 v) { return !(v > 0.0f) ? 0u : v >= 1.0f ? 65535u : (u32)(v * 65535.0f + 0.5f); }

// The UI's pixels to the window's: px_to_clip, then the viewport. `m` is the window pixel of
// UI pixel (x, y): m[0] + x m[2] + y m[4], m[1] + x m[3] + y m[5].
struct UiView {
    f32 m[6];
    f32 l0x, l0y, ldx_x, ldx_y, ldy_x, ldy_y; // the inverse, at a window pixel's centre
    bool ok;
};

UiView ui_view(const Renderer* r) {
    const PushData& p = r->push;
    f32 W = (f32)r->target_width, H = (f32)r->target_height, sw = p.screen_cull[2], sh = p.screen_cull[3];
    auto at = [&](f32 x, f32 y, f32* ox, f32* oy) {
        f32 sx = x - sw * 0.5f, sy = sh * 0.5f - y;
        f32 cx = p.screen[0] * sx + p.screen[2] * sy + p.screen_trans[0];
        f32 cy = p.screen[1] * sx + p.screen[3] * sy + p.screen_trans[1];
        *ox = (cx + 1.0f) * 0.5f * W;
        *oy = (cy + 1.0f) * 0.5f * H;
    };
    UiView v{};
    at(0.0f, 0.0f, &v.m[0], &v.m[1]);
    at(1.0f, 0.0f, &v.m[2], &v.m[3]);
    at(0.0f, 1.0f, &v.m[4], &v.m[5]);
    v.m[2] -= v.m[0]; v.m[3] -= v.m[1]; v.m[4] -= v.m[0]; v.m[5] -= v.m[1];
    f32 det = v.m[2] * v.m[5] - v.m[4] * v.m[3];
    v.ok = kv::fabs(det) > 1e-12f;
    if (!v.ok) return v;
    f32 i00 = v.m[5] / det, i01 = -v.m[4] / det, i10 = -v.m[3] / det, i11 = v.m[2] / det;
    v.ldx_x = i00; v.ldx_y = i10;
    v.ldy_x = i01; v.ldy_y = i11;
    v.l0x = i00 * (0.5f - v.m[0]) + i01 * (0.5f - v.m[1]);
    v.l0y = i10 * (0.5f - v.m[0]) + i11 * (0.5f - v.m[1]);
    return v;
}

// One item at `item` of the region at `region`; false when it draws nothing.
bool prepare_ui(Renderer* r, const UiView& v, const u8* region, const u8* item, CpuUi& u) {
    memset(&u, 0, sizeof u);
    u32 head = word_at(item);
    u32 clip = (head >> 2) & 0xFFFFu;
    const u8* cr = region + word_at(region + 8) + 16u * clip;
    f32 cx = float_at(cr), cy = float_at(cr + 4), cw = float_at(cr + 8), ch = float_at(cr + 12);
    u.kind = (head & 3u) == 1u ? 1 : 0;
    f32 lo[2], hi[2];
    if (u.kind == 1) {
        u.rx = float_at(item + 12); u.ry = float_at(item + 16); u.rw = float_at(item + 20); u.rh = float_at(item + 24);
        ui_color(word_at(item + 8), u.fill);
        f32 uv1[2];
        ui_uv(word_at(item + 28), u.uv0);
        ui_uv(word_at(item + 32), uv1);
        u.uvd[0] = uv1[0] - u.uv0[0];
        u.uvd[1] = uv1[1] - u.uv0[1];
        u.tex = texture_of(r, word_at(item + 4));
        u.nearest = (head & (1u << 20)) != 0;
        f32 a = u.fill[3] > 1e-4f ? u.fill[3] : 1e-4f;
        f32 lum = (u.fill[0] * 0.2126f + u.fill[1] * 0.7152f + u.fill[2] * 0.0722f) / a;
        lum = lum > 0.0f ? (lum < 1.0f ? lum : 1.0f) : 0.0f;
        u.gamma = (u8)(lum * 63.0f + 0.5f);
        if (!(u.rw > 0.0f && u.rh > 0.0f) || u.fill[3] <= 0.0f) return false;
        lo[0] = u.rx; lo[1] = u.ry; hi[0] = u.rx + u.rw; hi[1] = u.ry + u.rh;
        u.core[0] = u.core[1] = 1.0f; // empty
    } else {
        u.rx = float_at(item + 24); u.ry = float_at(item + 28); u.rw = float_at(item + 32); u.rh = float_at(item + 36);
        for (u32 k = 0; k < 4; ++k) u.radii[k] = float_at(item + 40 + 4 * k);
        u.border = float_at(item + 56);
        f32 angle = float_at(item + 60);
        u.so[0] = float_at(item + 64); u.so[1] = float_at(item + 68);
        f32 blur = float_at(item + 72), spread = float_at(item + 76);
        u.hx = u.rw * 0.5f; u.hy = u.rh * 0.5f;
        u.cx = u.rx + u.hx; u.cy = u.ry + u.hy;
        ui_color(word_at(item + 8), u.fill);
        ui_color(word_at(item + 16), u.edge);
        ui_color(word_at(item + 20), u.shade);
        u.gradient = (head & (1u << 18)) != 0;
        u.textured = (head & (1u << 19)) != 0;
        if (u.gradient) {
            ui_color(word_at(item + 12), u.fill2);
            u.dir[0] = kv::cos(angle);
            u.dir[1] = kv::sin(angle);
            u.reach = kv::fabs(u.hx * u.dir[0]) + kv::fabs(u.hy * u.dir[1]);
        }
        if (u.textured) {
            f32 uv1[2];
            u.uv0[0] = float_at(item + 80); u.uv0[1] = float_at(item + 84);
            uv1[0] = float_at(item + 88); uv1[1] = float_at(item + 92);
            u.uvd[0] = uv1[0] - u.uv0[0];
            u.uvd[1] = uv1[1] - u.uv0[1];
            u.tex = texture_of(r, word_at(item + 4));
            u.nearest = (head & (1u << 20)) != 0;
            u.tiled = (head & (1u << 21)) != 0;
            if (u.tiled) { u.tile[0] = float_at(item + 96); u.tile[1] = float_at(item + 100); }
            u.adjust = (head & (1u << 22)) != 0;
            if (u.adjust) {
                for (u32 k = 0; k < 3; ++k) u.gain[k] = float_at(item + 104 + 4 * k);
                f32 h = float_at(item + 116), c = kv::cos(h), sn = kv::sin(h);
                const f32 m[9] = {0.213f + c * 0.787f - sn * 0.213f, 0.715f - c * 0.715f - sn * 0.715f, 0.072f - c * 0.072f + sn * 0.928f,
                                  0.213f - c * 0.213f + sn * 0.143f, 0.715f + c * 0.285f + sn * 0.140f, 0.072f - c * 0.072f - sn * 0.283f,
                                  0.213f - c * 0.213f - sn * 0.787f, 0.715f - c * 0.715f + sn * 0.715f, 0.072f + c * 0.928f + sn * 0.072f};
                memcpy(u.hue_m, m, sizeof m);
                u.sat = float_at(item + 120);
                u.masked = (head & (1u << 23)) != 0;
                if (u.masked) {
                    u.mask = texture_of(r, word_at(item + 124));
                    u.mask_key = word_at(item + 128);
                    for (u32 k = 0; k < 4; ++k) u.mask_rect[k] = float_at(item + 132 + 4 * k);
                }
            }
            // fs_ui's hold: half a texel inside the uv rect.
            f32 hw = 0.5f / (f32)(u.tex.width ? u.tex.width : 1), hh = 0.5f / (f32)(u.tex.height ? u.tex.height : 1);
            for (u32 k = 0; k < 2; ++k) {
                f32 a = u.uv0[k], b = uv1[k], h = k ? hh : hw;
                f32 least = (a < b ? a : b) + h, most = (a < b ? b : a) - h;
                u.uvlo[k] = least;
                u.uvhi[k] = most > least ? most : least;
            }
        }
        u.shadow = u.shade[3] > 0.0f;
        f32 grow = 1.0f;
        if (word_at(item + 20) >> 24) {
            f32 g = kv::fabs(u.so[0]) > kv::fabs(u.so[1]) ? kv::fabs(u.so[0]) : kv::fabs(u.so[1]);
            g += (spread > 0.0f ? spread : 0.0f) + blur * 1.5f + 1.0f;
            grow = g > grow ? g : grow;
        }
        if (u.shadow) {
            u.shx = u.hx + spread; u.shy = u.hy + spread;
            for (u32 k = 0; k < 4; ++k) u.sradii[k] = u.radii[k] + spread;
            f32 sigma = blur * 0.5f > 0.5f ? blur * 0.5f : 0.5f;
            u.inv_sigma = 1.0f / (sigma * 1.41421356f);
        }
        lo[0] = u.rx - grow; lo[1] = u.ry - grow; hi[0] = u.rx + u.rw + grow; hi[1] = u.ry + u.rh + grow;
        // Where the box is exactly its fill: past its corners' radii and its border by half a
        // pixel, with nothing that varies across it (and a shadow only where it is opaque).
        f32 rr = 0.0f;
        for (u32 k = 0; k < 4; ++k) rr = u.radii[k] > rr ? u.radii[k] : rr;
        f32 b = u.hx < u.hy ? u.hx : u.hy;
        rr = rr < b ? rr : b;
        f32 in = 0.5f + (u.border > 0.0f ? u.border : 0.0f);
        in = rr > in ? rr : in;
        if (!u.gradient && !u.textured && (!u.shadow || u.fill[3] >= 1.0f)) {
            u.core[0] = u.cx - u.hx + in; u.core[1] = u.cy - u.hy + in;
            u.core[2] = u.cx + u.hx - in; u.core[3] = u.cy + u.hy - in;
        } else {
            u.core[0] = u.core[1] = 1.0f; // empty
        }
    }
    for (u32 k = 0; k < 4; ++k) u.flat[k] = to16(u.fill[k]);
    for (u32 k = 0; k < 3; ++k) u.flat[k] = u.flat[k] > u.flat[3] ? u.flat[3] : u.flat[k];
    // The quad cut by the clip, then its window pixels.
    lo[0] = lo[0] > cx ? lo[0] : cx; lo[1] = lo[1] > cy ? lo[1] : cy;
    hi[0] = hi[0] < cx + cw ? hi[0] : cx + cw; hi[1] = hi[1] < cy + ch ? hi[1] : cy + ch;
    if (!(lo[0] < hi[0] && lo[1] < hi[1])) return false;
    u.lo[0] = lo[0]; u.lo[1] = lo[1]; u.hi[0] = hi[0]; u.hi[1] = hi[1];
    f32 minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (u32 k = 0; k < 4; ++k) {
        f32 x = k & 1 ? hi[0] : lo[0], y = k & 2 ? hi[1] : lo[1];
        f32 wx = v.m[0] + x * v.m[2] + y * v.m[4], wy = v.m[1] + x * v.m[3] + y * v.m[5];
        if (!(wx > -1e6f && wx < 1e6f && wy > -1e6f && wy < 1e6f)) return false;
        minx = wx < minx ? wx : minx; maxx = wx > maxx ? wx : maxx;
        miny = wy < miny ? wy : miny; maxy = wy > maxy ? wy : maxy;
    }
    u.x0 = (i32)kv::floor(minx); u.x1 = (i32)kv::ceil(maxx) + 1;
    u.y0 = (i32)kv::floor(miny); u.y1 = (i32)kv::ceil(maxy) + 1;
    if (u.x0 < 0) u.x0 = 0;
    if (u.y0 < 0) u.y0 = 0;
    if (u.x1 > (i32)r->target_width) u.x1 = (i32)r->target_width;
    if (u.y1 > (i32)r->target_height) u.y1 = (i32)r->target_height;
    if (u.x0 >= u.x1 || u.y0 >= u.y1) return false;
    u.l0x = v.l0x; u.l0y = v.l0y; u.ldx_x = v.ldx_x; u.ldx_y = v.ldx_y; u.ldy_x = v.ldy_x; u.ldy_y = v.ldy_y;
    u.hash = frame_hash(&u, offsetof(CpuUi, hash));
    return true;
}

struct TileJob {
    Renderer*  r;
    u32        across, tiles;
    const u32* starts;
    const u32* todo; // the tiles to draw, when not every one is
    const CpuRasterPath* path;
};

void draw_tiles(u32, u32 begin, u32 end, void* user) {
    const TileJob& j = *(const TileJob*)user;
    Renderer* r = j.r;
    u16 pixels[kTile * kTile * 4];
    CpuFrame fr;
    fr.luts = &cpu_luts();
    fr.quads = r->quads.data;
    fr.ui = r->ui.data;
    memcpy(fr.clear, r->clear, sizeof fr.clear);
    fr.quantize8 = r->quantize8;
    fr.shaders = &r->shader_base;
    fr.post = r->pass == RcPass::Window && r->post_on ? r->post.fn : nullptr;
    bool world = r->pass == RcPass::World;
    for (u32 i = begin; i < end; ++i) {
        u32 t = j.todo ? j.todo[i] : i;
        u32 tx = (t % j.across) * kTile, ty = (t / j.across) * kTile;
        u32 tw = r->target_width - tx < kTile ? r->target_width - tx : kTile;
        u32 th = r->target_height - ty < kTile ? r->target_height - ty : kTile;
        j.path->raster_tile(fr, r->bin_items.data + j.starts[t], j.starts[t + 1] - j.starts[t], pixels, tx, ty, tw, th);
        if (world) {
            // The world pass: into the post pass's image, as it is (16-bit linear).
            u16* dst = (u16*)r->post.world.data;
            for (u32 y = 0; y < th; ++y) memcpy(dst + ((usize)(ty + y) * r->post.world_width + tx) * 4, pixels + (usize)y * kTile * 4, (usize)tw * 4 * sizeof(u16));
        } else {
            j.path->encode_tile(*fr.luts, pixels, tw, th, r->target + (usize)ty * r->target_pitch + (usize)tx * 4, r->target_pitch, r->target_rgba);
        }
    }
}

} // namespace

// ---- the sort ---------------------------------------------------------------------------------

void rb_sort(Renderer* r, FrameInFlight& f, const PushData& push) {
    KV_ZONE("cpu.sort");
    r->push = push;
    r->layers = (const f32*)f.layers.data;
    u32 rows = push.row_count < r->tables.capacity ? push.row_count : r->tables.capacity;
    const GpuSprite* sprites = sprites_of(r);
    u32* bucket_of = (u32*)r->tables.bucket_of.data;
    u32* totals = totals_of(r);
    u32* starts = starts_of(r);
    memset(totals, 0, sizeof(u32) * kBucketCount);
    for (u32 i = 0; i < rows; ++i) {
        const GpuSprite& s = sprites[i];
        bucket_of[i] = 0xFFFFFFFFu;
        if (!(s.flags & kSpriteAlive)) continue;
        f32 pos[2], rot, sc[2];
        place(r, s, pos, &rot, sc);
        f32 ex = kv::fabs(sc[0]) * s.size[0], ey = kv::fabs(sc[1]) * s.size[1];
        f32 rad = 0.5f * kv::sqrt(ex * ex + ey * ey);
        const f32* c = (s.flags & kSpriteScreen) ? push.screen_cull : push.cull;
        if (pos[0] + rad < c[0] || pos[0] - rad > c[0] + c[2] || pos[1] + rad < c[1] || pos[1] - rad > c[1] + c[3]) continue;
        u32 b = bucket_of_row(r, s);
        bucket_of[i] = b;
        ++totals[b];
    }
    u32 at = 0;
    for (u32 b = 0; b < kBucketCount; ++b) { starts[b] = at; at += totals[b]; }
    starts[kBucketCount] = at; // the visible count, after the starts: the GPU's meta
    MemScope scope;
    u32* cursor = arena_push_n<u32>(*scope.arena, kBucketCount);
    memcpy(cursor, starts, sizeof(u32) * kBucketCount);
    u32* sorted = (u32*)r->tables.sorted.data;
    for (u32 i = 0; i < rows; ++i)
        if (bucket_of[i] != 0xFFFFFFFFu) sorted[cursor[bucket_of[i]]++] = i;
    for (u32 b = 0; rw_next_keyed(r->world, &b); ++b) sort_keyed(r, b);
}

void rb_read_sort(Renderer* r, FrameInFlight&) {
    u32 rows = r->world.sprites.len < r->tables.capacity ? r->world.sprites.len : r->tables.capacity;
    u64 meta = sizeof(u32) * (2 * kBucketCount + 4);
    u64 bytes = sizeof(u32) * (u64)rows * 2 + meta;
    rb_buffer_retire(r, r->check_buffer);
    if (!cpu_alloc(r, bytes, r->check_buffer)) { r->check_pending = false; return; }
    memcpy(r->check_buffer.data, r->tables.sorted.data, sizeof(u32) * rows);
    memcpy(r->check_buffer.data + sizeof(u32) * rows, r->tables.bucket_of.data, sizeof(u32) * rows);
    memcpy(r->check_buffer.data + sizeof(u32) * rows * 2, r->sort_meta.data, meta);
    r->check_rows     = rows;
    r->check_pending  = false;
    r->check_recorded = true;
}

void rb_sort_end(Renderer*, FrameInFlight&, bool) {}

// ---- a pass -----------------------------------------------------------------------------------

void rb_pass_begin(Renderer* r, FrameInFlight& f, RcPass pass, const RenderView& view) {
    if (pass == RcPass::Surface) {
        // Another window's frame is the target until the pass ends; the main one's is kept.
        const Surface& s = r->surfaces[r->surface_at];
        r->main_target = CpuTarget{r->target, r->target_width, r->target_height, r->target_pitch, r->target_rgba};
        r->target = s.frame.pixels;
        r->target_width = s.frame.width;
        r->target_height = s.frame.height;
        r->target_pitch = s.frame.pitch;
        r->target_rgba = s.frame.rgba;
    }
    r->pass = pass;
    r->post_on = false;
    r->clear[0] = cpu_linear16(view.clear_color.r);
    r->clear[1] = cpu_linear16(view.clear_color.g);
    r->clear[2] = cpu_linear16(view.clear_color.b);
    r->quads.clear();
    r->ui.clear();
    r->draws.clear();
    r->layers = (const f32*)f.layers.data;
    // What a game shader reads this pass (shader.h): the push block (rb_bind_sprites sets
    // it), the layers, the materials, every texture slot as the rasterizer reads it.
    r->shader_textures.clear();
    for (u32 i = 0; i < r->textures.len; ++i) r->shader_textures.push(texture_of(r, i));
    CpuShaderCall& c = r->shader_base;
    c = CpuShaderCall{};
    c.push = &r->push;
    c.layers = r->layers;
    c.layer_count = kLayerCount;
    c.materials = (const GpuMaterial*)r->material_table.data;
    c.material_count = kMaxMaterials;
    c.textures = r->shader_textures.data;
    c.texture_count = r->shader_textures.len;
    c.world = &r->world_view;
    c.post = &r->post_push;
    c.params = r->post.params;
    c.param_count = r->post.param_count;
}

// The window pass begins with the post shader over the world image: each tile's ground is the
// shader's picture instead of the clear (raster.cc).
void rb_pass_post(Renderer* r, FrameInFlight&, const RenderView& view) {
    PostPass& p = r->post;
    if (!p.fn || !p.world.data) return;
    r->post_on = true;
    r->world_view = CpuWorld{(const u16*)p.world.data, p.world_width, p.world_height};
    CpuPostPush& pp = r->post_push;
    pp = CpuPostPush{};
    // The target's size (post_fragment.slangh): the shader's uv is the pixel over it. At a
    // render scale that is the smaller target's, as a GPU's would be at that density.
    pp.size[0] = (f32)p.world_width;
    pp.size[1] = (f32)p.world_height;
    pp.time = (f32)(time_now() - r->start_time);
    pp.tick = (u32)r->world.tick;
    pp.count = p.param_count;
    pp.alpha = view.alpha;
    r->shader_base.param_count = p.param_count;
}

void rb_bind_sprites(Renderer* r, FrameInFlight&, const PushData& push) { r->push = push; }

u32 rb_draw_run(Renderer* r, FrameInFlight&, const DrawRun& run) {
    if (!r->target) return 0;
    const u32* totals = totals_of(r);
    const u32* starts = starts_of(r);
    const u32* sorted = (const u32*)r->tables.sorted.data;
    for (u32 b = run.first; b <= run.last && b < kBucketCount; ++b)
        for (u32 k = 0; k < totals[b]; ++k) {
            CpuQuad q;
            if (prepare(r, sorted[starts[b] + k], q)) { r->draws.push(r->quads.len); r->quads.push(q); }
        }
    return 1;
}

bool rb_ui_buffer(Renderer* r, FrameInFlight& f, u64 bytes) {
    Buffer b;
    if (!cpu_alloc(r, bytes, b)) return false;
    rb_buffer_retire(r, f.ui);
    f.ui = b;
    return true;
}

// A layer's list: its items, in order, after the sprites drawn so far.
void rb_ui_draw(Renderer* r, FrameInFlight& f, const PushData& push) {
    if (!r->target || !f.ui.data) return;
    f64 t0 = r->stats ? time_now() : 0.0;
    r->push = push;
    UiView v = ui_view(r);
    if (!v.ok) return;
    const u8* region = f.ui.data + push.ui_base;
    u32 items = word_at(region);
    if (items > push.ui_count) items = push.ui_count;
    for (u32 i = 0; i < items; ++i) {
        CpuUi u;
        if (prepare_ui(r, v, region, region + word_at(region + 16 + 4 * i), u)) {
            r->draws.push(r->ui.len | kCpuDrawUi);
            r->ui.push(u);
        }
    }
    if (r->stats) r->stat_ui += time_now() - t0;
}

// The pass's draws into tiles, in order, and the tiles drawn: a job each. In the window
// pass a tile whose draws hash as last frame's did, into the same target, is not drawn.
// A Surface pass's end gives the main window back its target.
static void restore_target(Renderer* r) {
    const CpuTarget& m = r->main_target;
    r->target = m.pixels;
    r->target_width = m.width;
    r->target_height = m.height;
    r->target_pitch = m.pitch;
    r->target_rgba = m.rgba;
}

static void pass_end(Renderer* r);

void rb_pass_end(Renderer* r, FrameInFlight&, RcPass pass) {
    pass_end(r);
    if (pass == RcPass::Surface) restore_target(r);
}

static void pass_end(Renderer* r) {
    if (!r->target) return;
    KV_ZONE("cpu.raster");
    f64 t0 = r->stats ? time_now() : 0.0;
    u32 across = (r->target_width + kTile - 1) / kTile, down = (r->target_height + kTile - 1) / kTile, tiles = across * down;
    auto bounds = [&](u32 d, i32* x0, i32* y0, i32* x1, i32* y1) {
        if (d & kCpuDrawUi) { const CpuUi& u = r->ui[d & ~kCpuDrawUi]; *x0 = u.x0; *y0 = u.y0; *x1 = u.x1; *y1 = u.y1; }
        else { const CpuQuad& q = r->quads[d]; *x0 = q.x0; *y0 = q.y0; *x1 = q.x1; *y1 = q.y1; }
    };
    r->bin_counts.clear();
    for (u32 t = 0; t <= tiles; ++t) r->bin_counts.push(0);
    for (u32 d : r->draws) {
        i32 x0, y0, x1, y1;
        bounds(d, &x0, &y0, &x1, &y1);
        for (u32 ty = (u32)y0 / kTile; ty <= (u32)(y1 - 1) / kTile; ++ty)
            for (u32 tx = (u32)x0 / kTile; tx <= (u32)(x1 - 1) / kTile; ++tx) ++r->bin_counts[ty * across + tx + 1];
    }
    for (u32 t = 1; t <= tiles; ++t) r->bin_counts[t] += r->bin_counts[t - 1];
    r->bin_items.clear();
    for (u32 i = 0; i < r->bin_counts[tiles]; ++i) r->bin_items.push(0);
    MemScope scope;
    u32* fill = arena_push_n<u32>(*scope.arena, tiles);
    memcpy(fill, r->bin_counts.data, sizeof(u32) * tiles);
    for (u32 d : r->draws) {
        i32 x0, y0, x1, y1;
        bounds(d, &x0, &y0, &x1, &y1);
        for (u32 ty = (u32)y0 / kTile; ty <= (u32)(y1 - 1) / kTile; ++ty)
            for (u32 tx = (u32)x0 / kTile; tx <= (u32)(x1 - 1) / kTile; ++tx) r->bin_items[fill[ty * across + tx]++] = d;
    }

    // The tiles' hashes: their draws' in order, over the clear.
    // A window pass over the post shader's picture reads the whole world image, wherever its
    // shader looks: drawn whole.
    u8* skip = nullptr;
    if (r->pass == RcPass::Window && r->tile_cache && !r->post_on) {
        bool same_target = r->hashed_target == r->target && r->hashed_width == r->target_width && r->hashed_height == r->target_height &&
                           r->hashed_pitch == r->target_pitch && r->hashed_rgba == r->target_rgba && r->tile_hash.len == tiles;
        if (!same_target) {
            r->tile_hash.clear();
            for (u32 t = 0; t < tiles; ++t) r->tile_hash.push(0);
        }
        skip = arena_push_n<u8>(*scope.arena, tiles);
        u64 base = frame_hash_mix(frame_hash_mix(0x2545F4914F6CDD1Dull, (u64)r->clear[0] | (u64)r->clear[1] << 16 | (u64)r->clear[2] << 32), r->quantize8);
        for (u32 t = 0; t < tiles; ++t) {
            u64 h = base;
            for (u32 i = r->bin_counts[t]; i < r->bin_counts[t + 1]; ++i) {
                u32 d = r->bin_items[i];
                h = frame_hash_mix(h, (d & kCpuDrawUi) ? r->ui[d & ~kCpuDrawUi].hash : r->quads[d].hash);
            }
            h |= 1; // never 0: 0 is a tile never drawn
            skip[t] = same_target && r->tile_hash[t] == h;
            r->tile_hash[t] = h;
        }
        r->hashed_target = r->target;
        r->hashed_width = r->target_width;
        r->hashed_height = r->target_height;
        r->hashed_pitch = r->target_pitch;
        r->hashed_rgba = r->target_rgba;
    } else if (r->pass == RcPass::Window) {
        r->tile_hash.clear(); // drawn whole: the next frame's hashes start over
    }
    // The tiles to draw: a job each.
    u32* todo = nullptr;
    u32 count = tiles;
    if (skip) {
        todo = arena_push_n<u32>(*scope.arena, tiles);
        count = 0;
        for (u32 t = 0; t < tiles; ++t)
            if (!skip[t]) todo[count++] = t;
    }
    u32 skipped = tiles - count;
    if (r->pass == RcPass::Window) { r->tiles_skipped = skipped; r->tiles_drawn = count; }

    cpu_luts(); // made here, on this thread, before the jobs read them
    TileJob j{r, across, tiles, r->bin_counts.data, todo, &cpu_raster_path()};
    if (count) jobs_run(count, 1, draw_tiles, &j, "cpu.tiles");
    // The pass ends of a frame are summed (the world's and the window's, with a post shader);
    // the line counts window passes, one a frame.
    if (r->stats) {
        f64 dt = time_now() - t0;
        r->stat_end += dt;
        r->stat_frame_end += dt;
    }
    if (r->stats && r->pass == RcPass::Window) {
        r->stat_end_worst = r->stat_frame_end > r->stat_end_worst ? r->stat_frame_end : r->stat_end_worst;
        r->stat_frame_end = 0.0;
        r->stat_drawn += tiles - skipped;
        r->stat_skipped += skipped;
        r->stat_items += r->draws.len;
        if (++r->stat_passes == r->stat_every) {
            f64 n = r->stat_passes;
            KV_INFO(KV_RB_NAME, "stats: %ux%u, %s kernels, %.0f draws; record %.3f ms (UI lists %.3f, pass end %.3f, worst %.3f), present %.3f ms; tiles %.1f drawn, %.1f skipped",
                    r->target_width, r->target_height, cpu_raster_path().name, (f64)r->stat_items / n, r->stat_record * 1000.0 / n,
                    r->stat_ui * 1000.0 / n, r->stat_end * 1000.0 / n, r->stat_end_worst * 1000.0, r->stat_present * 1000.0 / n,
                    (f64)r->stat_drawn / n, (f64)r->stat_skipped / n);
            r->stat_passes = 0;
            r->stat_ui = r->stat_end = r->stat_end_worst = r->stat_record = r->stat_present = 0.0;
            r->stat_drawn = r->stat_skipped = r->stat_items = 0;
        }
    }
}

} } // namespace kv::KV_RB_NS
