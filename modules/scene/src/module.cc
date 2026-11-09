#include "korvid/scene/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/profile.h"
#include "korvid/math/fields.h"
#include "korvid/sprite/transform2.h"
#include <math.h>

namespace kv {

Placement2 placement_compose(const Placement2& parent, const Local2& local) {
    f32 cs = cosf(parent.rotation), sn = sinf(parent.rotation);
    f32 lx = local.position.x * parent.scale.x, ly = local.position.y * parent.scale.y;
    Placement2 w;
    w.position = point2{parent.position.x + cs * lx - sn * ly, parent.position.y + sn * lx + cs * ly};
    w.rotation = parent.rotation + local.rotation;
    w.scale    = parent.scale * local.scale;
    return w;
}

namespace {

struct SceneState {
    SceneStats stats;
    bool       cycle_logged = false;
};

struct ChildRef { u32 table; u32 row; };

// The depth of a child: 1 for the child of a root, one more per parent that is itself a
// child. 0 for a cycle or a chain past the cap. Stops counting at a parent that does not
// resolve: that one is an orphan handled on its own, and this child composes against
// whatever it reads.
u32 depth_of(Engine& e, Table* child_table, u32 row) {
    u32 d = 1;
    Table* t = child_table;
    Handle h = t->handle_at(row);
    for (;;) {
        const Parent* p = t->get<Parent>(h);
        if (!p || p->parent.is_null()) return d;
        Table* pt = engine_table_of(e, p->parent);
        if (!pt || !pt->valid(p->parent) || !pt->has(ComponentType<Parent>::id)) return d;
        if (++d > KV_SCENE_MAX_DEPTH) return 0;
        t = pt;
        h = p->parent;
    }
}

// Phase PostSim, every tick. Two passes: depth and orphan handling per child row, then
// composition by depth so a parent is final before its children read it. The scratch
// arena holds the lists; nothing persists between ticks but the counters.
void scene_compose(TickContext& ctx) {
    KV_ZONE("scene.compose");
    SceneState& st = ctx.state_as<SceneState>();
    Engine& e = *ctx.engine;
    MemScope scope;
    Array<ChildRef> by_depth[KV_SCENE_MAX_DEPTH + 1];
    for (Array<ChildRef>& a : by_depth) a = array_in<ChildRef>(mem_scratch());
    SceneStats s;

    for (KindRows k : ctx.view<Transform2, const Local2, Parent>()) {
        Span<Parent> parents = k.col<Parent>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            Parent& p = parents[i];
            if (p.parent.is_null()) continue;
            Table* pt = engine_table_of(e, p.parent);
            if (!pt || !pt->valid(p.parent) || !pt->has(ComponentType<Transform2>::id)) {
                ++s.orphaned;
                if (p.orphan == OrphanPolicy::Detach) p.parent = Handle{};
                else ctx.destroy(k.handle_at(i));
                continue;
            }
            u32 d = depth_of(e, k.table, i);
            if (d == 0) {
                ++s.cycles;
                if (!st.cycle_logged) {
                    st.cycle_logged = true;
                    char text[96];
                    kv_handle_describe(k.handle_at(i).bits, text, sizeof text);
                    KV_WARN("scene", "parent chain of %s is a cycle or deeper than %u: left as a root", text, KV_SCENE_MAX_DEPTH);
                }
                continue;
            }
            if (d > s.max_depth) s.max_depth = d;
            by_depth[d].push(ChildRef{k.table->index, i});
        }
    }

    for (u32 d = 1; d <= s.max_depth; ++d) {
        for (ChildRef r : by_depth[d]) {
            Table& t = e.tables[r.table];
            const Parent& p = *(const Parent*)(t.column<Parent>()->data + (usize)r.row * t.column<Parent>()->stride);
            Table* pt = engine_table_of(e, p.parent);
            const Transform2* pw = pt->get<Transform2>(p.parent);
            const Local2& local  = *(const Local2*)(t.column<Local2>()->data + (usize)r.row * t.column<Local2>()->stride);
            Transform2& w        = *(Transform2*)(t.column<Transform2>()->data + (usize)r.row * t.column<Transform2>()->stride);
            w = placement_compose(*pw, local);
            ++s.composed;
        }
    }
    st.stats = s;
    if (s.composed) KV_MARK_V("scene composed", s.composed);
}

bool scene_startup(Engine&, ModuleInstance& self) {
    SceneState* s = arena_push<SceneState>(self.arena);
    if (!s) return false;
    self.set_state(s);
    return true;
}

constinit const FieldDesc k_parent_fields[] = {KV_FIELDS(Parent, parent, orphan)};
constinit const FieldDesc k_local2_fields[] = {KV_FIELDS(Local2, position, rotation, scale)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Parent, k_parent_fields),
    KV_COMPONENT(Local2, k_local2_fields),
};

constinit const SystemDesc k_systems[] = {
    {.name = ".compose", .phase = Phase::PostSim, .fn = scene_compose, .reads = "*[Local2].Local2",
     .writes = "*.Transform2, *[Parent].Parent", .runs = RunWhen::Always},
};

} // namespace

KV_MODULE_DEFINE(scene_module) = {
    .name       = "scene",
    .components = k_components,
    .systems    = k_systems,
    .startup    = scene_startup,
    .memory     = {.reserve = 16 * 1024},
};

bool scene_attach(Table& table, Handle child, Handle parent, const Local2& local, OrphanPolicy orphan) {
    Parent* p = table.get<Parent>(child);
    Local2* l = table.get<Local2>(child);
    if (!p || !l) return false;
    p->parent = parent;
    p->orphan = orphan;
    *l = local;
    return true;
}

bool scene_detach(Table& table, Handle child) {
    Parent* p = table.get<Parent>(child);
    if (!p) return false;
    p->parent = Handle{};
    return true;
}

const SceneStats* scene_stats(Engine& e) {
    SceneState* s = engine_module_state_as<SceneState>(e, "scene");
    return s ? &s->stats : nullptr;
}

} // namespace kv
