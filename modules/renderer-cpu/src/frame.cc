// renderer-cpu's frame: the sort and the cull as the GPU's compute passes do them, the draw
// runs made into quads in the window's pixels, and a pass's quads binned into tiles and
// drawn as a job batch (docs/PRDs/CPU-Renderer-PRD.md, milestone 2). The placement follows
// sprite_common.slangh's row_placement and sprite.slang's vs_main line for line.
#include "renderer_internal.h"
#include "korvid/core/job.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv { namespace KV_RB_NS {

namespace {

const GpuSprite* sprites_of(const Renderer* r) { return (const GpuSprite*)r->tables.sprites.data; }
const GpuMaterial* materials_of(const Renderer* r) { return (const GpuMaterial*)r->material_table.data; }
u32* totals_of(Renderer* r) { return (u32*)r->sort_meta.data; }
u32* starts_of(Renderer* r) { return (u32*)r->sort_meta.data + kBucketCount; }

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

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
    // Its texture: the slot's, else the default (slot 0) while it has no pixels yet.
    u32 slot = m.texture < r->textures.len && r->textures.is_alive(m.texture) && r->textures.items[m.texture].resident ? m.texture : 0;
    const Texture& t = r->textures.items[slot];
    q.tex = CpuTexture{t.pixels.data, t.width, t.height, t.format == TextureFormat::RGBA8_SRGB};
    q.filter = m.filter ? 1 : 0;
    q.blend = (u8)(m.blend < 3 ? m.blend : 1);
    return true;
}

struct TileJob {
    Renderer* r;
    u32       across, tiles;
    const u32* starts;
};

void draw_tiles(u32, u32 begin, u32 end, void* user) {
    const TileJob& j = *(const TileJob*)user;
    Renderer* r = j.r;
    u16 pixels[kTile * kTile * 4];
    for (u32 t = begin; t < end; ++t) {
        u32 tx = (t % j.across) * kTile, ty = (t / j.across) * kTile;
        u32 tw = r->target_width - tx < kTile ? r->target_width - tx : kTile;
        u32 th = r->target_height - ty < kTile ? r->target_height - ty : kTile;
        cpu_raster_tile(r->quads.data, r->bin_items.data + j.starts[t], j.starts[t + 1] - j.starts[t], pixels, tx, ty, tw, th, r->clear, r->quantize8);
        cpu_encode_tile(pixels, tw, th, r->target + (usize)ty * r->target_pitch + (usize)tx * 4, r->target_pitch, r->target_rgba);
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

void rb_pass_begin(Renderer* r, FrameInFlight& f, RcPass, const RenderView& view) {
    r->clear[0] = cpu_linear16(view.clear_color.r);
    r->clear[1] = cpu_linear16(view.clear_color.g);
    r->clear[2] = cpu_linear16(view.clear_color.b);
    r->quads.clear();
    r->layers = (const f32*)f.layers.data;
}

void rb_pass_post(Renderer*, FrameInFlight&, const RenderView&) {} // milestone 4

void rb_bind_sprites(Renderer* r, FrameInFlight&, const PushData& push) { r->push = push; }

u32 rb_draw_run(Renderer* r, FrameInFlight&, const DrawRun& run) {
    if (!r->target) return 0;
    const u32* totals = totals_of(r);
    const u32* starts = starts_of(r);
    const u32* sorted = (const u32*)r->tables.sorted.data;
    for (u32 b = run.first; b <= run.last && b < kBucketCount; ++b)
        for (u32 k = 0; k < totals[b]; ++k) {
            CpuQuad q;
            if (prepare(r, sorted[starts[b] + k], q)) r->quads.push(q);
        }
    return 1;
}

// The pass's quads into tiles, in order, and the tiles drawn: a job each.
void rb_pass_end(Renderer* r, FrameInFlight&, RcPass) {
    if (!r->target) return;
    KV_ZONE("cpu.raster");
    u32 across = (r->target_width + kTile - 1) / kTile, down = (r->target_height + kTile - 1) / kTile, tiles = across * down;
    r->bin_counts.clear();
    for (u32 t = 0; t <= tiles; ++t) r->bin_counts.push(0);
    for (const CpuQuad& q : r->quads)
        for (u32 ty = (u32)q.y0 / kTile; ty <= (u32)(q.y1 - 1) / kTile; ++ty)
            for (u32 tx = (u32)q.x0 / kTile; tx <= (u32)(q.x1 - 1) / kTile; ++tx) ++r->bin_counts[ty * across + tx + 1];
    for (u32 t = 1; t <= tiles; ++t) r->bin_counts[t] += r->bin_counts[t - 1];
    r->bin_items.clear();
    for (u32 i = 0; i < r->bin_counts[tiles]; ++i) r->bin_items.push(0);
    MemScope scope;
    u32* fill = arena_push_n<u32>(*scope.arena, tiles);
    memcpy(fill, r->bin_counts.data, sizeof(u32) * tiles);
    for (u32 i = 0; i < r->quads.len; ++i) {
        const CpuQuad& q = r->quads[i];
        for (u32 ty = (u32)q.y0 / kTile; ty <= (u32)(q.y1 - 1) / kTile; ++ty)
            for (u32 tx = (u32)q.x0 / kTile; tx <= (u32)(q.x1 - 1) / kTile; ++tx) r->bin_items[fill[ty * across + tx]++] = i;
    }
    cpu_luts(); // made here, on this thread, before the jobs read them
    TileJob j{r, across, tiles, r->bin_counts.data};
    jobs_run(tiles, 1, draw_tiles, &j, "cpu.tiles");
}

} } // namespace kv::KV_RB_NS
