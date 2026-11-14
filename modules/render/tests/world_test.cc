// The render world with no GPU: a delta stream in; the rows, the dirty bits and the bucket
// counts out. What every backend's frame is built from (korvid/render/world.h).
#include "korvid/render/world.h"
#include "korvid/core/memory.h"
#include "korvid/core/test.h"
#include <string.h>

using namespace kv;

namespace {

struct World {
    RenderWorld w;
    Handle      plain, add; // materials: row 0 is the default (alpha), then one additive
    World() {
        rw_init(w, mem_general());
        MaterialRecord def{};
        def.blend = BlendMode::Alpha; def.slot = (u32)BlendMode::Alpha; def.tint = color_white();
        plain = w.materials.create(def);
        MaterialRecord a = def;
        a.blend = BlendMode::Additive; a.slot = (u32)BlendMode::Additive;
        add = w.materials.create(a);
    }
    ~World() { rw_free(w); }
};

SpriteDelta tick() { SpriteDelta d{}; d.kind = SpriteDeltaKind::Tick; return d; }

SpriteDelta set(u64 entity, f32 x, f32 y, i32 layer, Handle material, SpriteSort sort = SpriteSort::Row, f32 order = 0.0f) {
    SpriteDelta d{};
    d.kind = SpriteDeltaKind::Set;
    d.entity = Handle{entity};
    d.state.placement.position = point2{x, y};
    d.state.material = material;
    d.state.uv = rect2{0, 0, 1, 1};
    d.state.size = vec2{2, 2};
    d.state.tint = color_white();
    d.state.layer = layer;
    d.state.sort = sort;
    d.state.order = order;
    return d;
}

SpriteDelta remove(u64 entity) { SpriteDelta d{}; d.kind = SpriteDeltaKind::Remove; d.entity = Handle{entity}; return d; }

void apply(RenderWorld& w, const SpriteDelta& d) { rw_apply(w, Span<const SpriteDelta>{&d, 1}); }

u32 dirty_rows(RenderWorld& w, u32* runs = nullptr) {
    u32 n = 0;
    u32 covered = rw_dirty_runs(w.sprite_dirty, w.sprites.len, 0, [&](u32, u32) { ++n; });
    if (runs) *runs = n;
    return covered;
}

u32 total(const u32* counts) { u32 n = 0; for (u32 b = 0; b < kBucketCount; ++b) n += counts[b]; return n; }

} // namespace

