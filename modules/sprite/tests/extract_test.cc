#include "korvid/sprite/extract.h"
#include "korvid/sprite/module.h"
#include "korvid/sprite/world.h"
#include "korvid/core/engine.h"
#include "korvid/core/test.h"

namespace {
using namespace kv;
// A still kind: rows that never change once spawned (a map's tiles).
constinit const KindDesc k_still_kinds[] = {{.name = ".tile", .components = "Transform2, Sprite", .capacity = 64, .still = true}};
KV_MODULE_DEFINE(k_still) = {.name = "still", .kinds = k_still_kinds};
} // namespace

static kv::u32 count_kind(const kv::Array<kv::SpriteDelta>& d, kv::SpriteDeltaKind k) {
    kv::u32 n = 0;
    for (const kv::SpriteDelta& x : d) n += x.kind == k;
    return n;
}

void test_extract(kv::TestCtx& c) {
    using namespace kv;
    c.begin("extract");

    const ModuleDesc* const list[] = {&sprite_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    SpriteWorld& w = *sprite_world(e);
    Table& t = *w.table;
    ExtractState st;

    Sprite s0; s0.layer = 1; s0.tint = color_red(); s0.size = vec2{32, 16};
    Sprite s1; s1.layer = -1; s1.uv = rect2{0.5f, 0, 0.5f, 1};
    Handle a = w.create(transform2_at(point2{0, 0}), s0);
    Handle b = w.create(transform2_at(point2{5, 5}, 1.0f), s1);
    Handle d = w.create(transform2_at(point2{9, 9}), s0);
    w.destroy(b); // the renderer never heard of b: no Remove for it

    // First extract: a Tick, then a Set for every live entity the renderer has not seen.
    Array<SpriteDelta> out;
    t.begin_tick();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 3 && out[0].kind == SpriteDeltaKind::Tick);
    KV_CHECK(c, out[1].kind == SpriteDeltaKind::Set && out[1].entity == a);
    KV_CHECK(c, out[2].kind == SpriteDeltaKind::Set && out[2].entity == d);
    KV_CHECK(c, out[1].state.layer == 1 && out[1].state.tint == color_red() && out[1].state.size == vec2{32, 16});
    KV_CHECK(c, out[1].state.placement.position == point2{0, 0} && out[1].state.material.is_null());
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Remove) == 0);

    // Nothing changed: a Tick and nothing else.
    out.clear();
    t.begin_tick();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 1 && out[0].kind == SpriteDeltaKind::Tick);

    // One moved, one changed its sprite: exactly those two, with the new state.
    out.clear();
    t.begin_tick();
    w.transform(a)->position = point2{2, 0};
    w.sprite(d)->tint = color_blue();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 3 && count_kind(out, SpriteDeltaKind::Set) == 2);
    KV_CHECK(c, out[1].entity == a && out[1].state.placement.position == point2{2, 0});
    KV_CHECK(c, out[2].entity == d && out[2].state.tint == color_blue());

    // Destroying a known entity is a Remove; a new entity in the reused slot is a Set
    // with its own handle, so the renderer never confuses the two.
    out.clear();
    t.begin_tick();
    w.destroy(a);
    Handle f = w.create(transform2_at(point2{1, 1}), s1);
    KV_CHECK(c, f.index() == b.index() && f != b); // FIFO: b's slot came back first
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 3);
    KV_CHECK(c, out[1].kind == SpriteDeltaKind::Remove && out[1].entity == a);
    KV_CHECK(c, out[2].kind == SpriteDeltaKind::Set && out[2].entity == f && out[2].state.layer == -1);

    // Destroyed and replaced in the same tick: the old handle is removed, the new one set.
    out.clear();
    t.begin_tick();
    w.destroy(f);
    Handle g = w.create(transform2_at(point2{4, 4}), s0);
    KV_CHECK(c, g.index() == a.index()); // FIFO again: a's slot was freed before f's
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 3 && out[1].kind == SpriteDeltaKind::Remove && out[1].entity == f);
    KV_CHECK(c, out[2].kind == SpriteDeltaKind::Set && out[2].entity == g);

    // A move is a Set without snap; a teleport (the previous copy written too, so it
    // equals the new transform) is a Set with snap; a transform the renderer already has
    // is nothing, whatever the previous copy says.
    out.clear();
    t.begin_tick();
    w.transform(g)->position = point2{40, 4};
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 2 && out[1].entity == g && !out[1].state.snap);
    out.clear();
    t.begin_tick();
    w.transform(g)->position = point2{-40, 4};
    *t.prev<Transform2>(g) = *w.transform(g);
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 2 && out[1].entity == g && out[1].state.snap && out[1].state.placement.position == point2{-40, 4});
    out.clear();
    t.begin_tick();
    *t.prev<Transform2>(g) = transform2_at(point2{7, 7}); // a stale previous copy alone changes nothing
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 1);

    // Deltas accumulate across ticks until the caller clears: two ticks, two markers.
    out.clear();
    t.begin_tick(); w.transform(g)->position = point2{2, 2}; extract_sprites(e, st, nullptr, out);
    t.begin_tick(); w.transform(g)->position = point2{3, 3}; extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 4 && count_kind(out, SpriteDeltaKind::Tick) == 2 && out[3].state.placement.position == point2{3, 3});

    // Through the schedule: a tick runs PreSim's copy and PostSim's extract on the module's
    // own state, whose record of the renderer is separate from `st` and still empty, so
    // both live entities are Sets, in slot order, with the state as of the tick.
    w.sprite(g)->layer = 5;
    sprite_deltas(e)->clear();
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    Array<SpriteDelta>& deltas = *sprite_deltas(e);
    KV_CHECK(c, deltas.len == 3 && deltas[1].kind == SpriteDeltaKind::Set && deltas[1].entity == g && deltas[1].state.layer == 5);
    KV_CHECK(c, deltas[2].entity == d);

    // Layer scroll: a layer's offset goes out once when it changes, after the Tick marker;
    // a snap goes out even unchanged, and is cleared once sent.
    SpriteLayers layers{};
    layers.set(-14, vec2{0, -40});
    out.clear();
    extract_sprites(e, st, &layers, out);
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Layer) == 1 && out[1].kind == SpriteDeltaKind::Layer && out[1].state.layer == -14 &&
                out[1].state.placement.position == point2{0, -40} && !out[1].state.snap);
    out.clear();
    extract_sprites(e, st, &layers, out);
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Layer) == 0);
    layers.set(-14, vec2{0, -40}, true);
    layers.set(300, vec2{1, 1}); // past the last layer: ignored
    out.clear();
    extract_sprites(e, st, &layers, out);
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Layer) == 1 && out[1].state.snap && layers.snap[-14 - KV_SPRITE_LAYER_MIN] == 0);
    KV_CHECK(c, layers.get(300) == vec2{0, 0} && sprite_layers(e) != nullptr);

    out.free();
    st.free();
    engine_shutdown(e);
}

