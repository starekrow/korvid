// The name language: '.' expands to the module's prefix (and interns), '?' survives in
// front, other names pass through untouched; comma lists split and trim, skip empties,
// and a null list is nothing. Then the whole thing through an engine: dot names in the
// tables resolve to the same ids as full ones, and a bad name still fails init.
#include "korvid/core/engine.h"
#include "korvid/core/test.h"
#include <string.h>

namespace {

using namespace kv;

struct Pos { f32 x; };
constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(Pos, x)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(Pos, k_pos_fields)};
constinit const KindDesc k_kinds[] = {{.name = ".thing", .components = " Pos ", .capacity = 16}};
constinit const ResourceDesc k_resources[] = {{".score"}};
void noop(TickContext&) {}
constinit const SystemDesc k_systems[] = {
    {.name = ".a", .phase = Phase::Sim, .fn = noop, .writes = ".thing.Pos, .score"},
    {.name = ".b", .phase = Phase::Sim, .fn = noop, .reads = ".thing.Pos", .after = "?.a, ?absent.nobody"},
};
KV_MODULE_DEFINE(k_q) = {.name = "q", .components = k_components, .kinds = k_kinds, .resources = k_resources, .systems = k_systems};

constinit const SystemDesc k_bad_systems[] = {{.name = ".c", .phase = Phase::Sim, .fn = noop, .reads = ".nothing"}};
KV_MODULE_DEFINE(k_bad) = {.name = "q", .components = k_components, .kinds = k_kinds, .systems = k_bad_systems};

// "?name" in a module that is there: the system has to be too, so a typo is an error.
constinit const SystemDesc k_typo_systems[] = {
    {.name = ".a", .phase = Phase::Sim, .fn = noop},
    {.name = ".b", .phase = Phase::Sim, .fn = noop, .after = "?.aa"},
};
KV_MODULE_DEFINE(k_typo) = {.name = "q", .systems = k_typo_systems};

} // namespace

void test_qualify(kv::TestCtx& c) {
    using namespace kv;
    c.begin("qualify");

    KV_CHECK(c, strcmp(engine_qualify("rocks", ".move"), "rocks.move") == 0);
    KV_CHECK(c, strcmp(engine_qualify("rocks", "?.move"), "?rocks.move") == 0);
    KV_CHECK(c, strcmp(engine_qualify("rocks", "input.state"), "input.state") == 0);
    KV_CHECK(c, strcmp(engine_qualify("rocks", "*.Velocity"), "*.Velocity") == 0);
    KV_CHECK(c, engine_qualify("rocks", ".move") == engine_qualify("rocks", ".move")); // interned: one pointer
    KV_CHECK(c, strcmp(engine_qualify(nullptr, ".move"), ".move") == 0);

    char out[64];
    const char* list = " a, .b ,,c.d , ";
    KV_CHECK(c, name_list_next(list, out, sizeof out) && strcmp(out, "a") == 0);
    KV_CHECK(c, name_list_next(list, out, sizeof out) && strcmp(out, ".b") == 0);
    KV_CHECK(c, name_list_next(list, out, sizeof out) && strcmp(out, "c.d") == 0);
    KV_CHECK(c, !name_list_next(list, out, sizeof out));
    const char* none = nullptr;
    KV_CHECK(c, !name_list_next(none, out, sizeof out));
    const char* empty = "";
    KV_CHECK(c, !name_list_next(empty, out, sizeof out));

    const ModuleDesc* const list1[] = {&k_q};
    Engine e;
    KV_CHECK(c, engine_init(e, list1));
    KV_CHECK(c, engine_table(e, "q.thing") && strcmp(engine_table(e, "q.thing")->name, "q.thing") == 0);
    KV_CHECK(c, engine_resource_id(e, "q.score") && engine_resource_id(e, "q.thing.Pos"));
    KV_CHECK(c, engine_system_phase(e, "q.a") == Phase::Sim && engine_system_phase(e, ".a") == Phase::Count);
    KV_CHECK(c, e.phases.systems[(u32)Phase::Sim].len == 2 && strcmp(e.phases.systems[(u32)Phase::Sim][1].name(), "q.b") == 0);
    engine_shutdown(e);

    const ModuleDesc* const list2[] = {&k_bad};
    Engine f;
    KV_CHECK(c, !engine_init(f, list2)); // ".nothing" is q.nothing, which nobody declares
    engine_shutdown(f);

    const ModuleDesc* const list3[] = {&k_typo};
    Engine g;
    KV_CHECK(c, !engine_init(g, list3)); // "?.aa": module q is here and has no q.aa
    engine_shutdown(g);
}