void test_world_apply(TestCtx& c) {
    c.begin("world_apply");
    World t;
    RenderWorld& w = t.w;
    KV_CHECK(c, t.plain.index() == 0 && t.add.index() == 1);

    // A sprite is born where it is: previous equals current, stamped with the tick, dirty.
    apply(w, tick());
    apply(w, set(7, 10, 20, 3, t.plain));
    KV_CHECK(c, w.sprites.live == 1 && w.tick == 1 && w.deltas_applied == 2);
    u32* slot = w.sprite_of_entity.find(Handle{7});
    KV_CHECK(c, slot != nullptr);
    const GpuSprite& g = w.sprites.items[*slot].gpu;
    KV_CHECK(c, g.cur[0] == 10 && g.cur[1] == 20 && g.prev[0] == 10 && g.prev[1] == 20 && g.stamp == 1);
    KV_CHECK(c, (g.flags & kSpriteAlive) && !(g.flags & kSpriteScreen) && g.layer == 3 && g.material == 0);
    KV_CHECK(c, w.bucket_counts[rw_bucket_of(3, (u32)BlendMode::Alpha)] == 1 && total(w.bucket_counts) == 1);
    KV_CHECK(c, dirty_rows(w) == 1);
    KV_CHECK(c, dirty_rows(w) == 0); // the walk cleared the bits

    // The first Set of a later tick moves current to previous; a second Set in that tick
    // leaves previous alone.
    apply(w, tick());
    apply(w, set(7, 11, 20, 3, t.plain));
    KV_CHECK(c, g.prev[0] == 10 && g.cur[0] == 11 && g.stamp == 2);
    apply(w, set(7, 12, 20, 3, t.plain));
    KV_CHECK(c, g.prev[0] == 10 && g.cur[0] == 12);
    KV_CHECK(c, dirty_rows(w) == 1);

    // A tick with no Set touches no row and dirties nothing.
    apply(w, tick());
    KV_CHECK(c, g.stamp == 2 && dirty_rows(w) == 0);

    // A snap forgets where the sprite was.
    SpriteDelta snap = set(7, 50, 60, 3, t.plain);
    snap.state.snap = true;
    apply(w, snap);
    KV_CHECK(c, g.prev[0] == 50 && g.prev[1] == 60 && g.cur[0] == 50);

    // A change of layer or material moves the count from one bucket to the other; keyed
    // sprites are counted beside the plain ones.
    apply(w, set(7, 50, 60, -4, t.add, SpriteSort::Key, 2.0f));
    u32 b = rw_bucket_of(-4, (u32)BlendMode::Additive);
    KV_CHECK(c, w.bucket_counts[b] == 1 && total(w.bucket_counts) == 1 && w.keyed_counts[b] == 1 && total(w.keyed_counts) == 1);
    KV_CHECK(c, g.material == 1 && ((g.flags >> kSpriteSortShift) & 3) == (u32)SpriteSort::Key && g.order == 2.0f);
    apply(w, set(7, 50, 60, -4, t.add));
    KV_CHECK(c, w.bucket_counts[b] == 1 && total(w.keyed_counts) == 0);

    // Layers clamp into the bucket range; a stale material is the default's row.
    KV_CHECK(c, rw_bucket_of(-1000, 0) == 0 && rw_bucket_of(1000, 7) == kBucketCount - 1);
    KV_CHECK(c, rw_material_row_of(w, Handle{}) == 0 && rw_material_row_of(w, t.add) == 1);

    // Screen sprites carry their anchor.
    SpriteDelta hud = set(8, 0, 0, 9, t.plain);
    hud.state.space = SpriteSpace::Screen;
    hud.state.anchor = ScreenAnchor::TopLeft;
    apply(w, hud);
    const GpuSprite& h = w.sprites.items[*w.sprite_of_entity.find(Handle{8})].gpu;
    KV_CHECK(c, (h.flags & kSpriteScreen) && ((h.flags >> kSpriteAnchorShift) & 15) == (1u | 2u << 2)); // x low, y high

    // Removal: the count goes, the slot is dirty and is staged as a dead row.
    u32 gone = *slot;
    dirty_rows(w);
    apply(w, remove(7));
    KV_CHECK(c, w.sprites.live == 1 && total(w.bucket_counts) == 1 && !w.sprite_of_entity.find(Handle{7}));
    GpuSprite staged[2];
    memset(staged, 0xff, sizeof staged);
    rw_stage_sprites(w, 0, 2, staged);
    KV_CHECK(c, !(staged[gone].flags & kSpriteAlive) && (staged[1 - gone].flags & kSpriteAlive));
    KV_CHECK(c, dirty_rows(w) == 1);
    apply(w, remove(7)); // twice is nothing
    KV_CHECK(c, total(w.bucket_counts) == 1);

    // Layer offsets: current at the Layer delta, previous at the next tick, both at a snap.
    SpriteDelta l{};
    l.kind = SpriteDeltaKind::Layer;
    l.state.layer = 5;
    l.state.placement.position = point2{3, 4};
    apply(w, l);
    KV_CHECK(c, w.layer_cur[5 + 128][0] == 3 && w.layer_prev[5 + 128][0] == 0);
    apply(w, tick());
    KV_CHECK(c, w.layer_prev[5 + 128][0] == 3 && w.layer_prev[5 + 128][1] == 4);
    l.state.placement.position = point2{9, 9};
    l.state.snap = true;
    apply(w, l);
    KV_CHECK(c, w.layer_prev[5 + 128][0] == 9 && w.layer_cur[5 + 128][1] == 9);
}

