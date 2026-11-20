// KTables: the engine builds one per declaration, registers its resources and handle
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
constinit const KTableDesc k_ktables[] = {
    {.name = "t.mover",  .components = "Pos, Vel",  .capacity = 3000, .id = &g_mover_id},
    {.name = "t.marker", .components = "Pos, Tag", .capacity = 2},
};

void sys_move(TickContext&) {}
constinit const SystemDesc k_systems[] = {
    {.name = "t.move", .phase = Phase::Sim, .fn = sys_move, .reads = "*.Vel", .writes = "*.Pos, t.mover"},
};
KV_MODULE_DEFINE(k_t) = {.name = "t", .components = k_components, .ktables = k_ktables, .systems = k_systems};

// Rejections.
constinit const KTableDesc k_unknown_ktables[] = {{.name = "u.k", .components = "Pos, Nope", .capacity = 8}};
KV_MODULE_DEFINE(k_unknown) = {.name = "u", .ktables = k_unknown_ktables};

constinit const KTableDesc k_dup_ktables[] = {{.name = "t.mover", .components = "Pos, Vel", .capacity = 8}};
KV_MODULE_DEFINE(k_dup) = {.name = "d", .ktables = k_dup_ktables};

constinit const KTableDesc k_twice_ktables[] = {{.name = "w.k", .components = "Pos, Pos", .capacity = 8}};
KV_MODULE_DEFINE(k_twice) = {.name = "w", .ktables = k_twice_ktables};

constinit const SystemDesc k_nowhere_systems[] = {{.name = "n.sys", .phase = Phase::Sim, .fn = sys_move, .reads = "*.Tag, *.Nope"}};
KV_MODULE_DEFINE(k_nowhere) = {.name = "n", .systems = k_nowhere_systems};

} // namespace

void test_ktable(kv::TestCtx& c) {
    using namespace kv;
    c.begin("ktable");

    const ModuleDesc* const list[] = {&k_t};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // Tables, ids and resources in declaration order; wildcard references accepted.
    KV_CHECK(c, e.ktables.len == 2 && g_mover_id == 1 && engine_ktable_id(e, "t.marker") == 2);
    KTable* mover  = engine_ktable(e, "t.mover");
    KTable* marker = engine_ktable(e, g_mover_id + 1);
    KV_CHECK(c, mover && marker && mover->index == 0 && engine_ktable(e, "t.nope") == nullptr && engine_ktable(e, 3u) == nullptr);
    KV_CHECK(c, engine_resource_id(e, "t.mover") == mover->resource);
    KV_CHECK(c, engine_resource_id(e, "t.mover.Pos") == mover->columns[0].resource);
    KV_CHECK(c, engine_resource_id(e, "t.mover.Vel") == mover->columns[1].resource);
    KV_CHECK(c, engine_resource_id(e, "t.marker.Tag") != 0 && engine_resource_id(e, "t.marker.Vel") == 0);
    KV_CHECK(c, mover->has(ComponentType<Pos>::id) && mover->has(ComponentType<Vel>::id) && !mover->has(ComponentType<Tag>::id));
    KV_CHECK(c, mover->column<Pos>()->prev != nullptr && mover->column<Vel>()->prev == nullptr);
    KV_CHECK(c, mover->handle_kind != 0 && marker->handle_kind != 0 && mover->handle_kind != marker->handle_kind);
    KV_CHECK(c, strcmp(kv_handle_kind_name(mover->handle_kind), "t.mover") == 0);

    // Spawn: zero rows, or the components handed in; prev starts equal to current.
    Handle a = mover->spawn();
    Handle b = mover->spawn(Pos{1, 2}, Vel{3, 4});
    KV_CHECK(c, mover->live() == 2 && mover->rows() == 2 && a.kind() == mover->handle_kind);
    KV_CHECK(c, mover->get<Pos>(a)->x == 0 && mover->get<Vel>(a)->dy == 0);
    KV_CHECK(c, mover->get<Pos>(b)->y == 2 && mover->get<Vel>(b)->dx == 3);
    KV_CHECK(c, mover->prev<Pos>(b)->y == 2 && mover->prev<Vel>(b) == nullptr);
    KV_CHECK(c, mover->get<Tag>(b) == nullptr); // not a component of this ktable
    KV_CHECK(c, marker->get<Pos>(b) == nullptr); // wrong ktable: handle kind mismatch

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
    // Truncated at every length, the text is terminated inside the buffer and is a prefix of
    // the whole. (The separator between two components once took the last byte, leaving no
    // terminator: a cap of 27 here, which ends "y=2} " and ran on into whatever followed.)
    {
        const char* whole = "t.mover#1:1 Pos{x=30, y=2} Vel{dx=3, dy=4}";
        usize len = strlen(whole);
        bool ok = true;
        for (usize cap = 1; cap <= len + 1; ++cap) {
            char small[64];
            memset(small, 0xF9, sizeof small);
            kv_handle_describe(b.bits, small, cap);
            usize n = strnlen(small, cap);
            if (n >= cap || strncmp(small, whole, n) != 0) ok = false;
        }
        KV_CHECK(c, ok);
    }

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

    // Shutdown frees the ktables, unregisters the handle kinds and clears the id slot.
    u16 kind = mover->handle_kind;
    engine_shutdown(e);
    KV_CHECK(c, e.ktables.len == 0 && g_mover_id == 0 && !kv_handle_kind_is_live(kind));
#if KV_DEV
    KV_CHECK(c, mem_stats_module(1).committed_bytes == 0);
#endif

    // Rejections: unknown component, duplicate ktable, a component twice, a wildcard nobody has.
    const ModuleDesc* const unknown[] = {&k_t, &k_unknown};
    const ModuleDesc* const dup[]     = {&k_t, &k_dup};
    const ModuleDesc* const twice[]   = {&k_t, &k_twice};
    const ModuleDesc* const nowhere[] = {&k_t, &k_nowhere};
    const Span<const ModuleDesc* const> rejected[] = {unknown, dup, twice, nowhere};
    for (Span<const ModuleDesc* const> l : rejected) {
        Engine f;
        KV_CHECK(c, !engine_init(f, l));
        KV_CHECK(c, f.ktables.len == 0 && g_mover_id == 0);
    }

    // The root sets a capacity over the KTableDesc's; past it a spawn is refused, and counted.
    {
        const KTableCapacity caps[] = {{"t.marker", 5}, {"t.nothing", 9}};
        EngineConfig cfg;
        cfg.capacities = caps;
        Engine f;
        KV_CHECK(c, engine_init(f, list, cfg));
        KTable* m = engine_ktable(f, "t.marker");
        KV_CHECK(c, m && m->capacity == 5 && engine_ktable(f, "t.mover")->capacity == 3000);
        for (u32 i = 0; i < 7; ++i) m->spawn(Pos{}, Tag{});
        KV_CHECK(c, m->live() == 5 && m->refused == 2);
        engine_shutdown(f);
    }

}
