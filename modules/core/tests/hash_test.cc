// The state hash: the same world hashes the same however its kinds were numbered. Two
// engines with the modules in opposite orders, and a third with an extra module ahead of
// them, give their kinds different ids; a handle in a field still hashes the same.
#include "korvid/core/engine.h"
#include "korvid/core/module.h"
#include "korvid/core/test.h"

namespace {

using namespace kv;

struct HPos { f32 x, y; };
struct HRef { Handle other; u32 n; };

constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(HPos, x, y)};
constinit const FieldDesc k_ref_fields[] = {KV_FIELDS(HRef, other, n)};
constinit const ComponentDesc k_components[] = {KV_COMPONENT(HPos, k_pos_fields), KV_COMPONENT(HRef, k_ref_fields)};

constinit const KindDesc k_a_kinds[] = {{.name = "ha.thing", .components = "HPos, HRef", .capacity = 8}};
constinit const KindDesc k_b_kinds[] = {{.name = "hb.mark", .components = "HPos", .capacity = 8}};
constinit const KindDesc k_x_kinds[] = {{.name = "hx.extra", .components = "HPos", .capacity = 8}, {.name = "hx.more", .components = "HPos", .capacity = 8}};
KV_MODULE_DEFINE(k_ha) = {.name = "ha", .components = k_components, .kinds = k_a_kinds};
KV_MODULE_DEFINE(k_hb) = {.name = "hb", .kinds = k_b_kinds};
KV_MODULE_DEFINE(k_hx) = {.name = "hx", .kinds = k_x_kinds};

// The same world in any engine: two marks, a thing pointing at the second.
u64 world_hash(Engine& e, f32 nudge, u16* mark_kind) {
    Table* marks = engine_table(e, "hb.mark");
    Table* things = engine_table(e, "ha.thing");
    marks->spawn(HPos{1, 2});
    Handle m = marks->spawn(HPos{3, 4});
    things->spawn(HPos{5 + nudge, 6}, HRef{m, 7});
    *mark_kind = marks->kind;
    return engine_hash_state(e);
}

} // namespace

void test_hash(kv::TestCtx& c) {
    using namespace kv;
    c.begin("hash");
    const ModuleDesc* const ab[] = {&k_ha, &k_hb};
    const ModuleDesc* const ba[] = {&k_hb, &k_ha};
    const ModuleDesc* const xab[] = {&k_hx, &k_ha, &k_hb};
    u64 h[4];
    u16 kinds[4];
    const Span<const ModuleDesc* const> lists[] = {ab, ba, xab, ab};
    for (u32 i = 0; i < 4; ++i) {
        Engine e;
        KV_CHECK(c, engine_init(e, lists[i]));
        h[i] = world_hash(e, i == 3 ? 0.5f : 0.0f, &kinds[i]);
        engine_shutdown(e);
    }
    KV_CHECK(c, kinds[0] != kinds[1] && kinds[1] != kinds[2]); // the ids did move
    KV_CHECK(c, h[0] == h[1]);
    KV_CHECK(c, h[1] == h[2]); // the extra module's kinds are empty: no state
    KV_CHECK(c, h[3] != h[0]); // and the state still counts
}