void test_world_materials(TestCtx& c) {
    c.begin("world_materials");
    World t;
    RenderWorld& w = t.w;
    apply(w, tick());
    apply(w, set(1, 0, 0, 0, t.add));
    apply(w, set(2, 0, 0, 0, t.add, SpriteSort::Feet));
    apply(w, set(3, 0, 0, 0, t.plain));
    dirty_rows(w);
    u32 alpha = rw_bucket_of(0, (u32)BlendMode::Alpha), additive = rw_bucket_of(0, (u32)BlendMode::Additive);
    KV_CHECK(c, w.bucket_counts[additive] == 2 && w.keyed_counts[additive] == 1 && w.bucket_counts[alpha] == 1);

    // The material's slot changes: its sprites change bucket and are uploaded again.
    w.materials.get(t.add)->slot = 5;
    rw_rebind_material(w, t.add.index());
    u32 game = rw_bucket_of(0, 5);
    KV_CHECK(c, w.bucket_counts[additive] == 0 && w.bucket_counts[game] == 2 && w.keyed_counts[game] == 1 && w.keyed_counts[additive] == 0);
    u32 runs = 0;
    KV_CHECK(c, dirty_rows(w, &runs) == 2 && runs == 1);

    // The row as the GPU holds it; a dead row is the default's.
    MaterialRecord* m = w.materials.get(t.add);
    m->tint = color{0.5f, 0.25f, 1.0f, 1.0f};
    m->params[3] = 7.0f;
    m->filter = TextureFilter::Linear;
    GpuMaterial row;
    rw_material_row(rw_material_at(w, 1), 42, row);
    KV_CHECK(c, row.texture == 42 && row.blend == (u32)BlendMode::Additive && row.slot == 5 && row.filter == 1 && row.tint[1] == 0.25f && row.params[3] == 7.0f);

    // The material goes: its sprites fall back to row 0 and the default's bucket.
    w.materials.destroy(t.add);
    rw_rebind_material(w, 1);
    KV_CHECK(c, w.bucket_counts[game] == 0 && w.bucket_counts[alpha] == 3 && w.keyed_counts[alpha] == 1);
    KV_CHECK(c, w.sprites.items[*w.sprite_of_entity.find(Handle{1})].gpu.material == 0);
    KV_CHECK(c, &rw_material_at(w, 1) == &w.materials.items[0]);

    rw_mark_material_dirty(w, 70);
    u32 a0 = 0, b0 = 0;
    rw_dirty_runs(w.material_dirty, kMaxMaterials, 8, [&](u32 a, u32 b) { a0 = a; b0 = b; });
    KV_CHECK(c, a0 == 70 && b0 == 71);

    BlendMode mode;
    KV_CHECK(c, blend_mode_parse("additive", &mode) && mode == BlendMode::Additive && !blend_mode_parse("nope", &mode));
    KV_CHECK(c, strcmp(blend_mode_name(BlendMode::Opaque), "opaque") == 0);
}

