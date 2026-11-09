#include "korvid/collision/module.h"
#include "korvid/core/sort.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/map.h"
#include "korvid/core/profile.h"
#include "korvid/math/fields.h"
#include "korvid/scene/parent.h"
#include "korvid/sprite/transform2.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constinit const FieldDesc k_collider_fields[] = {KV_FIELDS(Collider2, radius, half, offset, layer, mask, pace, shape, body, flags, pad)};
constinit const FieldDesc k_move_fields[]     = {KV_FIELDS(Move2, delta)};
constinit const FieldDesc k_result_fields[]   = {KV_FIELDS(MoveResult2, asked, moved, blocker, pushed, pushed_by, hit_x, hit_y, displaced, pad)};
constinit const ComponentDesc k_components[]  = {
    KV_COMPONENT(Collider2, k_collider_fields), KV_COMPONENT(Move2, k_move_fields), KV_COMPONENT(MoveResult2, k_result_fields),
};

constinit const FieldDesc k_begin_fields[] = {KV_FIELDS(OverlapBegin, a, b)};
constinit const FieldDesc k_end_fields[]   = {KV_FIELDS(OverlapEnd, a, b)};
constinit const EventTypeDesc k_events[] = {
    KV_EVENT_EX(OverlapBegin, k_begin_fields, 0),
    KV_EVENT_EX(OverlapEnd, k_end_fields, 0),
};

struct Pair { u64 a, b; };

struct CollisionState {
    Array<Pair>       prev; // sorted, from the last tick
    Array<Pair>       now;
    Array<Handle>     moved_by_others; // published as collision.moved
    CollisionStaticFn static_fn   = nullptr;
    void*             static_user = nullptr;
    CollisionStats    stats;
};

// ---- shapes -----------------------------------------------------------------------------------

// The collider's bounding box around a position, min corner and max corner.
void box_of(const Collider2& c, point2 p, f32* x0, f32* y0, f32* x1, f32* y1) {
    vec2 h = c.shape == (u8)ColliderShape::Circle ? vec2{c.radius, c.radius} : c.half;
    *x0 = p.x + c.offset.x - h.x; *x1 = p.x + c.offset.x + h.x;
    *y0 = p.y + c.offset.y - h.y; *y1 = p.y + c.offset.y + h.y;
}

bool meets(u32 la, u32 ma, u32 lb, u32 mb) { return (la & mb) && (lb & ma); }

// ---- the overlap broadphase --------------------------------------------------------------------

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

// Phase Sim, after the resolve: every collider into a uniform grid whose cell is the
// largest collider's diameter, so overlapping pairs always sit in neighboring cells;
// pairs in canonical order, sorted, diffed against last tick's.
void broadphase(TickContext& ctx) {
    KV_ZONE("collision.broadphase");
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
            it.h = k.handle_at(i); it.p = t[i].position + c[i].offset;
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
                    if (!meets(a.layer, a.mask, b.layer, b.mask)) continue;
                    if (!overlaps(a, b)) continue;
                    Pair p{a.h.bits, b.h.bits};
                    if (p.a > p.b) { u64 t = p.a; p.a = p.b; p.b = t; }
                    st.now.push(p);
                }
            }
    }
    if (st.now.len > 1) kv::sort(st.now.data, st.now.len, sizeof(Pair), compare_pairs);

    // Diff two sorted lists: new pairs begin, missing pairs end. An End whose entity is
    // gone is dropped at delivery, by the bus's stale-handle rule.
    u32 i = 0, j = 0;
    while (i < st.prev.len || j < st.now.len) {
        int c = i == st.prev.len ? 1 : j == st.now.len ? -1 : compare_pairs(&st.prev[i], &st.now[j]);
        if (c == 0) { ++i; ++j; }
        else if (c < 0) { ctx.emit(OverlapEnd{Handle{st.prev[i].a}, Handle{st.prev[i].b}}); ++i; }
        else { ctx.emit(OverlapBegin{Handle{st.now[j].a}, Handle{st.now[j].b}}); ++j; }
    }
    Array<Pair> t = st.prev; st.prev = st.now; st.now = t;
}

// ---- the resolve -------------------------------------------------------------------------------

