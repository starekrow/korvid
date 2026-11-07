// Kinds: the engine builds a table per declared kind, registers its resources and handle
// kind, zeroes spawned rows, keeps previous copies, rejects bad declarations, and tears
// everything down at shutdown.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos { f32 x, y; };
struct Vel { f32 dx, dy; };
struct Tag { u8 team; Handle owner; };

constinit const FieldDesc k_pos_fields[] = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const FieldDesc k_vel_fields[] = {KV_FIELD(Vel, dx), KV_FIELD(Vel, dy)};
constinit const FieldDesc k_tag_fields[] = {KV_FIELD(Tag, team), KV_FIELD(Tag, owner)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev),
    KV_COMPONENT(Vel, k_vel_fields),
    KV_COMPONENT(Tag, k_tag_fields),
};

constinit u32 g_mover_id = 0;
constinit const char* const k_mover_components[] = {"Pos", "Vel"};
constinit const char* const k_marker_components[] = {"Pos", "Tag"};
constinit const KindDesc k_kinds[] = {
    {.name = "t.mover",  .components = k_mover_components,  .capacity = 3000, .id = &g_mover_id},
    {.name = "t.marker", .components = k_marker_components, .capacity = 2},
};

constinit const char* const k_move_reads[]  = {"*.Vel"};
constinit const char* const k_move_writes[] = {"*.Pos", "t.mover"};
void sys_move(TickContext&) {}
constinit const SystemDesc k_systems[] = {
    {.name = "t.move", .phase = Phase::Sim, .fn = sys_move, .reads = k_move_reads, .writes = k_move_writes},
};
KV_MODULE_DEFINE(k_t) = {.name = "t", .components = k_components, .kinds = k_kinds, .systems = k_systems};

// Rejections.
constinit const char* const k_unknown_components[] = {"Pos", "Nope"};
constinit const KindDesc k_unknown_kinds[] = {{.name = "u.k", .components = k_unknown_components, .capacity = 8}};
KV_MODULE_DEFINE(k_unknown) = {.name = "u", .kinds = k_unknown_kinds};

constinit const KindDesc k_dup_kinds[] = {{.name = "t.mover", .components = k_mover_components, .capacity = 8}};
KV_MODULE_DEFINE(k_dup) = {.name = "d", .kinds = k_dup_kinds};

constinit const char* const k_twice_components[] = {"Pos", "Pos"};
constinit const KindDesc k_twice_kinds[] = {{.name = "w.k", .components = k_twice_components, .capacity = 8}};
KV_MODULE_DEFINE(k_twice) = {.name = "w", .kinds = k_twice_kinds};

constinit const char* const k_nowhere_reads[] = {"*.Tag", "*.Nope"};
constinit const SystemDesc k_nowhere_systems[] = {{.name = "n.sys", .phase = Phase::Sim, .fn = sys_move, .reads = k_nowhere_reads}};
KV_MODULE_DEFINE(k_nowhere) = {.name = "n", .systems = k_nowhere_systems};

} // namespace