void test_world_runs(TestCtx& c) {
    c.begin("world_runs");
    // Dirty runs: bits closer than `join` clean rows are one run; rows past `rows` are not walked.
    Array<u64> bits;
    bits.resize_fill(4, 0);
    auto mark = [&](u32 i) { bits[i >> 6] |= (u64)1 << (i & 63); };
    mark(3); mark(4); mark(10); mark(100); mark(130); mark(200);
    u32 runs[8][2], n = 0;
    u32 covered = rw_dirty_runs(bits, 180, 8, [&](u32 a, u32 b) { runs[n][0] = a; runs[n][1] = b; ++n; });
    KV_CHECK(c, n == 3 && runs[0][0] == 3 && runs[0][1] == 11 && runs[1][0] == 100 && runs[1][1] == 101 && runs[2][0] == 130 && runs[2][1] == 131);
    KV_CHECK(c, covered == 8 + 1 + 1);
    bits.free();

    // Draw runs: buckets of one slot join across empty ones until another slot intervenes.
    static u32 counts[kBucketCount];
    memset(counts, 0, sizeof counts);
    counts[1] = 5;                  // layer -128, alpha
    counts[kSlotCount * 2 + 1] = 2; // layer -126, alpha: the same run
    counts[kSlotCount * 3 + 2] = 1; // layer -125, additive: a new run
    counts[kSlotCount * 4 + 1] = 4; // alpha again: a third
    u32 cursor = 0;
    DrawRun run;
    KV_CHECK(c, rw_next_run(counts, &cursor, kBucketCount, &run) && run.slot == 1 && run.first == 1 && run.last == kSlotCount * 2 + 1);
    char label[64];
    rw_run_label(label, sizeof label, run, "alpha");
    KV_CHECK(c, strcmp(label, "layers -128..-126 alpha") == 0);
    KV_CHECK(c, rw_next_run(counts, &cursor, kBucketCount, &run) && run.slot == 2 && run.first == run.last);
    KV_CHECK(c, rw_next_run(counts, &cursor, kBucketCount, &run) && run.slot == 1 && run.first == kSlotCount * 4 + 1);
    KV_CHECK(c, !rw_next_run(counts, &cursor, kBucketCount, &run) && cursor == kBucketCount);
    // A split (the post layer): the run stops at the end it is given.
    cursor = 0;
    KV_CHECK(c, rw_next_run(counts, &cursor, kSlotCount * 2, &run) && run.last == 1 && !rw_next_run(counts, &cursor, kSlotCount * 2, &run));
}

void test_world_push(TestCtx& c) {
    c.begin("world_push");
    World t;
    apply(t.w, tick());
    apply(t.w, set(1, 0, 0, 0, t.plain));
    RenderView view;
    view.alpha = 0.25f;
    view.view = rect2{-8, -4.5f, 16, 9};
    PushData down = rw_push_block(t.w, view, 1280, 720, 3.5f, false);
    PushData up   = rw_push_block(t.w, view, 1280, 720, 3.5f, true);
    KV_CHECK(c, down.alpha == 0.25f && down.tick == 1 && down.row_count == 1 && down.time == 3.5f && down.key_bucket == 0);
    KV_CHECK(c, down.cull[0] == -8 && down.cull[1] == -4.5f && down.cull[2] == 16 && down.cull[3] == 9);
    KV_CHECK(c, down.screen_cull[0] == -640 && down.screen_cull[3] == 720);
    // y up is y down with the y row negated, and nothing else.
    KV_CHECK(c, up.view[0] == down.view[0] && up.view[2] == down.view[2] && up.view[1] == -down.view[1] && up.view[3] == -down.view[3]);
    KV_CHECK(c, up.view_trans[0] == down.view_trans[0] && up.view_trans[1] == -down.view_trans[1]);
    KV_CHECK(c, up.screen[3] == -down.screen[3] && up.screen[0] == down.screen[0] && up.screen_trans[1] == -down.screen_trans[1]);
    KV_CHECK(c, memcmp(up.cull, down.cull, sizeof up.cull) == 0);
    // The right edge of the view maps to clip x = 1, the window's right edge likewise.
    f32 x = down.view[0] * 8 + down.view[2] * 0 + down.view_trans[0];
    f32 sx = down.screen[0] * 640 + down.screen_trans[0];
    KV_CHECK(c, x > 0.9999f && x < 1.0001f && sx > 0.9999f && sx < 1.0001f);
    // A turned camera culls by the turned view's bounding box.
    view.rotation = 1.5707963f;
    PushData turned = rw_push_block(t.w, view, 1280, 720, 0, false);
    KV_CHECK(c, turned.cull[2] > 8.99f && turned.cull[2] < 9.01f && turned.cull[3] > 15.99f && turned.cull[3] < 16.01f);
}