// A body as the resolve sees it: its box now (half-open, world units, +y up), where its
// position is, and what it may do.
struct Body {
    Handle h;
    Table* table;
    u32    slot;
    f32    b[4];       // x0, y0, x1, y1
    point2 pos;
    vec2   moved;      // this tick, by itself and by others
    u32    layer, mask;
    f32    pace;
    u8     kind, flags;
    bool   can_move;   // its kind has Move2
    bool   displaced;
    Handle pushed, pushed_by, blocker;
    u8     hit[2];
    u32    next, prev; // the grid's list
    u64    cell;
};

constexpr u32 kNone = 0xFFFFFFFFu;

// The bodies in a uniform grid, filed by the cell of their min corner. A body is no larger
// than a cell, so one that overlaps a region has its min corner within a cell of it.
struct Grid {
    Map<u64, u32> heads;
    f32           cell = 1.0f;

    i32  coord(f32 v) const { return (i32)floorf(v / cell); }
    void insert(Array<Body>& bs, u32 i) {
        Body& b = bs[i];
        b.cell = cell_key(coord(b.b[0]), coord(b.b[1]));
        u32* head = heads.get_or_insert(b.cell, kNone);
        b.prev = kNone;
        b.next = *head;
        if (*head != kNone) bs[*head].prev = i;
        *head = i;
    }
    void remove(Array<Body>& bs, u32 i) {
        Body& b = bs[i];
        if (b.prev != kNone) bs[b.prev].next = b.next;
        else if (u32* head = heads.find(b.cell)) *head = b.next;
        if (b.next != kNone) bs[b.next].prev = b.prev;
        b.next = b.prev = kNone;
    }
    // Calls fn(j) for every body that may touch [x0, x1) x [y0, y1); all of them when the
    // region spans too many cells to be worth walking.
    template <class F>
    void each(Array<Body>& bs, f32 x0, f32 y0, f32 x1, f32 y1, F fn) {
        i32 cx0 = coord(x0) - 1, cy0 = coord(y0) - 1, cx1 = coord(x1), cy1 = coord(y1);
        if ((i64)(cx1 - cx0 + 1) * (i64)(cy1 - cy0 + 1) > 256) {
            for (u32 j = 0; j < bs.len; ++j) fn(j);
            return;
        }
        for (i32 cy = cy0; cy <= cy1; ++cy)
            for (i32 cx = cx0; cx <= cx1; ++cx)
                if (u32* head = heads.find(cell_key(cx, cy)))
                    for (u32 j = *head; j != kNone; j = bs[j].next) fn(j);
    }
};

struct Sweep {
    f32  lo;        // the box's new low edge on the axis
    u32  blocker;   // the body it stopped at, kNone for the world or nothing
    bool hit;
};

// Body i along axis a (0 x, 1 y) by d, against the world and every other solid or
// kinematic body it meets but `skip`. Exact stops: a box stopped by an edge ends on it.
Sweep sweep(CollisionState& st, Array<Body>& bs, Grid& grid, u32 i, u32 a, f32 d, u32 skip) {
    Body& me = bs[i];
    f32 lo = me.b[a], hi = me.b[a + 2], size = hi - lo;
    Sweep out{lo + d, kNone, false};
    if (d == 0.0f) { out.lo = lo; return out; }
    if (st.static_fn) {
        rect2 box{me.b[0], me.b[1], me.b[2] - me.b[0], me.b[3] - me.b[1]};
        bool hit = false, inside = false;
        rect2 got = st.static_fn(st.static_user, box, a ? vec2{0.0f, d} : vec2{d, 0.0f}, &hit, &inside);
        f32 glo = a ? got.y : got.x;
        if (hit && ((d > 0.0f && glo < out.lo) || (d < 0.0f && glo > out.lo))) { out.lo = glo; out.hit = true; }
    }
    // The region the box covers on its way.
    f32 r[4] = {me.b[0], me.b[1], me.b[2], me.b[3]};
    if (d > 0.0f) r[a + 2] += d; else r[a] += d;
    u32 o = 1 - a;
    grid.each(bs, r[0], r[1], r[2], r[3], [&](u32 j) {
        if (j == i || j == skip) return;
        const Body& it = bs[j];
        if (!meets(me.layer, me.mask, it.layer, it.mask)) return;
        if (!(it.b[o] < me.b[o + 2] && me.b[o] < it.b[o + 2])) return; // apart on the other axis (touching is apart)
        f32 jlo = it.b[a], jhi = it.b[a + 2];
        if (jlo < hi && lo < jhi) { // overlapping now: it may move away from the other's middle, not toward it
            f32 mid2 = lo + hi, jmid2 = jlo + jhi;
            if ((d > 0.0f && mid2 < jmid2) || (d < 0.0f && mid2 > jmid2)) {
                if (out.lo != lo || out.blocker == kNone || j < out.blocker) out.blocker = j;
                out.lo = lo;
                out.hit = true;
            }
            return;
        }
        if (d > 0.0f && jlo >= hi) {
            f32 stop = jlo - size;
            if (stop < out.lo || (stop == out.lo && out.blocker != kNone && j < out.blocker)) { out.lo = stop; out.hit = true; out.blocker = j; }
        } else if (d < 0.0f && jhi <= lo) {
            f32 stop = jhi;
            if (stop > out.lo || (stop == out.lo && out.blocker != kNone && j < out.blocker)) { out.lo = stop; out.hit = true; out.blocker = j; }
        }
    });
    return out;
}

