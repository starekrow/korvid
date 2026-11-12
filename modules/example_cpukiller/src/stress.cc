// The engine-level scenario: one kind of a few hundred thousand bodies, four stateless
// systems that integrate in parallel, bounce and emit, churn entities by chunk commands,
// and sum a reduction, plus a handler that eats the events. Run at one thread and at
// many, the state hash must agree and the tick rate is the number worth having.
#include "korvid/example_cpukiller/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/random.h"

namespace kv {

namespace {

struct Pos { f32 x, y; };
struct Vel { f32 dx, dy; };
struct Age { u32 ticks; };
constinit const FieldDesc k_pos_fields[] = {KV_FIELDS(Pos, x, y)};
constinit const FieldDesc k_vel_fields[] = {KV_FIELDS(Vel, dx, dy)};
constinit const FieldDesc k_age_fields[] = {KV_FIELDS(Age, ticks)};
constinit const ComponentDesc k_components[] = {
    KV_COMPONENT(Pos, k_pos_fields, CompPrev), KV_COMPONENT(Vel, k_vel_fields), KV_COMPONENT(Age, k_age_fields),
};
constinit u32 g_body_kind = 0;
constinit const KindDesc k_kinds[] = {{.name = ".body", .components = "Pos, Vel, Age", .capacity = 1u << 20, .id = &g_body_kind}};

struct Bounced { Handle who; };
constinit const FieldDesc k_bounced_fields[] = {KV_FIELDS(Bounced, who)};
constinit const EventTypeDesc k_events[] = {KV_EVENT_EX(Bounced, k_bounced_fields, 0)};

struct Stress { u32 bodies; u64 bounces_handled; f64 energy; u32 churned; };
u32 g_bodies = 200000;

constexpr f32 kBox = 1000.0f;

// Views, not kind reads: a kind read would cover every column and drag Pos into
// integrate's read set, which bounce writes, and the two would form a cycle.
void integrate(TickContext& ctx) {
    for (KindRows k : ctx.view<Pos, const Vel>()) {
        Span<Pos> pos = k.col<Pos>();
        Span<const Vel> vel = k.col<const Vel>();
        parallel_for(ctx, k.rows(), 2048, [&](ChunkContext&, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                pos[i].x += vel[i].dx * (1.0f / 60.0f);
                pos[i].y += vel[i].dy * (1.0f / 60.0f);
            }
        });
    }
}

// Walls, with an event per bounce. It writes Vel, which integrate reads, so the sets put
// it before integrate; a declared `after` the other way would be a cycle, since within a
// phase "reads what you wrote last tick" cannot be said. Bounce, then integrate, is fine.
void bounce(TickContext& ctx) {
    for (KindRows k : ctx.view<Pos, Vel>()) {
        Span<Pos> pos = k.col<Pos>();
        Span<Vel> vel = k.col<Vel>();
        parallel_for(ctx, k.rows(), 2048, [&](ChunkContext& cc, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                bool hit = false;
                if (pos[i].x < -kBox) { pos[i].x = -kBox; vel[i].dx = -vel[i].dx; hit = true; }
                if (pos[i].x >  kBox) { pos[i].x =  kBox; vel[i].dx = -vel[i].dx; hit = true; }
                if (pos[i].y < -kBox) { pos[i].y = -kBox; vel[i].dy = -vel[i].dy; hit = true; }
                if (pos[i].y >  kBox) { pos[i].y =  kBox; vel[i].dy = -vel[i].dy; hit = true; }
                if (hit) cc.emit(Bounced{k.handle_at(i)});
            }
        });
    }
}

// Ages everything; every 600th row is destroyed by chunk command and a fresh one spawned.
void churn(TickContext& ctx) {
    u64 tick = ctx.tick;
    for (KindRows k : ctx.view<Age, const Pos>()) {
        Span<Age> age = k.col<Age>();
        Span<const Pos> pos = k.col<const Pos>();
        parallel_for(ctx, k.rows(), 4096, [&](ChunkContext& cc, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                if (++age[i].ticks % 600 == (tick % 600)) {
                    cc.destroy(k.handle_at(i));
                    cc.spawn_later(g_body_kind, Pos{-pos[i].x, -pos[i].y}, Vel{(f32)((i * 7) % 200) - 100.0f, (f32)((i * 13) % 200) - 100.0f}, Age{0});
                }
            }
        });
    }
}

// A reduction over every body: kinetic energy, folded in chunk order.
void energy(TickContext& ctx) {
    Stress& s = ctx.state_as<Stress>();
    for (KindRows k : ctx.view<const Vel>()) {
        Span<const Vel> vel = k.col<const Vel>();
        s.energy = parallel_reduce(ctx, k.rows(), 4096, 0.0, [&](ChunkContext&, u32 b, u32 e) {
            f64 sum = 0.0;
            for (u32 i = b; i < e; ++i) if (k.alive(i)) sum += (f64)(vel[i].dx * vel[i].dx + vel[i].dy * vel[i].dy);
            return sum;
        }, [](f64 a, f64 x) { return a + x; });
    }
}

void on_bounced(TickContext& ctx, Span<const Bounced> hits) { ctx.state_as<Stress>().bounces_handled += hits.len; }

bool stress_startup(Engine& engine, ModuleInstance& self) {
    Stress* s = arena_push<Stress>(self.arena);
    if (!s) return false;
    self.set_state(s);
    s->bodies = g_bodies;
    Table* t = engine_table(engine, g_body_kind);
    Rng rng = Rng::seeded(99, 3);
    for (u32 i = 0; i < g_bodies; ++i)
        t->spawn(Pos{rng.range(-kBox, kBox), rng.range(-kBox, kBox)}, Vel{rng.range(-300, 300), rng.range(-300, 300)}, Age{rng.below(600)});
    return true;
}

constinit const SystemDesc k_systems[] = {
    {.name = ".integrate", .phase = Phase::Sim, .fn = integrate, .reads = ".body.Vel", .writes = ".body.Pos", .state = StateUse::None},
    {.name = ".bounce",    .phase = Phase::Sim, .fn = bounce,    .writes = ".body.Pos, .body.Vel", .emits = "Bounced", .state = StateUse::None},
    {.name = ".churn",     .phase = Phase::Sim, .fn = churn,     .reads = ".body.Pos", .writes = ".body.Age", .state = StateUse::None},
    {.name = ".energy",    .phase = Phase::PostSim, .fn = energy, .reads = ".body.Vel"},
};
constinit const HandlerDesc k_handlers[] = {{.name = ".on_bounced", KV_ON(Bounced, on_bounced), .phase = Phase::PostSim}};

} // namespace

KV_MODULE_DEFINE(example_cpukiller_module) = {
    .name       = "cpukiller",
    .events     = k_events,
    .components = k_components,
    .kinds      = k_kinds,
    .systems    = k_systems,
    .handlers   = k_handlers,
    .startup    = stress_startup,
    .memory     = {.reserve = 64 * 1024},
};

void cpukiller_set_bodies(u32 n) { g_bodies = n; }

} // namespace kv