// A sort the way the GPU would write it, built on the CPU: the verifier accepts it and
// refuses each way of getting it wrong.
void test_world_sort_check(TestCtx& c) {
    c.begin("world_sort_check");
    World t;
    RenderWorld& w = t.w;
    apply(w, tick());
    apply(w, set(1, 0, 0, 0, t.plain));                       // row 0
    apply(w, set(2, 0, 0, 0, t.plain));                       // row 1
    apply(w, set(3, 0, 0, 2, t.plain, SpriteSort::Key, 5.0f)); // row 2
    apply(w, set(4, 0, 0, 2, t.plain, SpriteSort::Key, 1.0f)); // row 3: sorts ahead of row 2
    apply(w, set(5, 0, 8, 2, t.plain, SpriteSort::Feet, 0.0f)); // row 4: key -8, ahead of both
    const u32 rows = w.sprites.len;
    KV_CHECK(c, rows == 5);
    f32 keys[5];
    rw_snapshot_keys(w, 1.0f, keys);
    KV_CHECK(c, keys[0] == 0 && keys[2] == 5 && keys[3] == 1 && keys[4] == -8);

    const u32 words = rows * 2 + 2 * kBucketCount + 4;
    u32* rb = (u32*)alloc_in(mem_general(), sizeof(u32) * words);
    u32 b0 = rw_bucket_of(0, 1), b2 = rw_bucket_of(2, 1);
    auto build = [&]() {
        memset(rb, 0, sizeof(u32) * words);
        u32* sorted = rb; u32* bucket_of = rb + rows; u32* totals = bucket_of + rows; u32* starts = totals + kBucketCount;
        sorted[0] = 0; sorted[1] = 1; sorted[2] = 4; sorted[3] = 3; sorted[4] = 2;
        bucket_of[0] = bucket_of[1] = b0; bucket_of[2] = bucket_of[3] = bucket_of[4] = b2;
        totals[b0] = 2; totals[b2] = 3;
        for (u32 b = 0, at = 0; b < kBucketCount; ++b) { starts[b] = at; at += totals[b]; }
        totals[2 * kBucketCount] = 5; // the visible count follows the starts
    };
    build();
    KV_CHECK(c, rw_verify_sort(w, rb, rows, keys, "test"));

    LogLevel was = log_level();
    log_set_level(LogLevel::Off);   // the refusals below log errors by design
    build(); rb[3] = 2; rb[4] = 3;                 // key order wrong
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    build(); rb[0] = 1; rb[1] = 0;                 // row order wrong in an unkeyed bucket
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    build(); rb[1] = 0;                            // a row twice, one missing
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    build(); rb[rows + 1] = b2;                    // a row in a bucket's run that is not its bucket
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    build(); rb[rows * 2 + kBucketCount + b2] = 3; // a bucket's start off
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    build(); rb[rows * 2 + 2 * kBucketCount] = 4;  // the GPU's visible count off
    KV_CHECK(c, !rw_verify_sort(w, rb, rows, keys, "test"));
    log_set_level(was);

    // A culled row is in no bucket and is not listed.
    build();
    rb[rows + 1] = 0xFFFFFFFFu; rb[rows * 2 + b0] = 1; rb[rows * 2 + 2 * kBucketCount] = 4;
    rb[1] = 4; rb[2] = 3; rb[3] = 2;
    for (u32 b = 0, at = 0; b < kBucketCount; ++b) { rb[rows * 2 + kBucketCount + b] = at; at += rb[rows * 2 + b]; }
    KV_CHECK(c, rw_verify_sort(w, rb, rows, keys, "test"));
    free_in(mem_general(), rb, sizeof(u32) * words);
}