// Moves body i's box and position along an axis to a new low edge.
void place(Array<Body>& bs, Grid& grid, u32 i, u32 a, f32 new_lo) {
    Body& b = bs[i];
    f32 by = new_lo - b.b[a];
    if (by == 0.0f) return;
    grid.remove(bs, i);
    b.b[a] = new_lo;
    b.b[a + 2] += by;
    if (a) { b.pos.y += by; b.moved.y += by; } else { b.pos.x += by; b.moved.x += by; }
    grid.insert(bs, i);
}

int compare_tables(const void* x, const void* y) { return strcmp((*(Table* const*)x)->name, (*(Table* const*)y)->name); }

// Phase Sim, after every writer of Move2: every intent, in order, swept against the world
// and the bodies as they stand, pushing one deep; then the columns.
void resolve(TickContext& ctx) {
    KV_ZONE("collision.resolve");
    CollisionState& st = ctx.state_as<CollisionState>();
    st.moved_by_others.clear();
    MemScope scope;
    // The kinds with bodies, by name, so the order of the moves is the same whatever order
    // the kinds registered in.
    Array<Table*> tables = array_in<Table*>(mem_scratch());
    for (KindRows k : ctx.view<const Transform2, const Collider2>()) tables.push(k.table);
    if (tables.len > 1) kv::sort(tables.data, tables.len, sizeof(Table*), compare_tables);

    Array<Body> bs = array_in<Body>(mem_scratch());
    Array<u32> movers = array_in<u32>(mem_scratch());
    Array<vec2> intents = array_in<vec2>(mem_scratch());
    f32 largest = 1.0f;
    for (Table* t : tables) {
        KindRows k{&ctx, t};
        Span<const Transform2> xf = k.col<const Transform2>();
        Span<const Collider2> co = k.col<const Collider2>();
        bool can_move = t->has(ComponentType<Move2>::id);
        Span<const Move2> mv = can_move ? k.col<const Move2>() : Span<const Move2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i) || co[i].body == (u8)BodyKind::Sensor) continue;
            Body b{};
            b.h = k.handle_at(i); b.table = t; b.slot = i;
            box_of(co[i], xf[i].position, &b.b[0], &b.b[1], &b.b[2], &b.b[3]);
            b.pos = xf[i].position;
            b.layer = co[i].layer; b.mask = co[i].mask; b.pace = co[i].pace;
            b.kind = co[i].body; b.flags = co[i].flags; b.can_move = can_move;
            b.next = b.prev = kNone;
            f32 w = b.b[2] - b.b[0], h = b.b[3] - b.b[1];
            if (w > largest) largest = w;
            if (h > largest) largest = h;
            if (can_move && b.kind == (u8)BodyKind::Kinematic) { movers.push(bs.len); intents.push(mv[i].delta); }
            bs.push(b);
        }
    }
    Grid grid;
    grid.heads.init(mem_scratch());
    grid.cell = largest;
    for (u32 i = 0; i < bs.len; ++i) grid.insert(bs, i);

    CollisionStats stats;
    stats.bodies = bs.len;
    for (u32 m = 0; m < movers.len; ++m) {
        u32 i = movers[m];
        vec2 want = intents[m];
        if (want.x == 0.0f && want.y == 0.0f) continue;
        ++stats.moves;
        for (u32 a = 0; a < 2; ++a) {
            f32 d = a ? want.y : want.x;
            if (d == 0.0f) continue;
            u32 pushed = kNone; // the body this axis pushed out of the way
            Sweep s = sweep(st, bs, grid, i, a, d, kNone);
            // A pushable solid in the way, for a pusher: it goes at its pace, as far as the
            // mover would go past it, against everything but the mover, pushing nothing.
            if (s.blocker != kNone && (bs[i].flags & ColliderPusher)) {
                Body& j = bs[s.blocker];
                if (j.kind == (u8)BodyKind::Solid && (j.flags & ColliderPushable) && j.can_move) {
                    u32 jb = s.blocker;
                    f32 slow = d * j.pace;
                    f32 gap = s.lo - bs[i].b[a];
                    f32 need = slow - gap;
                    bool inside = bs[jb].b[a] < bs[i].b[a + 2] && bs[i].b[a] < bs[jb].b[a + 2];
                    if (!inside && ((d > 0.0f && need > 0.0f) || (d < 0.0f && need < 0.0f))) {
                        Sweep js = sweep(st, bs, grid, jb, a, need, i);
                        if (js.lo != bs[jb].b[a]) {
                            place(bs, grid, jb, a, js.lo);
                            if (!bs[jb].displaced) st.moved_by_others.push(bs[jb].h);
                            bs[jb].displaced = true;
                            bs[jb].pushed_by = bs[i].h;
                            bs[i].pushed = bs[jb].h;
                            pushed = jb;
                            ++stats.pushes;
                        }
                        s = sweep(st, bs, grid, i, a, slow, kNone);
                    }
                }
            }
            // Ending against what it just pushed is pushing, not being stopped.
            if (s.hit && !(s.blocker != kNone && s.blocker == pushed)) {
                bs[i].hit[a] = 1;
                if (s.blocker != kNone && bs[i].blocker.is_null()) bs[i].blocker = bs[s.blocker].h;
                ++stats.blocked;
            }
            place(bs, grid, i, a, s.lo);
        }
    }

    // Back into the columns: positions, a child's Local2 by the same delta, the results.
    u32 bi = 0;
    for (Table* t : tables) {
        if (!t->has(ComponentType<Move2>::id)) { while (bi < bs.len && bs[bi].table == t) ++bi; continue; }
        KindRows k{&ctx, t};
        Span<Transform2> xf = k.col<Transform2>();
        Span<const Move2> mv = k.col<const Move2>();
        bool child = t->has(ComponentType<Local2>::id) && t->has(ComponentType<Parent>::id);
        Span<Local2> loc = child ? k.col<Local2>() : Span<Local2>();
        Span<MoveResult2> res = t->has(ComponentType<MoveResult2>::id) ? k.col<MoveResult2>() : Span<MoveResult2>();
        for (u32 i = 0; i < res.len; ++i)
            if (k.alive(i)) { res[i] = MoveResult2{}; res[i].asked = mv[i].delta; }
        for (; bi < bs.len && bs[bi].table == t; ++bi) {
            const Body& b = bs[bi];
            if (b.moved.x != 0.0f || b.moved.y != 0.0f) {
                xf[b.slot].position = b.pos;
                if (child) loc[b.slot].position = loc[b.slot].position + b.moved;
            }
            if (res.len) {
                MoveResult2& r = res[b.slot];
                r.moved = b.moved;
                r.blocker = b.blocker;
                r.pushed = b.pushed;
                r.pushed_by = b.pushed_by;
                r.hit_x = b.hit[0];
                r.hit_y = b.hit[1];
                r.displaced = b.displaced;
            }
        }
    }
    st.stats = stats;
}

