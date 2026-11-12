// The derived schedule: readers after writers whatever the declaration order, `after`
// and `before` edges, kind writes covering their columns and attachments, early reads
// both ways (ordered, and from a snapshot kind), one mode per resource, module state as
// a resource, append resources, units numbered by rank, the rejections (unordered double
// writers, cycles, contradictions) with names in the message, the sim-to-cosmetic flow
// rule, and the state hash: same operations, same hash; dead rows and padding invisible.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos  { f32 x, y; };
struct Vel  { f32 dx, dy; };
struct Glow { f32 a; };

constinit const FieldDesc k_pos_fields[]  = {KV_FIELD(Pos, x), KV_FIELD(Pos, y)};
constinit const FieldDesc k_vel_fields[]  = {KV_FIELD(Vel, dx), KV_FIELD(Vel, dy)};
constinit const FieldDesc k_glow_fields[] = {KV_FIELD(Glow, a)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Vel, k_vel_fields), KV_COMPONENT(Glow, k_glow_fields, CompCosmetic),
};
constinit const KindDesc k_kinds[] = {{.name = "s.body", .components = "Pos, Vel, Glow", .capacity = 64}};

char g_trace[256];
void trace(const char* s) { strncat(g_trace, s, sizeof g_trace - strlen(g_trace) - 1); }
void sys_wrap(TickContext&)   { trace("wrap "); }
void sys_move(TickContext&)   { trace("move "); }
void sys_steer(TickContext&)  { trace("steer "); }
void sys_glow(TickContext&)   { trace("glow "); }
void sys_spawn(TickContext&)  { trace("spawn "); }
void sys_other(TickContext&)  { trace("other "); }


