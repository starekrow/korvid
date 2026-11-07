#include "korvid/collision/module.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/map.h"
#include "korvid/math/fields.h"
#include "korvid/sprite/transform2.h"
#include <math.h>
#include <stdlib.h>

namespace kv {

namespace {

constinit const FieldDesc k_collider_fields[] = {KV_FIELDS(Collider2, radius, half, layer, mask, shape)};
constinit const ComponentDesc k_components[]  = {KV_COMPONENT(Collider2, k_collider_fields)};

constinit const FieldDesc k_begin_fields[] = {KV_FIELDS(OverlapBegin, a, b)};
constinit const FieldDesc k_end_fields[]   = {KV_FIELDS(OverlapEnd, a, b)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(OverlapBegin, k_begin_fields, 0),
    KV_EVENT_EX(OverlapEnd, k_end_fields, 0),
};

struct Pair { u64 a, b; };

struct CollisionState {
    Array<Pair> prev; // sorted, from the last tick
    Array<Pair> now;
};

// One collider as the broadphase sees it.
struct Item {
    Handle h;
    point2 p;
    f32    r;     // bounding radius
    u32    layer, mask;
    u8     shape;
    vec2   half;
};

int compare_pairs(const void* x, const void* y) {
    const Pair& a = *(const Pair*)x;
    const Pair& b = *(const Pair*)y;
    if (a.a != b.a) return a.a < b.a ? -1 : 1;
    return a.b < b.b ? -1 : a.b > b.b ? 1 : 0;
}

bool overlaps(const Item& a, const Item& b) {
    vec2 d = b.p - a.p;
    bool ca = a.shape == (u8)ColliderShape::Circle, cb = b.shape == (u8)ColliderShape::Circle;
    if (ca && cb) { f32 r = a.r + b.r; return length_sq(d) < r * r; }
    if (!ca && !cb) return fabsf(d.x) < a.half.x + b.half.x && fabsf(d.y) < a.half.y + b.half.y;
    // Circle against box: the box's nearest point to the circle's center.
    const Item& box = ca ? b : a;
    const Item& cir = ca ? a : b;
    vec2 off = cir.p - box.p;
    f32 cx = off.x < -box.half.x ? -box.half.x : off.x > box.half.x ? box.half.x : off.x;
    f32 cy = off.y < -box.half.y ? -box.half.y : off.y > box.half.y ? box.half.y : off.y;
    vec2 gap{off.x - cx, off.y - cy};
    return length_sq(gap) < cir.r * cir.r;
}

u64 cell_key(i32 cx, i32 cy) { return ((u64)(u32)cx << 32) | (u32)cy; }

// Phase Sim, after the movers: every kind with Transform2 and Collider2 into a uniform
// grid whose cell is the largest collider's diameter, so overlapping pairs always sit in
// neighboring cells; pairs in canonical order, sorted, diffed against last tick's.
void broadphase(TickContext& ctx) {
    CollisionState& st = ctx.state_as<CollisionState>();
    MemScope scope;
    Array<Item> items = array_in<Item>(mem_scratch());
    f32 max_r = 0.5f;
    for (KindRows k : ctx.view<const Transform2, const Collider2>()) {
        Span<const Transform2> t = k.col<const Transform2>();
        Span<const Collider2>  c = k.col<const Collider2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            Item it;
            it.h = k.handle_at(i); it.p = t[i].position;
            it.shape = c[i].shape; it.half = c[i].half; it.layer = c[i].layer; it.mask = c[i].mask;
            it.r = it.shape == (u8)ColliderShape::Circle ? c[i].radius : length(c[i].half);
            if (it.r > max_r) max_r = it.r;
            items.push(it);
        }
    }

    f32 cell = 2.0f * max_r;
    Map<u64, u32> heads;
    heads.init(mem_scratch());
    Array<u32> next = array_in<u32>(mem_scratch());
    next.resize_fill(items.len, 0xFFFFFFFFu);
    for (u32 i = 0; i < items.len; ++i) {
        u64 key = cell_key((i32)floorf(items[i].p.x / cell), (i32)floorf(items[i].p.y / cell));
        u32* head = heads.get_or_insert(key, 0xFFFFFFFFu);
        next[i] = *head;
        *head = i;
    }

    st.now.clear();
    for (u32 i = 0; i < items.len; ++i) {
        const Item& a = items[i];
        i32 cx = (i32)floorf(a.p.x / cell), cy = (i32)floorf(a.p.y / cell);
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) {
                u32* head = heads.find(cell_key(cx + dx, cy + dy));
                if (!head) continue;
                for (u32 j = *head; j != 0xFFFFFFFFu; j = next[j]) {
                    if (j <= i) continue; // each unordered pair once
                    const Item& b = items[j];
                    if (!(a.layer & b.mask) || !(b.layer & a.mask)) continue;
                    if (!overlaps(a, b)) continue;
                    Pair p{a.h.bits, b.h.bits};
                    if (p.a > p.b) { u64 t = p.a; p.a = p.b; p.b = t; }
                    st.now.push(p);
                }
            }
    }
    if (st.now.len > 1) qsort(st.now.data, st.now.len, sizeof(Pair), compare_pairs);

    // Diff two sorted lists: new pairs begin, missing pairs end. An End whose entity is
    // gone is dropped at delivery, by the bus's stale-handle rule.
    u32 i = 0, j = 0;
    while (i < st.prev.len || j < st.now.len) {
        int c = i == st.prev.len ? 1 : j == st.now.len ? -1 : compare_pairs(&st.prev[i], &st.now[j]);
        if (c == 0) { ++i; ++j; }
        else if (c < 0) { ctx.events->emit(OverlapEnd{Handle{st.prev[i].a}, Handle{st.prev[i].b}}); ++i; }
        else { ctx.events->emit(OverlapBegin{Handle{st.now[j].a}, Handle{st.now[j].b}}); ++j; }
    }
    Array<Pair> t = st.prev; st.prev = st.now; st.now = t;
}

bool collision_startup(Engine&, ModuleInstance& self) {
    CollisionState* s = arena_push<CollisionState>(self.arena);
    if (!s) return false;
    s->prev.alloc = s->now.alloc = &self.general;
    self.set_state(s);
    return true;
}

void collision_shutdown(Engine&, ModuleInstance& self) {
    CollisionState* s = self.state_as<CollisionState>();
    if (s) { s->prev.free(); s->now.free(); }
}

constinit const char* const k_reads[] = {"*.Transform2", "*.Collider2"};
constinit const SystemDesc k_systems[] = {
    {.name = "collision.broadphase", .phase = Phase::Sim, .fn = broadphase, .reads = k_reads},
};

} // namespace

KV_MODULE_DEFINE(collision_module) = {
    .name       = "collision",
    .events     = k_events,
    .components = k_components,
    .systems    = k_systems,
    .startup    = collision_startup,
    .shutdown   = collision_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

u32 collision_pair_count(Engine& e) {
    CollisionState* s = engine_module_state_as<CollisionState>(e, "collision");
    return s ? s->prev.len : 0;
}

} // namespace kv