// Phase PostSim: an intent lasts one tick.
void clear_intents(TickContext& ctx) {
    for (KindRows k : ctx.view<Move2>()) {
        Span<Move2> mv = k.col<Move2>();
        for (u32 i = 0; i < mv.len; ++i) mv[i] = Move2{};
    }
}

bool collision_startup(Engine& e, ModuleInstance& self) {
    CollisionState* s = arena_push<CollisionState>(self.arena);
    if (!s) return false;
    s->prev.alloc = s->now.alloc = s->moved_by_others.alloc = &self.general;
    self.set_state(s);
    return engine_publish(e, self, ".moved", &s->moved_by_others);
}

void collision_shutdown(Engine&, ModuleInstance& self) {
    CollisionState* s = self.state_as<CollisionState>();
    if (s) { s->prev.free(); s->now.free(); s->moved_by_others.free(); }
}

constinit const ResourceDesc k_resources[] = {{".moved"}};
constinit const SystemDesc k_systems[] = {
    {.name = ".resolve", .phase = Phase::Sim, .fn = resolve,
     .reads = "*[Collider2].Transform2, *[Collider2].Collider2, *[Move2].Move2",
     .writes = "*[Move2].Transform2, *[Move2].Local2, *[MoveResult2].MoveResult2, .moved"},
    {.name = ".broadphase", .phase = Phase::Sim, .fn = broadphase, .reads = "*[Collider2].Transform2, *[Collider2].Collider2", .after = ".resolve"},
    {.name = ".clear", .phase = Phase::PostSim, .fn = clear_intents, .writes = "*[Move2].Move2", .state = StateUse::None},
};

} // namespace