// Declared backwards on purpose: wrap (reads Pos) before move (writes Pos, reads Vel)
// before steer (writes Vel). The schedule must come out steer, move, wrap. glow is a
// cosmetic system reading Pos: after move, and free to sit anywhere else.
constinit const SystemDesc k_a_systems[] = {
    {.name = "s.wrap",  .phase = Phase::Sim, .fn = sys_wrap,  .reads = "*.Pos", .writes = "*.Pos", .after = "s.move"},
    {.name = "s.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "s.steer", .phase = Phase::Sim, .fn = sys_steer, .writes = "s.body.Vel"},
    {.name = "s.glow",  .phase = Phase::Sim, .fn = sys_glow,  .reads = "*.Pos", .writes = "*.Glow"},
};
KV_MODULE_DEFINE(k_a) = {.name = "a", .components = k_components, .kinds = k_kinds, .systems = k_a_systems};

// A kind writer covers every column, so spawn (writes s.body) is a second writer of Vel
// and Glow and must be ordered against steer and glow by hand; move and wrap follow from
// the reads. Before move it goes by the path through steer.
constinit const SystemDesc k_b_systems[] = {
    {.name = "s.spawn", .phase = Phase::Sim, .fn = sys_spawn, .writes = "s.body", .before = "s.steer, s.glow"},
};
KV_MODULE_DEFINE(k_b) = {.name = "b", .systems = k_b_systems};

// Rejections.
constinit const SystemDesc k_double_systems[] = {{.name = "s.other", .phase = Phase::Sim, .fn = sys_other, .writes = "*.Pos"}};
KV_MODULE_DEFINE(k_double) = {.name = "double", .systems = k_double_systems};

constinit const SystemDesc k_cycle_systems[] = {
    {.name = "c.a", .phase = Phase::PostSim, .fn = sys_other, .reads = "*.Vel", .writes = "*.Glow"},
    {.name = "c.b", .phase = Phase::PostSim, .fn = sys_other, .reads = "*.Glow", .writes = "s.body.Vel"},
};
KV_MODULE_DEFINE(k_cycle) = {.name = "cycle", .components = k_components, .kinds = k_kinds, .systems = k_cycle_systems};

// Reads cosmetic Glow and writes sim Vel: the flow rule.
constinit const SystemDesc k_flow_systems[] = {{.name = "f.back", .phase = Phase::Sim, .fn = sys_other, .reads = "*.Glow", .writes = "s.body.Vel"}};
KV_MODULE_DEFINE(k_flow) = {.name = "flow", .components = k_components, .kinds = k_kinds, .systems = k_flow_systems};

constinit const SystemDesc k_unknown_systems[] = {{.name = "u.sys", .phase = Phase::Sim, .fn = sys_other, .after = "s.nowhere"}};
KV_MODULE_DEFINE(k_unknown) = {.name = "unknown", .systems = k_unknown_systems};

constinit const SystemDesc k_later_systems[] = {
    {.name = "s.late",  .phase = Phase::PostSim, .fn = sys_other},
    {.name = "s.early", .phase = Phase::Sim,     .fn = sys_other, .after = "s.late"},
};
KV_MODULE_DEFINE(k_later) = {.name = "later", .systems = k_later_systems};

// An early read: think reads where the bodies are (^) and writes their Vel; move reads Vel
// and writes Pos. Without the '^' it is a cycle; with it, think runs first.
constinit const SystemDesc k_early_systems[] = {
    {.name = "e.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "e.think", .phase = Phase::Sim, .fn = sys_steer, .reads = "^*.Pos", .writes = "*.Vel"},
};
KV_MODULE_DEFINE(k_early) = {.name = "early", .components = k_components, .kinds = k_kinds, .systems = k_early_systems};
constinit const SystemDesc k_late_read_systems[] = {
    {.name = "l.move",  .phase = Phase::Sim, .fn = sys_move,  .reads = "*.Vel", .writes = "*.Pos"},
    {.name = "l.think", .phase = Phase::Sim, .fn = sys_steer, .reads = "*.Pos", .writes = "*.Vel"},
};
KV_MODULE_DEFINE(k_late_read) = {.name = "lateread", .components = k_components, .kinds = k_kinds, .systems = k_late_read_systems};
constinit const SystemDesc k_early_write_systems[] = {{.name = "w.bad", .phase = Phase::Sim, .fn = sys_other, .writes = "^*.Pos"}};
KV_MODULE_DEFINE(k_early_write) = {.name = "earlywrite", .components = k_components, .kinds = k_kinds, .systems = k_early_write_systems};

// A set form with a joined filter: "*[Vel+Glow].Pos" is Pos in the kinds with both, so
// it shares nothing with a writer of a kind that has Vel only (no order needed) and
// collides with an unordered writer of one that has both.
constinit const KindDesc k_filter_kinds[] = {
    {.name = "j.plain", .components = "Pos, Vel", .capacity = 8},
    {.name = "j.glowing", .components = "Pos, Vel, Glow", .capacity = 8},
};
constinit const SystemDesc k_joined_systems[] = {
    {.name = "j.both", .phase = Phase::Sim, .fn = sys_other, .writes = "*[Vel+Glow].Pos", .state = StateUse::None},
    {.name = "j.plain_mover", .phase = Phase::Sim, .fn = sys_move, .writes = "j.plain.Pos", .state = StateUse::None},
    {.name = "j.reader", .phase = Phase::Sim, .fn = sys_steer, .reads = "j.glowing.Pos", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_joined) = {.name = "j", .components = k_components, .kinds = k_filter_kinds, .systems = k_joined_systems};
constinit const SystemDesc k_joined_clash_systems[] = {
    {.name = "j.both", .phase = Phase::Sim, .fn = sys_other, .writes = "*[Vel+Glow].Pos", .state = StateUse::None},
    {.name = "j.glow_mover", .phase = Phase::Sim, .fn = sys_move, .writes = "j.glowing.Pos", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_joined_clash) = {.name = "j", .components = k_components, .kinds = k_filter_kinds, .systems = k_joined_clash_systems};
constinit const SystemDesc k_joined_unknown_systems[] = {{.name = "j.what", .phase = Phase::Sim, .fn = sys_other, .writes = "*[Vel+Nope].Pos"}};
KV_MODULE_DEFINE(k_joined_unknown) = {.name = "j", .components = k_components, .kinds = k_filter_kinds, .systems = k_joined_unknown_systems};

// An append resource: its appenders need no order, and their records reach the owner in
// unit order; a real resource two systems also both write still does. Only the owner
// writes it; anyone else declares `appends`. Module resources number after the kinds'
// columns, so the queue comes first here by id and by list: the case a check by id order
// would pass over.
char g_applied[64];
void queue_apply(Engine&, const void* record, u32 size) { if (size == 1) strncat(g_applied, (const char*)record, 1); }
void sys_append_a(TickContext& ctx) { char r = 'a'; ctx.append("q.queue", r); }
void sys_append_b(TickContext& ctx) { char r = 'b'; ctx.append("q.queue", r); }
constinit const ResourceDesc k_queue_res[] = {{".queue", queue_apply}};
constinit const SystemDesc k_sink_ok_systems[] = {
    {.name = "q.one", .phase = Phase::Sim, .fn = sys_append_a, .appends = "q.queue", .state = StateUse::None},
    {.name = "q.two", .phase = Phase::Sim, .fn = sys_append_b, .appends = "q.queue", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_sink_ok) = {.name = "q", .components = k_components, .kinds = k_kinds, .resources = k_queue_res, .systems = k_sink_ok_systems};
constinit const SystemDesc k_sink_clash_systems[] = {
    {.name = "q.one", .phase = Phase::Sim, .fn = sys_other, .writes = "s.body.Vel", .appends = "q.queue", .state = StateUse::None},
    {.name = "q.two", .phase = Phase::Sim, .fn = sys_other, .writes = "s.body.Vel", .appends = "q.queue", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_sink_clash) = {.name = "q", .components = k_components, .kinds = k_kinds, .resources = k_queue_res, .systems = k_sink_clash_systems};
constinit const SystemDesc k_sink_writer_systems[] = {{.name = "o.writes", .phase = Phase::Sim, .fn = sys_other, .writes = "q.queue"}};
KV_MODULE_DEFINE(k_sink_writer) = {.name = "o", .systems = k_sink_writer_systems};
constinit const SystemDesc k_not_append_systems[] = {{.name = "o.appends", .phase = Phase::Sim, .fn = sys_other, .appends = "s.body.Vel"}};
KV_MODULE_DEFINE(k_not_append) = {.name = "o", .systems = k_not_append_systems};

// A reader of two things one writer writes, one of them early (^) and one as it stands:
// with the kind's early reads ordered it would have to run both before and after the
// writer, and an order declared one way contradicts one of the reads, so both are refused.
// With the kind a snapshot, the pair is fine: the reader runs after and sees Pos from the
// copy, as the phase found it, and Vel as the writer left it.
constinit const SystemDesc k_mixed_read_systems[] = {
    {.name = "m.write", .phase = Phase::Sim, .fn = sys_move, .writes = "*.Pos, *.Vel", .state = StateUse::None},
    {.name = "m.read",  .phase = Phase::Sim, .fn = sys_steer, .reads = "^*.Pos, *.Vel", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_mixed_read) = {.name = "m", .components = k_components, .kinds = k_kinds, .systems = k_mixed_read_systems};
constinit const SystemDesc k_mixed_ordered_systems[] = {
    {.name = "m.write", .phase = Phase::Sim, .fn = sys_move, .writes = "*.Pos, *.Vel", .state = StateUse::None},
    {.name = "m.read",  .phase = Phase::Sim, .fn = sys_steer, .reads = "^*.Pos, *.Vel", .before = "m.write", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_mixed_ordered) = {.name = "m", .components = k_components, .kinds = k_kinds, .systems = k_mixed_ordered_systems};
// Within one module, sharing its state: refused all the same, not settled by declaration order.
constinit const SystemDesc k_mixed_module_systems[] = {
    {.name = "m.write", .phase = Phase::Sim, .fn = sys_move, .writes = "*.Pos, *.Vel"},
    {.name = "m.read",  .phase = Phase::Sim, .fn = sys_steer, .reads = "^*.Pos, *.Vel"},
};
KV_MODULE_DEFINE(k_mixed_module) = {.name = "m", .components = k_components, .kinds = k_kinds, .systems = k_mixed_module_systems};

f32 g_seen_pos = -1, g_seen_vel = -1;
void sys_set_both(TickContext& ctx) {
    for (KindRows k : ctx.view<Pos, Vel>()) for (u32 i = 0; i < k.rows(); ++i) if (k.alive(i)) { k.col<Pos>()[i].x += 1; k.col<Vel>()[i].dx += 2; }
}
void sys_see_both(TickContext& ctx) {
    for (KindRows k : ctx.view<const Pos, const Vel>()) for (u32 i = 0; i < k.rows(); ++i) if (k.alive(i)) { g_seen_pos = k.col<const Pos>()[i].x; g_seen_vel = k.col<const Vel>()[i].dx; }
}
constinit const KindDesc k_snap_kinds[] = {{.name = "n.body", .components = "Pos, Vel", .capacity = 8, .early_reads = EarlyReads::Snapshot}};
constinit const SystemDesc k_mixed_snap_systems[] = {
    {.name = "n.read",  .phase = Phase::Sim, .fn = sys_see_both, .reads = "^*.Pos, *.Vel", .state = StateUse::None},
    {.name = "n.write", .phase = Phase::Sim, .fn = sys_set_both, .writes = "*.Pos, *.Vel", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_mixed_snap) = {.name = "n", .components = k_components, .kinds = k_snap_kinds, .systems = k_mixed_snap_systems};

// One covers relation for the schedule and the checks: a write on a kind covers its
// attachments too, so it and a writer of one of them need an order (they once shared a
// level and could attach on one store at once); a cosmetic attachment read into a sim
// write is the flow rule's, like a column.
constinit const KindDesc k_attach_kinds[] = {{.name = "t.npc", .components = "Pos, Vel", .capacity = 8, .attachable = "Glow"}};
constinit const SystemDesc k_attach_race_systems[] = {
    {.name = "t.spawn", .phase = Phase::Sim, .fn = sys_other, .writes = "t.npc", .state = StateUse::None},
    {.name = "t.tag",   .phase = Phase::Sim, .fn = sys_other, .writes = "t.npc.@Glow", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_attach_race) = {.name = "t", .components = k_components, .kinds = k_attach_kinds, .systems = k_attach_race_systems};
constinit const SystemDesc k_attach_flow_systems[] = {{.name = "t.leak", .phase = Phase::Sim, .fn = sys_other, .reads = "t.npc.@Glow", .writes = "t.npc.Vel"}};
KV_MODULE_DEFINE(k_attach_flow) = {.name = "t", .components = k_components, .kinds = k_attach_kinds, .systems = k_attach_flow_systems};

// One mode per resource: two names covering Pos as read and read early are refused, as is
// an early read of what the system writes; a kind's read is a default its named columns
// override; a filter can leave a component out; module state is not named, and an early
// read is of a kind or a column only.
constinit const SystemDesc k_two_modes_systems[] = {{.name = "o.two", .phase = Phase::Sim, .fn = sys_other, .reads = "*.Pos, ^s.body.Pos"}};
KV_MODULE_DEFINE(k_two_modes) = {.name = "o", .systems = k_two_modes_systems};
constinit const SystemDesc k_early_own_systems[] = {{.name = "o.own", .phase = Phase::Sim, .fn = sys_other, .reads = "^s.body.Pos", .writes = "s.body.Pos"}};
KV_MODULE_DEFINE(k_early_own) = {.name = "o", .systems = k_early_own_systems};
constinit const KindDesc k_o_kinds[] = {{.name = "o.thing", .components = "Pos, Vel", .capacity = 8}};
constinit const SystemDesc k_kind_default_systems[] = {
    {.name = "o.vel",  .phase = Phase::Sim, .fn = sys_other, .writes = "o.thing.Vel", .state = StateUse::None},
    {.name = "o.move", .phase = Phase::Sim, .fn = sys_other, .reads = "o.thing.Vel", .writes = "o.thing.Pos", .state = StateUse::None},
    {.name = "o.look", .phase = Phase::Sim, .fn = sys_other, .reads = "o.thing, ^o.thing.Pos", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_kind_default) = {.name = "o", .components = k_components, .kinds = k_o_kinds, .systems = k_kind_default_systems};
constinit const SystemDesc k_excluded_systems[] = {{.name = "j.split", .phase = Phase::Sim, .fn = sys_other, .reads = "*[Vel-Glow].Pos, ^*[Glow].Pos"}};
KV_MODULE_DEFINE(k_excluded) = {.name = "j", .components = k_components, .kinds = k_filter_kinds, .systems = k_excluded_systems};
constinit const SystemDesc k_named_state_systems[] = {{.name = "o.peek", .phase = Phase::Sim, .fn = sys_other, .reads = "a.state"}};
KV_MODULE_DEFINE(k_named_state) = {.name = "o", .systems = k_named_state_systems};
constinit const SystemDesc k_early_queue_systems[] = {{.name = "o.peek", .phase = Phase::Sim, .fn = sys_other, .reads = "^q.queue"}};
KV_MODULE_DEFINE(k_early_queue) = {.name = "o", .systems = k_early_queue_systems};

// Module state is a resource: readers of it share a level, a writer is ordered against
// them in declaration order.
constinit const SystemDesc k_state_systems[] = {
    {.name = "st.r1", .phase = Phase::Sim, .fn = sys_other, .state = StateUse::Read},
    {.name = "st.r2", .phase = Phase::Sim, .fn = sys_other, .state = StateUse::Read},
    {.name = "st.w",  .phase = Phase::Sim, .fn = sys_other},
};
KV_MODULE_DEFINE(k_state) = {.name = "st", .systems = k_state_systems};

// A snapshot kind's liveness: grow spawns one at once, and count, after it (it also reads
// what grow publishes as it stands), still sees the bodies the phase began with.
struct Tally { u32 n; };
Tally g_tally;
u32 g_counted = 0, g_tally_seen = 0;
void sys_grow(TickContext& ctx) {
    ctx.write_kind("ns.body")->spawn(Pos{1, 1}, Vel{0, 0});
    ++ctx.resource<Tally>("ns.tally")->n;
}
void sys_count(TickContext& ctx) {
    g_counted = 0;
    for (KindRows k : ctx.view<const Pos>()) for (u32 i = 0; i < k.rows(); ++i) g_counted += k.alive(i);
    g_tally_seen = ctx.resource<const Tally>("ns.tally")->n;
}
bool ns_startup(Engine& e, ModuleInstance& self) { return engine_publish(e, self, ".tally", &g_tally); }
constinit const KindDesc k_ns_kinds[] = {{.name = "ns.body", .components = "Pos, Vel", .capacity = 8, .early_reads = EarlyReads::Snapshot}};
constinit const ResourceDesc k_ns_res[] = {{".tally"}};
constinit const SystemDesc k_ns_systems[] = {
    {.name = "ns.count", .phase = Phase::Sim, .fn = sys_count, .reads = "^ns.body, ns.tally", .state = StateUse::None},
    {.name = "ns.grow",  .phase = Phase::Sim, .fn = sys_grow,  .writes = "ns.body, ns.tally", .state = StateUse::None},
};
KV_MODULE_DEFINE(k_ns) = {.name = "ns", .components = k_components, .kinds = k_ns_kinds, .resources = k_ns_res, .systems = k_ns_systems, .startup = ns_startup};

const char* order(const Engine& e, Phase p, char* buf, usize cap) {
    buf[0] = 0;
    for (const ScheduledSystem& s : e.phases.systems[(u32)p]) { strncat(buf, s.desc->name, cap - strlen(buf) - 1); strncat(buf, " ", cap - strlen(buf) - 1); }
    return buf;
}

bool init_fails(TestCtx& c, Span<const ModuleDesc* const> list) {
    Engine f;
    bool ok = engine_init(f, list);
    if (ok) engine_shutdown(f);
    KV_CHECK(c, f.phases.system_count() == 0);
    return !ok;
}

} // namespace

void test_schedule(kv::TestCtx& c) {
    using namespace kv;
    c.begin("schedule");
    char buf[256];

    // Early reads turn the edge around.
    {
        const ModuleDesc* const early[] = {&k_early};
        Engine x;
        KV_CHECK(c, engine_init(x, early));
        KV_CHECK(c, strcmp(order(x, Phase::Sim, buf, sizeof buf), "e.think e.move ") == 0);
        engine_shutdown(x);
        const ModuleDesc* const late[] = {&k_late_read};
        KV_CHECK(c, init_fails(c, late));
        const ModuleDesc* const bad[] = {&k_early_write};
        KV_CHECK(c, init_fails(c, bad));
    }

    // Append resources: no order between appenders, records in unit order; a real resource
    // beside one still needs one; only the owner writes it; only it takes appends.
    {
        const ModuleDesc* const ok[] = {&k_sink_ok};
        Engine x;
        KV_CHECK(c, engine_init(x, ok));
        g_applied[0] = 0;
        x.phases.run_tick(x, x.events, 1.0 / 60.0, 0);
        KV_CHECK(c, strcmp(g_applied, "ab") == 0);
        engine_shutdown(x);
        const ModuleDesc* const clash[] = {&k_sink_clash};
        KV_CHECK(c, init_fails(c, clash));
        const ModuleDesc* const writer[] = {&k_sink_ok, &k_sink_writer};
        KV_CHECK(c, init_fails(c, writer));
        const ModuleDesc* const not_append[] = {&k_a, &k_not_append};
        KV_CHECK(c, init_fails(c, not_append));
    }

    // A reader both before (^) and after one writer cannot be placed, nor settled by an
    // order declared or by its module's; from a snapshot kind it is fine.
    {
        const ModuleDesc* const mixed[] = {&k_mixed_read};
        KV_CHECK(c, init_fails(c, mixed));
        const ModuleDesc* const ordered[] = {&k_mixed_ordered};
        KV_CHECK(c, init_fails(c, ordered));
        const ModuleDesc* const module[] = {&k_mixed_module};
        KV_CHECK(c, init_fails(c, module));
        const ModuleDesc* const snap[] = {&k_mixed_snap};
        Engine x;
        KV_CHECK(c, engine_init(x, snap));
        KV_CHECK(c, strcmp(order(x, Phase::Sim, buf, sizeof buf), "n.write n.read ") == 0);
        engine_table(x, "n.body")->spawn(Pos{10, 0}, Vel{20, 0});
        x.phases.run_tick(x, x.events, 1.0 / 60.0, 0);
        KV_CHECK(c, g_seen_pos == 10 && g_seen_vel == 22); // Pos as the phase found it, Vel as written
        engine_shutdown(x);
        // Forced ordered, the snapshot kind's pair is a contradiction again.
        Engine y;
        y.early_reads = EarlyReadsChoice::Order;
        KV_CHECK(c, !engine_init(y, snap));
        engine_shutdown(y);
    }

    // Attachments are covered by their kind, for the order and the flow rule.
    {
        const ModuleDesc* const race[] = {&k_attach_race};
        KV_CHECK(c, init_fails(c, race));
        const ModuleDesc* const flow[] = {&k_attach_flow};
        KV_CHECK(c, init_fails(c, flow));
    }

    // One mode per resource, and what may be read early.
    {
        const ModuleDesc* const two[] = {&k_a, &k_two_modes};
        KV_CHECK(c, init_fails(c, two));
        const ModuleDesc* const own[] = {&k_a, &k_early_own};
        KV_CHECK(c, init_fails(c, own));
        const ModuleDesc* const named_state[] = {&k_a, &k_named_state};
        KV_CHECK(c, init_fails(c, named_state));
        const ModuleDesc* const early_queue[] = {&k_sink_ok, &k_early_queue};
        KV_CHECK(c, init_fails(c, early_queue));
        const ModuleDesc* const kind_default[] = {&k_kind_default};
        Engine x;
        KV_CHECK(c, engine_init(x, kind_default));
        KV_CHECK(c, strcmp(order(x, Phase::Sim, buf, sizeof buf), "o.vel o.look o.move ") == 0); // Vel as it stands, Pos before the move
        const ScheduledSystem* look = nullptr;
        for (const ScheduledSystem& u : x.phases.systems[(u32)Phase::Sim]) if (!strcmp(u.name(), "o.look")) look = &u;
        Access pos = Access::Read, vel = Access::Early;
        KV_CHECK(c, look && look->mode(engine_resource_id(x, "o.thing.Pos"), &pos) && pos == Access::Early &&
                    look->mode(engine_resource_id(x, "o.thing.Vel"), &vel) && vel == Access::Read);
        // Units by rank, wherever they run: o.look is declared last and runs second.
        KV_CHECK(c, look && look->level == 2 && look->unit == 3);
        // Ids from the context carry their engine.
        TickContext ctx{};
        ctx.engine = &x;
        u32 id = ctx.resource_id("o.thing.Pos");
        KV_CHECK(c, id >> 24 == x.serial && (id & 0xFFFFFF) == engine_resource_id(x, "o.thing.Pos"));
        u32 rows = 0;
        KV_CHECK(c, ctx_column(ctx, id, &TypeKey<Pos>::key, false, &rows) != nullptr);
        engine_shutdown(x);
        const ModuleDesc* const excluded[] = {&k_excluded};
        Engine y;
        KV_CHECK(c, engine_init(y, excluded));
        engine_shutdown(y);
    }

    // Module state: two readers share a level; the writer after them, in declaration order.
    {
        const ModuleDesc* const st[] = {&k_state};
        Engine x;
        KV_CHECK(c, engine_init(x, st));
        const Array<ScheduledSystem>& sim = x.phases.systems[(u32)Phase::Sim];
        KV_CHECK(c, sim.len == 3 && sim[0].level == 1 && sim[1].level == 1 && sim[2].level == 2 && !strcmp(sim[2].name(), "st.w"));
        engine_shutdown(x);
    }

    // A snapshot kind's early reader sees the liveness the phase began with; forced to
    // order, the same declarations are a contradiction.
    {
        const ModuleDesc* const ns[] = {&k_ns};
        Engine x;
        KV_CHECK(c, engine_init(x, ns));
        KV_CHECK(c, strcmp(order(x, Phase::Sim, buf, sizeof buf), "ns.grow ns.count ") == 0);
        engine_table(x, "ns.body")->spawn(Pos{0, 0}, Vel{0, 0});
        x.phases.run_tick(x, x.events, 1.0 / 60.0, 0);
        KV_CHECK(c, g_counted == 1 && engine_table(x, "ns.body")->live() == 2 && g_tally_seen == 1); // the tally as grow left it
        x.phases.run_tick(x, x.events, 1.0 / 60.0, 1);
        KV_CHECK(c, g_counted == 2 && engine_table(x, "ns.body")->live() == 3);
        engine_shutdown(x);
        Engine y;
        y.early_reads = EarlyReadsChoice::Order;
        KV_CHECK(c, !engine_init(y, ns));
        engine_shutdown(y);
    }

    // Joined filters.
    {
        const ModuleDesc* const ok[] = {&k_joined};
        Engine x;
        KV_CHECK(c, engine_init(x, ok));
        const char* o = order(x, Phase::Sim, buf, sizeof buf);
        KV_CHECK(c, strstr(o, "j.both") < strstr(o, "j.reader")); // the reader of what it wrote follows it
        engine_shutdown(x);
        const ModuleDesc* const clash[] = {&k_joined_clash};
        KV_CHECK(c, init_fails(c, clash));
        const ModuleDesc* const unknown[] = {&k_joined_unknown};
        KV_CHECK(c, init_fails(c, unknown));
    }

    // Derived order: writers before readers, the declared edge honored, the kind writer first.
    const ModuleDesc* const list[] = {&k_a, &k_b};
    Engine e;
    e.threads = 1; // the trace is a global string
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, strcmp(order(e, Phase::Sim, buf, sizeof buf), "s.spawn s.steer s.move s.wrap s.glow ") == 0);
    g_trace[0] = 0;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, strcmp(g_trace, "spawn steer move wrap glow ") == 0);

    // The hash: deterministic, sensitive to sim fields, blind to cosmetic ones and dead rows.
    Table* body = engine_table(e, "s.body");
    u64 empty = engine_hash_state(e);
    Handle h0 = body->spawn(Pos{1, 2}, Vel{3, 4}, Glow{0.5f});
    Handle h1 = body->spawn(Pos{5, 6}, Vel{7, 8}, Glow{0.5f});
    u64 two = engine_hash_state(e);
    KV_CHECK(c, two != empty);
    body->get<Glow>(h0)->a = 0.9f;
    KV_CHECK(c, engine_hash_state(e) == two); // cosmetic
    body->get<Vel>(h0)->dx = 30;
    u64 moved = engine_hash_state(e);
    KV_CHECK(c, moved != two);
    body->get<Vel>(h0)->dx = 3;
    KV_CHECK(c, engine_hash_state(e) == two);
    body->destroy(h1);
    u64 one = engine_hash_state(e);
    KV_CHECK(c, one != two && one != empty);
    Handle h2 = body->spawn(Pos{5, 6}, Vel{7, 8}, Glow{0.5f}); // same slot, new generation: a different world
    KV_CHECK(c, h2.index() == h1.index() && engine_hash_state(e) != two);
    // Hashing is on: a tick logs and records it.
    e.hash_ticks = true;
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, e.last_hash == engine_hash_state(e));
    engine_shutdown(e);

    // The same declarations in another module order give the same schedule.
    const ModuleDesc* const swapped[] = {&k_b, &k_a};
    e.threads = 1;
    KV_CHECK(c, engine_init(e, swapped));
    KV_CHECK(c, strcmp(order(e, Phase::Sim, buf, sizeof buf), "s.spawn s.steer s.move s.wrap s.glow ") == 0);
    engine_shutdown(e);

    // Rejections: a second writer of *.Pos with no order; a read-write cycle; the flow rule;
    // after an unknown system; after a system in a later phase.
    const ModuleDesc* const dbl[]   = {&k_a, &k_b, &k_double};
    const ModuleDesc* const cyc[]   = {&k_cycle};
    const ModuleDesc* const flow[]  = {&k_flow};
    const ModuleDesc* const unk[]   = {&k_a, &k_unknown};
    const ModuleDesc* const later[] = {&k_later};
    KV_CHECK(c, init_fails(c, dbl));
    KV_CHECK(c, init_fails(c, cyc));
    KV_CHECK(c, init_fails(c, flow));
    KV_CHECK(c, init_fails(c, unk));
    KV_CHECK(c, init_fails(c, later));
}