void test_kind(kv::TestCtx& c) {
    using namespace kv;
    c.begin("kind");

    const ModuleDesc* const list[] = {&k_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // Tables, ids and resources in declaration order; wildcard references accepted.
    KV_CHECK(c, e.tables.len == 2 && g_mover_id == 1 && engine_kind_id(e, "t.marker") == 2);
    Table* mover  = engine_table(e, "t.mover");
    Table* marker = engine_table(e, g_mover_id + 1);
    KV_CHECK(c, mover && marker && mover->index == 0 && engine_table(e, "t.nope") == nullptr && engine_table(e, 3u) == nullptr);
    KV_CHECK(c, engine_resource_id(e, "t.mover") == mover->resource);
    KV_CHECK(c, engine_resource_id(e, "t.mover.Pos") == mover->columns[0].resource);
    KV_CHECK(c, engine_resource_id(e, "t.mover.Vel") == mover->columns[1].resource);
    KV_CHECK(c, engine_resource_id(e, "t.marker.Tag") != 0 && engine_resource_id(e, "t.marker.Vel") == 0);
    KV_CHECK(c, mover->has(ComponentType<Pos>::id) && mover->has(ComponentType<Vel>::id) && !mover->has(ComponentType<Tag>::id));
    KV_CHECK(c, mover->column<Pos>()->prev != nullptr && mover->column<Vel>()->prev == nullptr);
    KV_CHECK(c, mover->kind != 0 && marker->kind != 0 && mover->kind != marker->kind);
    KV_CHECK(c, strcmp(kv_handle_kind_name(mover->kind), "t.mover") == 0);

    // Spawn: zero rows, or the components handed in; prev starts equal to current.
    Handle a = mover->spawn();
    Handle b = mover->spawn(Pos{1, 2}, Vel{3, 4});
    KV_CHECK(c, mover->live() == 2 && mover->rows() == 2 && a.kind() == mover->kind);
    KV_CHECK(c, mover->get<Pos>(a)->x == 0 && mover->get<Vel>(a)->dy == 0);
    KV_CHECK(c, mover->get<Pos>(b)->y == 2 && mover->get<Vel>(b)->dx == 3);
    KV_CHECK(c, mover->prev<Pos>(b)->y == 2 && mover->prev<Vel>(b) == nullptr);
    KV_CHECK(c, mover->get<Tag>(b) == nullptr); // not a component of this kind
    KV_CHECK(c, marker->get<Pos>(b) == nullptr); // wrong table: handle kind mismatch

    // begin_tick copies current into previous, for prev columns only.
    mover->get<Pos>(b)->x = 10;
    mover->begin_tick();
    mover->get<Pos>(b)->x = 20;
    KV_CHECK(c, mover->prev<Pos>(b)->x == 10 && mover->get<Pos>(b)->x == 20);
    // The tick runner does it at the top of PreSim.
    mover->get<Pos>(b)->x = 30;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, mover->prev<Pos>(b)->x == 30);

    // Columns as spans, by slot.
    Span<Pos> ps = mover->rows_of<Pos>();
    KV_CHECK(c, ps.len == 2 && &ps[1] == mover->get<Pos>(b) && mover->rows_of<Tag>().len == 0);

    // Resolve and describe through the handle registry.
    KV_CHECK(c, resolve(b) == mover->get<Pos>(b) && resolve<Pos>(b) == mover->get<Pos>(b) && resolve<Vel>(b) == nullptr);
    char text[256];
    kv_handle_describe(b.bits, text, sizeof text);
    KV_CHECK(c, strcmp(text, "t.mover#1:1 Pos{x=30, y=2} Vel{dx=3, dy=4}") == 0);

    // Destroy: stale handles get null; the slot is reused with a new generation.
    KV_CHECK(c, mover->destroy(a) && !mover->destroy(a) && mover->live() == 1);
    KV_CHECK(c, mover->get<Pos>(a) == nullptr && resolve(a) == nullptr);
    Handle d = mover->spawn(Pos{5, 5}, Vel{});
    KV_CHECK(c, d.index() == a.index() && d.gen() != a.gen() && mover->get<Pos>(d)->x == 5 && mover->prev<Pos>(d)->x == 5);
    kv_handle_describe(a.bits, text, sizeof text);
    KV_CHECK(c, strstr(text, "stale") != nullptr);

    // Capacity is a budget: the third marker fails, nothing grows.
    Handle m0 = marker->spawn(Pos{}, Tag{1, b});
    Handle m1 = marker->spawn();
    KV_CHECK(c, !m0.is_null() && !m1.is_null() && marker->spawn().is_null() && marker->live() == 2);
    KV_CHECK(c, marker->get<Tag>(m0)->owner == b);

    // Past the first commit step: rows beyond 1024 land on freshly committed pages.
    for (u32 i = 0; i < 2000; ++i) KV_CHECK(c, !mover->spawn(Pos{(f32)i, 0}, Vel{}).is_null());
    KV_CHECK(c, mover->live() == 2002 && mover->rows_of<Pos>()[1500].x == 1498);
#if KV_DEV
    KV_CHECK(c, mem_stats_module(1).committed_bytes > 0);
#endif

    // Shutdown frees the tables, unregisters the handle kinds and clears the id slot.
    u16 kind = mover->kind;
    engine_shutdown(e);
    KV_CHECK(c, e.tables.len == 0 && g_mover_id == 0 && !kv_handle_kind_is_live(kind));
#if KV_DEV
    KV_CHECK(c, mem_stats_module(1).committed_bytes == 0);
#endif

    // Rejections: unknown component, duplicate kind, a component twice, a wildcard nobody has.
    const ModuleDesc* const unknown[] = {&k_t, &k_unknown};
    const ModuleDesc* const dup[]     = {&k_t, &k_dup};
    const ModuleDesc* const twice[]   = {&k_t, &k_twice};
    const ModuleDesc* const nowhere[] = {&k_t, &k_nowhere};
    const Span<const ModuleDesc* const> rejected[] = {unknown, dup, twice, nowhere};
    for (Span<const ModuleDesc* const> l : rejected) {
        Engine f;
        KV_CHECK(c, !engine_init(f, l));
        KV_CHECK(c, f.tables.len == 0 && g_mover_id == 0);
    }
}
