// Korvid render: the render world (korvid/render/world.h). No graphics API, no backend.
#include "korvid/render/world.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/arena.h"
#include "korvid/math/math.h"
#include "korvid/math/scalar.h"
#include <string.h>

namespace kv {

const char* blend_mode_name(BlendMode mode) {
    switch (mode) {
    case BlendMode::Opaque:   return "opaque";
    case BlendMode::Alpha:    return "alpha";
    case BlendMode::Additive: return "additive";
    default:                  return "?";
    }
}

bool blend_mode_parse(const char* s, BlendMode* out) {
    if (!s) return false;
    if (strcmp(s, "opaque") == 0)   { *out = BlendMode::Opaque;   return true; }
    if (strcmp(s, "alpha") == 0)    { *out = BlendMode::Alpha;    return true; }
    if (strcmp(s, "additive") == 0) { *out = BlendMode::Additive; return true; }
    return false;
}

bool rw_init(RenderWorld& w, Allocator* general) {
    w.sprite_of_entity.init(general);
    w.material_by_name.init(general);
    w.sprite_dirty.alloc = w.material_dirty.alloc = general;
    w.ui.alloc = general;
    w.material_dirty.resize_fill((kMaxMaterials + 63) / 64, 0);
    return w.materials.init(kMaxMaterials, general->tag) && w.sprites.init(kMaxSprites, general->tag);
}

void rw_free(RenderWorld& w) {
    w.materials.free();
    w.material_by_name.free();
    w.sprites.free();
    w.sprite_of_entity.free();
    w.sprite_dirty.free();
    w.material_dirty.free();
    for (RwUiLayer& l : w.ui) l.bytes.free();
    w.ui.free();
    w.ui_bytes = 0;
}

u32 rw_bucket_of(i32 layer, u32 slot) {
    if (layer < kLayerMin) layer = kLayerMin;
    if (layer > kLayerMax) layer = kLayerMax;
    u32 b = slot < kSlotCount ? slot : (u32)BlendMode::Alpha;
    return (u32)(layer - kLayerMin) * kSlotCount + b;
}

u32 rw_material_row_of(const RenderWorld& w, Handle material) {
    return w.materials.valid(material) ? material.index() : 0;
}

const MaterialRecord& rw_material_at(const RenderWorld& w, u32 row) {
    return w.materials.is_alive(row) ? w.materials.items[row] : w.materials.items[0];
}

void rw_material_row(const MaterialRecord& m, u32 texture_slot, GpuMaterial& out) {
    out = GpuMaterial{};
    out.tint[0] = m.tint.r; out.tint[1] = m.tint.g; out.tint[2] = m.tint.b; out.tint[3] = m.tint.a;
    for (u32 i = 0; i < KV_MATERIAL_PARAMS; ++i) out.params[i] = m.params[i];
    out.texture = texture_slot;
    out.blend   = (u32)m.blend;
    out.filter  = (u32)m.filter;
    out.slot    = m.slot;
}

void rw_mark_sprite_dirty(RenderWorld& w, u32 slot) {
    u32 word = slot >> 6;
    if (word >= w.sprite_dirty.len) w.sprite_dirty.resize_fill(word + 1, 0);
    w.sprite_dirty[word] |= (u64)1 << (slot & 63);
}

void rw_mark_material_dirty(RenderWorld& w, u32 row) {
    w.material_dirty[row >> 6] |= (u64)1 << (row & 63);
}

void rw_rebind_material(RenderWorld& w, u32 row) {
    u32 slot = rw_material_at(w, row).slot;
    for (u32 i = 0; i < w.sprites.len; ++i) {
        if (!w.sprites.is_alive(i)) continue;
        RenderSprite& s = w.sprites.items[i];
        if (s.gpu.material != row) continue;
        if (!w.materials.is_alive(row)) s.gpu.material = 0; // the material is gone: the default
        u32 bucket = rw_bucket_of(s.gpu.layer, slot);
        if (bucket != s.bucket) {
            --w.bucket_counts[s.bucket];
            ++w.bucket_counts[bucket];
            if (s.keyed) { --w.keyed_counts[s.bucket]; ++w.keyed_counts[bucket]; }
            s.bucket = bucket;
        }
        w.sprite_dirty[i >> 6] |= (u64)1 << (i & 63);
    }
}

void rw_stage_sprites(const RenderWorld& w, u32 a, u32 b, GpuSprite* dst) {
    for (u32 i = a; i < b; ++i) {
        if (w.sprites.is_alive(i)) dst[i - a] = w.sprites.items[i].gpu;
        else dst[i - a] = GpuSprite{}; // a dead slot: not alive, never drawn
    }
}

// ---- the deltas -----------------------------------------------------------------------------

namespace {

void write_placement(f32 out[4], f32* scale_y, const Placement2& p) {
    out[0] = p.position.x; out[1] = p.position.y; out[2] = p.rotation; out[3] = p.scale.x;
    *scale_y = p.scale.y;
}

} // namespace

void rw_apply(RenderWorld& w, Span<const SpriteDelta> deltas) {
    for (const SpriteDelta& d : deltas) {
        ++w.deltas_applied;
        switch (d.kind) {
        case SpriteDeltaKind::Tick:
            // A new tick. Rows keep their stamps; a row's current placement becomes its
            // previous one the first time it is set in a later tick, and a row that is not
            // set did not move. Nothing is visited.
            ++w.tick;
            memcpy(w.layer_prev, w.layer_cur, sizeof w.layer_prev); // the layers' offsets likewise, all 256 of them
            break;
        case SpriteDeltaKind::Layer: {
            i32 l = d.state.layer < kLayerMin ? kLayerMin : d.state.layer > kLayerMax ? kLayerMax : d.state.layer;
            f32* cur = w.layer_cur[l - kLayerMin];
            cur[0] = d.state.placement.position.x;
            cur[1] = d.state.placement.position.y;
            if (d.state.snap) memcpy(w.layer_prev[l - kLayerMin], cur, sizeof(f32) * 2);
            break;
        }
        case SpriteDeltaKind::Set: {
            const RenderSpriteState& st = d.state;
            u32 material = rw_material_row_of(w, st.material);
            u32 bucket   = rw_bucket_of(st.layer, w.materials.items[material].slot);
            RenderSprite* s;
            u32 slot;
            if (u32* found = w.sprite_of_entity.find(d.entity)) {
                slot = *found;
                s = &w.sprites.items[slot];
                if (s->gpu.stamp != w.tick) { // first Set this tick: where it is now is where it was
                    memcpy(s->gpu.prev, s->gpu.cur, sizeof s->gpu.prev);
                    s->gpu.scale_y[0] = s->gpu.scale_y[1];
                    s->gpu.stamp = w.tick;
                }
                if (st.snap) write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement); // a teleport: no history
                if (bucket != s->bucket) {
                    --w.bucket_counts[s->bucket];
                    ++w.bucket_counts[bucket];
                    if (s->keyed) { --w.keyed_counts[s->bucket]; ++w.keyed_counts[bucket]; }
                    s->bucket = bucket;
                }
            } else {
                RenderSprite fresh{};
                fresh.entity = d.entity;
                fresh.bucket = bucket;
                Handle h = w.sprites.create(fresh);
                if (h.is_null()) { KV_ERROR("render", "render world is full (%u sprites)", kMaxSprites); break; }
                slot = h.index();
                s = &w.sprites.items[slot];
                w.sprite_of_entity.set(d.entity, slot);
                ++w.bucket_counts[bucket];
                write_placement(s->gpu.prev, &s->gpu.scale_y[0], st.placement); // born here: no history
                s->gpu.stamp = w.tick;
            }
            write_placement(s->gpu.cur, &s->gpu.scale_y[1], st.placement);
            s->gpu.size[0] = st.size.x; s->gpu.size[1] = st.size.y;
            s->gpu.uv[0] = st.uv.x; s->gpu.uv[1] = st.uv.y; s->gpu.uv[2] = st.uv.w; s->gpu.uv[3] = st.uv.h;
            s->gpu.tint[0] = st.tint.r; s->gpu.tint[1] = st.tint.g; s->gpu.tint[2] = st.tint.b; s->gpu.tint[3] = st.tint.a;
            s->gpu.material = material;
            s->gpu.layer    = st.layer;
            i32 ax = 0, ay = 0;
            screen_anchor_dir(st.anchor, &ax, &ay);
            u32 anchor = (u32)(ax < 0 ? 1 : ax > 0 ? 2 : 0) | (u32)(ay < 0 ? 1 : ay > 0 ? 2 : 0) << 2;
            s->gpu.flags    = kSpriteAlive | (st.space == SpriteSpace::Screen ? kSpriteScreen : 0) | ((u32)st.sort << kSpriteSortShift) |
                              (anchor << kSpriteAnchorShift);
            s->gpu.order    = st.order;
            bool keyed = st.sort != SpriteSort::Row;
            if (keyed != s->keyed) { if (keyed) ++w.keyed_counts[bucket]; else --w.keyed_counts[bucket]; s->keyed = keyed; }
            rw_mark_sprite_dirty(w, slot);
            break;
        }
        case SpriteDeltaKind::Remove: {
            if (u32* found = w.sprite_of_entity.find(d.entity)) {
                u32 slot = *found;
                --w.bucket_counts[w.sprites.items[slot].bucket];
                if (w.sprites.items[slot].keyed) --w.keyed_counts[w.sprites.items[slot].bucket];
                w.sprites.destroy(w.sprites.handle_at(slot));
                w.sprite_of_entity.remove(d.entity);
                rw_mark_sprite_dirty(w, slot); // the row is rewritten dead
            }
            break;
        }
        }
    }
}

