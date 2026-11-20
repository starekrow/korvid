// Attachments and property bags: a component carried by one entity of a ktable that lists it
// as attachable, stored sparsely, reached by systems that declare "ktable.@Component" and by
// iteration in slot order; never part of a view, so engine systems ignore it; hashed;
// stale when its entity dies and swept at the boundary. Bags of typed named values the
// same way. A ktable may not attach what it has as a column, nor something unknown.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct APos { f32 x; };
struct Talk { u32 line; f32 mood; };
struct Glowing { f32 a; };
constinit const FieldDesc k_apos_fields[] = {KV_FIELD(APos, x)};
constinit const FieldDesc k_talk_fields[] = {KV_FIELDS(Talk, line, mood)};
constinit const FieldDesc k_glow_fields[] = {KV_FIELD(Glowing, a)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(APos, k_apos_fields), KV_COMPONENT(Talk, k_talk_fields), KV_COMPONENT(Glowing, k_glow_fields),
};
constinit const KTableDesc k_ktables[] = {
    {.name = "x.npc",   .components = "APos", .capacity = 16, .attachable = "Talk, Glowing", .props = true},
    {.name = "x.lamp",  .components = "APos, Glowing", .capacity = 4},
};

struct Seen { u32 glowing_rows; u32 talkers; u32 lines; i64 gold; f64 speed; const char* who; bool flag; };
constinit Seen g = {};

// Sim: a view of every ktable with Glowing sees the lamp's column and nothing attached.
void sys_glow(TickContext& ctx) {
    g.glowing_rows = 0;
    for (KTableRows k : ctx.view<const Glowing>())
        for (u32 i = 0; i < k.rows(); ++i) g.glowing_rows += k.alive(i);
}
// Sim: the talkers, through the attachments and the bags.
void sys_talk(TickContext& ctx) {
    g.talkers = 0; g.lines = 0;
    for (Attached<Talk> a : ctx.attached_all<Talk>("x.npc")) { ++g.talkers; g.lines += a.value->line; a.value->mood += 1.0f; }
    const KTable* npcs = ctx.read_ktable("x.npc");
    for (u32 i = 0; i < npcs->rows(); ++i) {
        if (!npcs->is_alive(i)) continue;
        Handle h = npcs->handle_at(i);
        if (!ctx.prop_has(h, "gold")) continue;
        g.gold = ctx.prop_int(h, "gold");
        g.speed = ctx.prop_float(h, "gold"); // an int read as a float
        g.who = ctx.prop_str(h, "who");
        g.flag = ctx.prop_bool(h, "flag", true);
    }
}

constinit const SystemDesc k_systems[] = {
    {.name = "x.glow", .phase = Phase::Sim, .fn = sys_glow, .reads = "*.Glowing"},
    {.name = "x.talk", .phase = Phase::Sim, .fn = sys_talk, .reads = "x.npc, x.npc.@props", .writes = "x.npc.@Talk"},
};
KV_MODULE_DEFINE(k_x) = {.name = "x", .components = k_components, .ktables = k_ktables, .systems = k_systems};

constinit const KTableDesc k_bad_ktables[] = {{.name = "y.both", .components = "APos", .capacity = 4, .attachable = "APos"}};
KV_MODULE_DEFINE(k_y) = {.name = "y", .ktables = k_bad_ktables};
constinit const KTableDesc k_unknown_ktables[] = {{.name = "z.thing", .components = "APos", .capacity = 4, .attachable = "Nope"}};
KV_MODULE_DEFINE(k_z) = {.name = "z", .ktables = k_unknown_ktables};

} // namespace

void test_attach(kv::TestCtx& c) {
    using namespace kv;
    c.begin("attachments and bags");
    const ModuleDesc* const list[] = {&k_x};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    KV_CHECK(c, engine_resource_id(e, "x.npc.@Talk") && engine_resource_id(e, "x.npc.@props") && !engine_resource_id(e, "x.lamp.@props"));
    KTable* npcs = engine_ktable(e, "x.npc");
    KTable* lamps = engine_ktable(e, "x.lamp");
    Handle a = npcs->spawn(APos{1}), b = npcs->spawn(APos{2}), cc = npcs->spawn(APos{3});
    lamps->spawn(APos{0}, Glowing{1});
    u64 bare = npcs->hash();

    // Attach: the entity has it, its neighbours do not; the ktable's hash follows.
    KV_CHECK(c, npcs->attach(b, Talk{7, 0.5f}) && npcs->attached<Talk>(b)->line == 7 && !npcs->attached<Talk>(a));
    KV_CHECK(c, npcs->attach(a, Glowing{0.25f}) && npcs->hash() != bare);
    KV_CHECK(c, !lamps->attach(lamps->handle_at(0), ComponentType<Talk>::id, &a)); // not attachable there
    npcs->attach(cc, Talk{5, 0.0f});
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 0);
    KV_CHECK(c, g.glowing_rows == 1);                 // the lamp's column; the npc's attached Glowing is not in a view
    KV_CHECK(c, g.talkers == 2 && g.lines == 12 && npcs->attached<Talk>(b)->mood == 1.5f);

    // Bags: typed values, conversions, removal; part of the hash.
    TickContext ctx{};
    ctx.engine = &e;
    ctx.events = &e.events;
    u64 before = npcs->hash();
    KV_CHECK(c, ctx.prop_set_int(b, "gold", 40) && ctx.prop_set_str(b, "who", "baker") && ctx.prop_set_bool(b, "flag", false));
    KV_CHECK(c, ctx.prop_set_float(b, "speed", 1.5) && ctx.prop_float(b, "speed") == 1.5 && ctx.prop_int(b, "speed") == 1);
    KV_CHECK(c, npcs->hash() != before && !ctx.prop_has(a, "gold") && ctx.prop_int(a, "gold", -1) == -1);
    KV_CHECK(c, ctx.prop_remove(b, "speed") && !ctx.prop_has(b, "speed") && !ctx.prop_remove(b, "speed"));
    KV_CHECK(c, ctx.prop_set_handle(b, "friend", a) && ctx.prop_handle(b, "friend") == a);
    e.phases.run_tick(e, e.events, 1.0 / 60.0, 1);
    KV_CHECK(c, g.gold == 40 && g.speed == 40.0 && !strcmp(g.who, "baker") && g.flag == false);
    char text[256];
    npcs->describe(b, text, sizeof text);
    KV_CHECK(c, strstr(text, "@Talk{") && strstr(text, "@props{") && strstr(text, "gold=40") && strstr(text, "who=\"baker\""));

    // Detach; then death: stale at once, swept at the boundary, not inherited by the slot.
    KV_CHECK(c, npcs->detach(cc, ComponentType<Talk>::id) && !npcs->attached<Talk>(cc) && !npcs->detach(cc, ComponentType<Talk>::id));
    npcs->destroy(b);
    KV_CHECK(c, !npcs->attached<Talk>(b) && npcs->sparse_dirty);
    engine_apply_commands(e);
    KV_CHECK(c, !npcs->sparse_dirty && npcs->attachment(ComponentType<Talk>::id)->rows.count() == 0 && npcs->props.len == 0);
    KV_CHECK(c, npcs->attachment(ComponentType<Glowing>::id)->rows.count() == 1); // a's
    engine_shutdown(e);

    // A ktable may not attach its own column, nor an unknown component.
    const ModuleDesc* const both[] = {&k_x, &k_y};
    Engine f;
    KV_CHECK(c, !engine_init(f, both));
    const ModuleDesc* const unknown[] = {&k_x, &k_z};
    Engine h;
    KV_CHECK(c, !engine_init(h, unknown));
}
