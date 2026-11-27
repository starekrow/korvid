// The brawl (brawl.h): three versions over bare arrays, and the engine's.
#include "brawl.h"
#include "korvid/core/atomic.h"
#include "korvid/core/job.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"

namespace kv {

// ---- the bare versions -------------------------------------------------------------------

struct BareArena {
    static constexpr u32 kLocks = 4096;
    u32      count = 0;
    i32*     hp = nullptr;      // every fighter's health: the state the threads fight over
    u32*     kills = nullptr;   // a fighter's own, written only by the chunk that has its row
    OsMutex* locks = nullptr;   // for Locked: a victim's, by index
    u64      tick = 0;
    // What the attackers say happened (atomic: a chunk adds its own sums when it ends).
    u64      dealt = 0, deaths = 0, loot = 0;
    // What the replacement pass found (atomic, likewise): the books' other side.
    u64      replaced = 0, made_good = 0;
};

namespace {

constexpr u32 kBareGrain = 1024;

// Unsynchronized on purpose: the race is the exhibit, so ThreadSanitizer is told to look
// away from this one function and nowhere else.
KV_NO_SANITIZE_THREAD void bare_swing_naive(u32, u32 begin, u32 end, void* user) {
    BareArena& a = *(BareArena*)user;
    u64 dealt = 0, deaths = 0;
    for (u32 i = begin; i < end; ++i) {
        BrawlSwing s = brawl_swing(i, a.count, a.tick);
        i32 h = a.hp[s.target];
        if (h <= 0) continue; // already dead this tick
        h -= (i32)s.damage;
        a.hp[s.target] = h;
        dealt += s.damage;
        if (h <= 0) { ++deaths; ++a.kills[i]; }
    }
    atomic_add_fetch(&a.dealt, dealt, MO_RELAXED);
    atomic_add_fetch(&a.deaths, deaths, MO_RELAXED);
    atomic_add_fetch(&a.loot, deaths, MO_RELAXED);
}

void bare_swing_atomic(u32, u32 begin, u32 end, void* user) {
    BareArena& a = *(BareArena*)user;
    u64 dealt = 0, deaths = 0;
    for (u32 i = begin; i < end; ++i) {
        BrawlSwing s = brawl_swing(i, a.count, a.tick);
        if (atomic_load(&a.hp[s.target], MO_RELAXED) <= 0) continue;
        i32 after = atomic_sub_fetch(&a.hp[s.target], (i32)s.damage, MO_RELAXED);
        dealt += s.damage;
        if (after <= 0 && after + (i32)s.damage > 0) { ++deaths; ++a.kills[i]; } // this swing took it through zero
    }
    atomic_add_fetch(&a.dealt, dealt, MO_RELAXED);
    atomic_add_fetch(&a.deaths, deaths, MO_RELAXED);
    atomic_add_fetch(&a.loot, deaths, MO_RELAXED);
}

void bare_swing_locked(u32, u32 begin, u32 end, void* user) {
    BareArena& a = *(BareArena*)user;
    u64 dealt = 0, deaths = 0;
    for (u32 i = begin; i < end; ++i) {
        BrawlSwing s = brawl_swing(i, a.count, a.tick);
        OsMutex* m = &a.locks[s.target % BareArena::kLocks];
        os_mutex_lock(m);
        i32 h = a.hp[s.target];
        if (h > 0) {
            h -= (i32)s.damage;
            a.hp[s.target] = h;
            dealt += s.damage;
            if (h <= 0) { ++deaths; ++a.kills[i]; }
        }
        os_mutex_unlock(m);
    }
    atomic_add_fetch(&a.dealt, dealt, MO_RELAXED);
    atomic_add_fetch(&a.deaths, deaths, MO_RELAXED);
    atomic_add_fetch(&a.loot, deaths, MO_RELAXED);
}

// The dead are replaced: each chunk its own rows, so this pass is right however it runs,
// and it is what counts the deaths that actually happened.
void bare_replace(u32, u32 begin, u32 end, void* user) {
    BareArena& a = *(BareArena*)user;
    u64 replaced = 0, made_good = 0;
    for (u32 i = begin; i < end; ++i) {
        if (a.hp[i] > 0) continue;
        ++replaced;
        made_good += (u64)(kBrawlFullHealth - a.hp[i]);
        a.hp[i] = kBrawlFullHealth;
    }
    atomic_add_fetch(&a.replaced, replaced, MO_RELAXED);
    atomic_add_fetch(&a.made_good, made_good, MO_RELAXED);
}

void bare_run(BareArena* a, bool parallel, JobFn fn, const char* name) {
    if (parallel) { jobs_run(a->count, kBareGrain, fn, a, name); return; }
    for (u32 b = 0, c = 0; b < a->count; b += kBareGrain, ++c) fn(c, b, b + kBareGrain < a->count ? b + kBareGrain : a->count, a);
}

} // namespace

BareArena* bare_arena_create(u32 fighters) {
    Allocator* al = mem_general();
    BareArena* a = alloc_items<BareArena>(al, 1);
    if (!a) return nullptr;
    *a = BareArena{};
    a->count = fighters;
    a->hp = alloc_items<i32>(al, fighters);
    a->kills = alloc_items<u32>(al, fighters);
    a->locks = alloc_items<OsMutex>(al, BareArena::kLocks);
    if (!a->hp || !a->kills || !a->locks) {
        if (a->hp) free_items(al, a->hp, fighters);
        if (a->kills) free_items(al, a->kills, fighters);
        if (a->locks) free_items(al, a->locks, BareArena::kLocks);
        free_items(al, a, 1);
        return nullptr;
    }
    for (u32 i = 0; i < fighters; ++i) { a->hp[i] = kBrawlFullHealth; a->kills[i] = 0; }
    for (u32 i = 0; i < BareArena::kLocks; ++i) os_mutex_init(&a->locks[i]);
    return a;
}

void bare_arena_destroy(BareArena* a) {
    if (!a) return;
    Allocator* al = mem_general();
    for (u32 i = 0; i < BareArena::kLocks; ++i) os_mutex_destroy(&a->locks[i]);
    free_items(al, a->hp, a->count);
    free_items(al, a->kills, a->count);
    free_items(al, a->locks, BareArena::kLocks);
    free_items(al, a, 1);
}

void bare_arena_tick(BareArena* a, BareBrawl how, bool parallel) {
    bare_run(a, parallel, how == BareBrawl::Naive ? bare_swing_naive : how == BareBrawl::Atomic ? bare_swing_atomic : bare_swing_locked, "brawl swing");
    bare_run(a, parallel, bare_replace, "brawl replace");
    ++a->tick;
}

BrawlBooks bare_arena_books(const BareArena* a) {
    BrawlBooks b;
    b.dealt = atomic_load(&a->dealt, MO_RELAXED);
    b.deaths = atomic_load(&a->deaths, MO_RELAXED);
    u64 missing = 0, h = hash_mix(atomic_load(&a->loot, MO_RELAXED));
    for (u32 i = 0; i < a->count; ++i) {
        missing += (u64)(kBrawlFullHealth - a->hp[i]);
        h = hash_mix(h ^ (((u64)(u32)a->hp[i] << 32) | a->kills[i]));
    }
    b.damage_lost = (i64)b.dealt - (i64)(missing + atomic_load(&a->made_good, MO_RELAXED));
    b.extra_deaths = (i64)b.deaths - (i64)atomic_load(&a->replaced, MO_RELAXED);
    b.hash = h;
    return b;
}

// ---- the engine's ------------------------------------------------------------------------
// One system swings: each chunk reads the fighters and emits a Hit for each swing that
// finds its target alive, into the chunk's own writer. One handler takes the Hits, which
// arrive in chunk order whatever threads ran the chunks, and does all three of the writes
// to somebody else's state: it is the only unit that writes a fighter. A victim that died
// earlier in the same tick was destroyed and its slot given to its replacement, so a later
// Hit's handle no longer resolves: that is the whole of "two killers, one victim" here.
// Loot is a spawn by command; a second system sweeps it up two ticks on.

namespace {

struct Health { i32 hp; };
struct Kills { u32 n; };
struct Loot { Handle from; u32 born; };
constinit const FieldDesc kb_health_fields[] = {KV_FIELDS(Health, hp)};
constinit const FieldDesc kb_kills_fields[] = {KV_FIELDS(Kills, n)};
constinit const FieldDesc kb_loot_fields[] = {KV_FIELDS(Loot, from, born)};
constinit const ComponentDesc kb_components[] = {
    KV_COMPONENT(Health, kb_health_fields), KV_COMPONENT(Kills, kb_kills_fields), KV_COMPONENT(Loot, kb_loot_fields),
};
constinit u32 g_fighter_ktable = 0;
constinit u32 g_loot_ktable = 0;
constinit const KTableDesc kb_ktables[] = {
    {.name = ".fighter", .components = "Health, Kills", .capacity = 1u << 20, .id = &g_fighter_ktable},
    {.name = ".loot", .components = "Loot", .capacity = 1u << 18, .id = &g_loot_ktable},
};

struct Hit { Handle attacker, victim; u32 damage; };
constinit const FieldDesc kb_hit_fields[] = {KV_FIELDS(Hit, attacker, victim, damage)};
constinit const EventTypeDesc kb_events[] = {KV_EVENT_EX(Hit, kb_hit_fields, 0)};

// The attackers' side of the books, as the bare versions keep it.
struct BrawlState { u64 dealt, deaths, kills, loot, made_good; u32 fighters; };
u32 g_brawl_fighters = 100000;
BrawlState* g_state = nullptr; // for brawl_books: the last engine's

void swing(TickContext& ctx) {
    u64 tick = ctx.tick;
    for (KTableRows k : ctx.view<const Health>()) {
        u32 rows = k.rows();
        parallel_for(ctx, rows, 1024, [&](ChunkContext& cc, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                if (!k.alive(i)) continue;
                BrawlSwing s = brawl_swing(i, rows, tick);
                if (k.alive(s.target)) cc.emit(Hit{k.handle_at(i), k.handle_at(s.target), s.damage});
            }
        });
    }
}

void on_hit(TickContext& ctx, Span<const Hit> hits) {
    BrawlState& s = ctx.state_as<BrawlState>();
    KTable* fighters = ctx.write_ktable(g_fighter_ktable);
    for (const Hit& h : hits) {
        Health* hp = fighters->get<Health>(h.victim);
        if (!hp) continue; // already dead this tick: the handle is its replacement's slot, a generation on
        hp->hp -= (i32)h.damage;
        s.dealt += h.damage;
        if (hp->hp > 0) continue;
        ++s.deaths;
        s.made_good += (u64)(kBrawlFullHealth - hp->hp);
        if (Kills* k = fighters->get<Kills>(h.attacker)) ++k->n; // unless the killer died first this tick
        ++s.kills;
        fighters->destroy(h.victim);
        fighters->spawn(Health{kBrawlFullHealth}, Kills{0});
        ctx.spawn_later(g_loot_ktable, Loot{h.victim, (u32)ctx.tick});
        ++s.loot;
    }
}

void sweep(TickContext& ctx) {
    u32 tick = (u32)ctx.tick;
    for (KTableRows k : ctx.view<const Loot>()) {
        Span<const Loot> loot = k.col<const Loot>();
        parallel_for(ctx, k.rows(), 1024, [&](ChunkContext& cc, u32 b, u32 e) {
            for (u32 i = b; i < e; ++i)
                if (k.alive(i) && tick - loot[i].born >= 2) cc.destroy(k.handle_at(i));
        });
    }
}

bool brawl_startup(Engine& engine, ModuleInstance& self) {
    BrawlState* s = arena_push<BrawlState>(self.arena);
    if (!s) return false;
    *s = BrawlState{};
    s->fighters = g_brawl_fighters;
    self.set_state(s);
    g_state = s;
    KTable* t = engine_ktable(engine, g_fighter_ktable);
    for (u32 i = 0; i < g_brawl_fighters; ++i) t->spawn(Health{kBrawlFullHealth}, Kills{0});
    return true;
}

constinit const SystemDesc kb_systems[] = {
    {.name = ".swing", .phase = Phase::Sim, .fn = swing, .reads = ".fighter.Health", .emits = "Hit", .state = StateUse::None},
    {.name = ".sweep", .phase = Phase::Sim, .fn = sweep, .reads = ".loot.Loot", .state = StateUse::None},
};
constinit const HandlerDesc kb_handlers[] = {{.name = ".on_hit", KV_ON(Hit, on_hit), .phase = Phase::PostSim, .writes = ".fighter"}};

} // namespace