void test_extract_still(kv::TestCtx& c) {
    using namespace kv;
    c.begin("extract_still");

    const ModuleDesc* const list[] = {&sprite_module, &k_still};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    Table& t = *engine_table(e, "still.tile");
    ExtractState st;
    Array<SpriteDelta> out;
    Sprite s0; s0.layer = -16;
    Handle a = t.spawn(transform2_at(point2{0, 0}), s0);
    Handle b = t.spawn(transform2_at(point2{16, 0}), s0);

    // Sent once, as they are born.
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, count_kind(out, SpriteDeltaKind::Set) == 2);

    // A still kind's rows are not looked at again: a change to a living row is not seen
    // (the contract), and with nothing born or dead the table is skipped whole.
    t.get<Transform2>(a)->position = point2{99, 99};
    out.clear();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 1 && out[0].kind == SpriteDeltaKind::Tick);

    // A birth and a death: exactly those, never the unchanged row.
    u64 churn = t.churn;
    t.destroy(b);
    Handle d = t.spawn(transform2_at(point2{32, 0}), s0);
    KV_CHECK(c, t.churn == churn + 2);
    out.clear();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 3 && count_kind(out, SpriteDeltaKind::Remove) == 1 && count_kind(out, SpriteDeltaKind::Set) == 1);
    KV_CHECK(c, out[1].entity == b && out[2].entity == d);

    // Its previous copies are its values from the spawn on: begin_tick has nothing to copy.
    t.begin_tick();
    KV_CHECK(c, t.prev<Transform2>(d)->position == point2{32, 0});

    // A restore (a save loaded) gives back the same handles with values that may differ:
    // the next extract compares every row once, as for any kind.
    u32 gens[8], unused[8], nf = 0, n = t.rows();
    for (u32 i = 0; i < n; ++i) gens[i] = t.slots.gens[i];
    t.slots.each_free([&](u32 i) { unused[nf++] = i; });
    KV_CHECK(c, t.restore_slots(Span<const u32>(gens, n), Span<const u32>(unused, nf)) && t.valid(a) && t.valid(d));
    *t.get<Transform2>(a) = transform2_at(point2{7, 7});
    *t.get<Transform2>(d) = transform2_at(point2{32, 0});
    *t.get<Sprite>(a) = s0;
    *t.get<Sprite>(d) = s0;
    out.clear();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 2 && out[1].kind == SpriteDeltaKind::Set && out[1].entity == a && out[1].state.placement.position == point2{7, 7});
    out.clear();
    extract_sprites(e, st, nullptr, out);
    KV_CHECK(c, out.len == 1); // and back to skipping

    out.free();
    st.free();
    engine_shutdown(e);
}