// ---- the frame's arithmetic -----------------------------------------------------------------

bool rw_next_run(const u32* bucket_counts, u32* cursor, u32 end, DrawRun* out) {
    u32 b = *cursor;
    while (b < end && !bucket_counts[b]) ++b;
    if (b >= end) { *cursor = end; return false; }
    u32 slot = b % kSlotCount;
    u32 last = b;
    for (u32 n = b + 1; n < end; ++n) {
        if (!bucket_counts[n]) continue;
        if (n % kSlotCount != slot) break;
        last = n;
    }
    *out = DrawRun{slot, b, last};
    *cursor = last + 1;
    return true;
}

bool rw_next_keyed(const RenderWorld& w, u32* bucket) {
    for (u32 b = *bucket; b < kBucketCount; ++b)
        if (w.keyed_counts[b] && w.bucket_counts[b]) { *bucket = b; return true; }
    return false;
}

void rw_run_label(char* out, usize cap, const DrawRun& run, const char* what) {
    format(out, cap, "layers %d..%d %s", (i32)(run.first / kSlotCount) + kLayerMin, (i32)(run.last / kSlotCount) + kLayerMin, what);
}

PushData rw_push_block(const RenderWorld& w, const RenderView& view, f32 sw, f32 sh, f32 time, bool clip_y_up) {
    point2 center = rect_center(view.view);
    affine2 proj  = ortho2d(rect2_from_center(point2{0, 0}, rect_size(view.view))) * affine2_rotate(-view.rotation) *
                    affine2_translate(vec2{-center.x, -center.y});
    f32 cs = kv::fabs(kv::cos(view.rotation)), sn = kv::fabs(kv::sin(view.rotation));
    f32 hx = 0.5f * (view.view.w * cs + view.view.h * sn), hy = 0.5f * (view.view.w * sn + view.view.h * cs);
    affine2 screen = ortho2d(rect2{-sw * 0.5f, -sh * 0.5f, sw, sh});
    if (clip_y_up) { // y' = -(b x + d y + ty)
        proj.b = -proj.b;     proj.d = -proj.d;     proj.ty = -proj.ty;
        screen.b = -screen.b; screen.d = -screen.d; screen.ty = -screen.ty;
    }
    PushData push{};
    push.view[0] = proj.a; push.view[1] = proj.b; push.view[2] = proj.c; push.view[3] = proj.d;
    push.view_trans[0] = proj.tx; push.view_trans[1] = proj.ty;
    push.alpha     = view.alpha;
    push.tick      = w.tick;
    push.cull[0] = center.x - hx; push.cull[1] = center.y - hy; push.cull[2] = 2.0f * hx; push.cull[3] = 2.0f * hy;
    push.screen[0] = screen.a; push.screen[1] = screen.b; push.screen[2] = screen.c; push.screen[3] = screen.d;
    push.screen_trans[0] = screen.tx; push.screen_trans[1] = screen.ty;
    push.row_count = w.sprites.len;
    push.screen_cull[0] = -sw * 0.5f; push.screen_cull[1] = -sh * 0.5f; push.screen_cull[2] = sw; push.screen_cull[3] = sh;
    push.time = time;
    return push;
}