KV_MODULE_DEFINE(brawl_module) = {
    .name       = "brawl",
    .events     = kb_events,
    .components = kb_components,
    .ktables    = kb_ktables,
    .systems    = kb_systems,
    .handlers   = kb_handlers,
    .startup    = brawl_startup,
    .memory     = {.reserve = 64 * 1024},
};

namespace {
constinit const ModuleDesc* const kb_modules[] = {&brawl_module};
}
Span<const ModuleDesc* const> brawl_modules() { return Span<const ModuleDesc* const>(kb_modules, KV_COUNTOF(kb_modules)); }

void brawl_set_fighters(u32 n) { g_brawl_fighters = n; }

// The other side of the engine's books is read off the ktable, not from the handler: the
// health actually missing, and the fighters actually replaced (a ktable's churn counts
// every spawn and destroy: the first `fighters` are the startup's, then two a death).
BrawlBooks brawl_books(Engine& e) {
    BrawlBooks b;
    if (!g_state) return b;
    const BrawlState& s = *g_state;
    const KTable* t = engine_ktable(e, g_fighter_ktable);
    u64 missing = 0;
    Span<const Health> hp = t->rows_of<Health>();
    for (u32 i = 0; i < hp.len; ++i) if (t->is_alive(i)) missing += (u64)(kBrawlFullHealth - hp[i].hp);
    b.dealt = s.dealt;
    b.deaths = s.deaths;
    b.damage_lost = (i64)s.dealt - (i64)(missing + s.made_good);
    b.extra_deaths = (i64)s.deaths - (i64)((t->churn - s.fighters) / 2);
    b.hash = engine_hash_state(e);
    return b;
}

} // namespace kv