KV_MODULE_DEFINE(collision_module) = {
    .name       = "collision",
    .events     = k_events,
    .components = k_components,
    .resources  = k_resources,
    .systems    = k_systems,
    .startup    = collision_startup,
    .shutdown   = collision_shutdown,
    .memory     = {.reserve = 64 * 1024},
};

u32 collision_pair_count(Engine& e) {
    CollisionState* s = engine_module_state_as<CollisionState>(e, "collision");
    return s ? s->prev.len : 0;
}

const CollisionStats* collision_stats(Engine& e) {
    CollisionState* s = engine_module_state_as<CollisionState>(e, "collision");
    return s ? &s->stats : nullptr;
}

void collision_static(Engine& e, CollisionStaticFn fn, void* user) {
    CollisionState* s = engine_module_state_as<CollisionState>(e, "collision");
    if (!s) return;
    s->static_fn = fn;
    s->static_user = user;
}

// ---- queries ----------------------------------------------------------------------------------

u32 collision_query(TickContext& ctx, rect2 area, u32 layers, Array<Handle>& out) {
    u32 n = 0;
    for (KindRows k : ctx.view<const Transform2, const Collider2>()) {
        Span<const Transform2> xf = k.col<const Transform2>();
        Span<const Collider2> co = k.col<const Collider2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i) || !(co[i].layer & layers)) continue;
            f32 x0, y0, x1, y1;
            box_of(co[i], xf[i].position, &x0, &y0, &x1, &y1);
            if (x0 < area.x + area.w && area.x < x1 && y0 < area.y + area.h && area.y < y1) { out.push(k.handle_at(i)); ++n; }
        }
    }
    return n;
}

CollisionHit collision_cast(TickContext& ctx, rect2 box, vec2 delta, u32 layers, Handle ignore) {
    CollisionHit best;
    best.t = 1.0f;
    for (KindRows k : ctx.view<const Transform2, const Collider2>()) {
        Span<const Transform2> xf = k.col<const Transform2>();
        Span<const Collider2> co = k.col<const Collider2>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i) || !(co[i].layer & layers) || k.handle_at(i) == ignore) continue;
            f32 b[4];
            box_of(co[i], xf[i].position, &b[0], &b[1], &b[2], &b[3]);
            // The slabs: when the moving box's extent enters and leaves the other's, per axis.
            f32 enter = 0.0f, leave = 1.0f;
            bool miss = false;
            const f32 lo[2] = {box.x, box.y}, hi[2] = {box.x + box.w, box.y + box.h}, d[2] = {delta.x, delta.y};
            for (u32 a = 0; a < 2 && !miss; ++a) {
                if (d[a] == 0.0f) { if (!(lo[a] < b[a + 2] && b[a] < hi[a])) miss = true; continue; }
                f32 t0 = (b[a] - hi[a]) / d[a], t1 = (b[a + 2] - lo[a]) / d[a];
                if (t0 > t1) { f32 t = t0; t0 = t1; t1 = t; }
                if (t0 > enter) enter = t0;
                if (t1 < leave) leave = t1;
                if (enter >= leave) miss = true;
            }
            if (miss || enter >= best.t || (enter == best.t && !best.entity.is_null())) continue;
            best.t = enter;
            best.entity = k.handle_at(i);
        }
    }
    if (best.entity.is_null()) best.t = 1.0f;
    return best;
}

} // namespace kv