// ---- the sort check -------------------------------------------------------------------------

void rw_snapshot_keys(const RenderWorld& w, f32 alpha, f32* keys) {
    u32 rows = w.sprites.len;
    for (u32 i = 0; i < rows; ++i) {
        const GpuSprite& g = w.sprites.items[i].gpu;
        u32 mode = (g.flags >> kSpriteSortShift) & 3;
        f32 t = g.stamp == w.tick ? alpha : 1.0f;
        f32 y = g.prev[1] + (g.cur[1] - g.prev[1]) * t;
        keys[i] = !w.sprites.is_alive(i) ? 0.0f : mode == 1 ? g.order : mode == 2 ? -(y + g.order) : 0.0f;
    }
}

bool rw_verify_sort(const RenderWorld& w, const u32* readback, u32 rows, const f32* keys, const char* tag) {
    const u32* sorted    = readback;
    const u32* bucket_of = sorted + rows;
    const u32* meta      = bucket_of + rows;
    const u32* totals    = meta;
    const u32* starts    = meta + kBucketCount;
    u32 visible_rows = 0, bad = 0, seen_total = 0, in_use = 0;
    MemScope scope;
    u8* seen = arena_push_n<u8>(*scope.arena, rows ? rows : 1);
    memset(seen, 0, rows);
    for (u32 i = 0; i < rows; ++i) if (bucket_of[i] != 0xFFFFFFFFu) ++visible_rows;
    u32 expect_start = 0;
    for (u32 b = 0; b < kBucketCount; ++b) {
        if (starts[b] != expect_start) { ++bad; KV_ERROR(tag, "sort check: bucket %u starts at %u, expected %u", b, starts[b], expect_start); }
        expect_start += totals[b];
        in_use += totals[b] != 0;
        if (totals[b] > w.bucket_counts[b]) { ++bad; KV_ERROR(tag, "sort check: bucket %u has %u rows on the GPU, %u on the CPU", b, totals[b], w.bucket_counts[b]); }
        // In (key, row) order: a bucket with no keyed rows has every key 0, so that is row
        // order. Keys within a hair of each other may come in either order (the GPU and the
        // CPU interpolate with different roundings); past kKeySortMax rows, row order resumes.
        u32 prev = 0;
        for (u32 k = 0; k < totals[b]; ++k) {
            u32 row = sorted[starts[b] + k];
            if (row >= rows || bucket_of[row] != b) { if (bad++ < 8) KV_ERROR(tag, "sort check: bucket %u entry %u is row %u (bucket %u)", b, k, row, row < rows ? bucket_of[row] : ~0u); continue; }
            f32 ka = keys && k ? keys[prev] : 0.0f, kb = keys ? keys[row] : 0.0f;
            bool ordered = true;
            if (!k || k == kKeySortMax) ordered = true;                          // the first, or the first unsorted
            else if (k > kKeySortMax || !keys || ka == kb) ordered = row > prev;
            else if (kv::fabs(ka - kb) <= 1e-3f * (1.0f + kv::fabs(ka))) ordered = true; // a near tie: either order
            else ordered = ka < kb;
            if (!ordered) { if (bad++ < 8) KV_ERROR(tag, "sort check: bucket %u entry %u row %u (key %g) after row %u (key %g)", b, k, row, kb, prev, ka); }
            if (seen[row]++) { if (bad++ < 8) KV_ERROR(tag, "sort check: row %u listed twice", row); }
            prev = row;
            ++seen_total;
        }
    }
    if (seen_total != visible_rows || meta[2 * kBucketCount] != visible_rows) {
        ++bad;
        KV_ERROR(tag, "sort check: %u rows visible, %u listed, GPU says %u", visible_rows, seen_total, meta[2 * kBucketCount]);
    }
    KV_INFO(tag, "sort check: %u rows, %u visible, %u buckets in use: %s", rows, visible_rows, in_use, bad ? "FAILED" : "ok");
    return bad == 0;
}

} // namespace kv
