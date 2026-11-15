#include "korvid/core/phase.h"
#include "korvid/core/name.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
#include "korvid/core/profile.h"
#include <string.h>

namespace kv {

const char* phase_name(Phase p) {
    switch (p) {
    case Phase::Input:   return "input";
    case Phase::PreSim:  return "presim";
    case Phase::Sim:     return "sim";
    case Phase::PostSim: return "postsim";
    case Phase::Frame:   return "frame";
    case Phase::Work:    return "work";
    default:             return "?";
    }
}

namespace {
thread_local const ScheduledSystem* t_running = nullptr;
} // namespace

const ScheduledSystem* ctx_running(const ScheduledSystem* unit) {
    const ScheduledSystem* was = t_running;
    t_running = unit;
    return was;
}
const ScheduledSystem* ctx_running() { return t_running; }

const char* access_name(Access a) {
    switch (a) {
    case Access::Read:   return "read";
    case Access::Early:  return "read early";
    case Access::Write:  return "write";
    case Access::Append: return "append";
    }
    return "?";
}

const char* early_reads_name(EarlyReadsChoice m) {
    return m == EarlyReadsChoice::Snapshot ? "every kind a snapshot" : m == EarlyReadsChoice::Order ? "every kind ordered" : "as each kind says";
}

bool ScheduledSystem::mode(u32 resource, Access* out) const {
    u32 lo = 0, hi = access.len;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (access[mid].resource < resource) lo = mid + 1;
        else hi = mid;
    }
    if (lo == access.len || access[lo].resource != resource) return false;
    *out = access[lo].mode;
    return true;
}

void ScheduledSystem::free_buffers() {
    declared.free();
    access.free();
    commands.bytes.free();
    for (Table& t : early) t.columns.free();
    early.free();
    cursors.free();
}

u32 PhaseList::system_count() const {
    u32 n = 0;
    for (const Array<ScheduledSystem>& p : systems) n += p.len;
    return n;
}

namespace {

// A unit runs with its own writer and command buffer; attribution follows it onto
// whatever thread it is on. Its batches are what its streams took since it last ran (or,
// read early, up to its phase's start); a handler with nothing in its own is not called.
void run_unit(TickContext ctx, EventBus& events, ScheduledSystem& s) {
    for (ScheduledSystem::Cursor& c : s.cursors) {
        const EventStream& st = events.streams[c.type];
        c.from = c.at;
        c.to   = c.early ? st.phase_start : st.end();
        if (c.to < c.from) c.to = c.from; // a fresh sim moved the stream on
        c.at   = c.to;
    }
    MemScope scope; // a batch that dropped the dead lives in scratch for the call
    const void* data = nullptr;
    u32 count = 0;
    if (s.handler) {
        for (const ScheduledSystem::Cursor& c : s.cursors)
            if (c.type == *s.handler->event) data = events.batch(c.type, c.from, c.to, &count);
        if (!count) return;
    }
    const ScheduledSystem* was = ctx_running(&s);
    ctx.state      = s.module->state;
    ctx.state_type = s.module->state_type;
    ctx.system     = &s;
    ctx.writer     = &events.writer(s.unit);
    ctx.commands   = &s.commands;
    u16 owner_was = mem_owner();
    mem_set_owner((u16)(s.module->index + 1));
    {
        KV_ZONE(s.name());
        if (s.desc) s.desc->fn(ctx);
        else s.handler->fn(ctx, data, count);
    }
    mem_set_owner(owner_was);
    ctx_running(was);
}

struct LevelJob {
    TickContext*      ctx;
    EventBus*         events;
    ScheduledSystem** list;
};

void level_job(u32 chunk, u32, u32, void* user) {
    LevelJob& lj = *(LevelJob*)user;
    run_unit(*lj.ctx, *lj.events, *lj.list[chunk]);
}

// The phase start of its snapshot kinds, copied, and every early reader's view of them
// made over again from the tables as they are now, with the copies where it reads
// early. Nothing else runs.
usize take_snapshots(PhaseList& pl, u32 phase) {
    if (!pl.snapshots[phase].len) return 0;
    KV_ZONE("snapshot");
    usize copied = 0;
    for (KindSnapshot& ks : pl.snapshots[phase]) {
        const Table& t = *ks.table;
        if (ks.liveness) {
            ks.len  = t.slots.len;
            ks.live = t.slots.live;
            ks.gens.resize_fill(ks.len, 0);
            if (ks.len) memcpy(ks.gens.data, t.slots.gens, (usize)ks.len * sizeof(u32));
            copied += (usize)ks.len * sizeof(u32);
        }
        for (KindSnapshot::Rows& r : ks.columns) {
            usize bytes = (usize)t.rows() * r.column->stride;
            r.bytes.resize_fill((u32)bytes, 0);
            if (bytes) memcpy(r.bytes.data, r.column->data, bytes);
            copied += bytes;
        }
    }
    for (ScheduledSystem& s : pl.systems[phase]) {
        for (Table& view : s.early) {
            const KindSnapshot* ks = nullptr;
            for (const KindSnapshot& k : pl.snapshots[phase]) if (k.table->index == view.index) ks = &k;
            const Table& t = *ks->table;
            Array<Column> cols = view.columns;
            view = t;
            view.columns = cols;
            memcpy(view.columns.data, t.columns.data, (usize)t.columns.len * sizeof(Column));
            if (ks->liveness) {
                view.slots.gens = (u32*)ks->gens.data;
                view.slots.len  = ks->len;
                view.slots.live = ks->live;
            }
            for (const KindSnapshot::Rows& r : ks->columns) {
                Access m;
                if (s.mode(r.column->resource, &m) && m == Access::Early)
                    view.columns[(u32)(r.column - t.columns.data)].data = (u8*)r.bytes.data;
            }
        }
    }
    return copied;
}

// Before a level runs: the streams its units consume as they stand, merged from this
// phase's units if they are not yet. Every emitter of them in the phase is done: the
// schedule put the consumers after them.
void merge_for_level(PhaseList& pl, EventBus& events, u32 phase, u32 i, u32 j) {
    for (u32 k = i; k < j; ++k)
        for (const ScheduledSystem::Cursor& c : pl.systems[phase][k].cursors)
            if (!c.early && (c.type >= events.streams.len || !events.streams[c.type].merged))
                events.merge(c.type, pl.first_unit[phase], pl.last_unit[phase]);
}

} // namespace

void PhaseList::run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick, bool paused) {
    TickContext ctx;
    ctx.engine = &engine;
    ctx.events = &events;
    ctx.dt     = dt;
    ctx.tick   = tick;
    ctx.paused = paused;
    ctx.state  = nullptr;
    ctx.state_type = nullptr;
    if (g_prof_on) prof_tick(tick);
    KV_ZONE("tick");
    usize snapshot_bytes = 0;
    // Handlers run whenever they have events; a system on a paused tick only if Always.
    auto runs = [&](const ScheduledSystem& s) { return !paused || !s.desc || s.desc->runs == RunWhen::Always; };
    auto main_thread = [](const ScheduledSystem& s) { return s.desc && s.desc->main_thread; };
    // The tick start: the timers, then what came from outside the schedule and what was
    // held for this tick.
    {
        KV_ZONE("tick start");
        if (!paused) {
            u32 fired = engine.timers.fired;
            engine.timers.fire(tick, events);
            if (engine.timers.fired != fired) KV_MARK_V("timers fired", engine.timers.fired - fired);
        }
        events.merge_outside(true);
        engine_trace_delivery(engine, tick, (u32)Phase::Count);
    }
    for (u32 p = 0; p < KV_SIM_PHASES; ++p) {
        // A paused tick's number is the sim tick that runs next, which a fresh sim
        // (engine_sim_reset) may just have changed: the units after it see the new one.
        if (paused) ctx.tick = tick = engine.clock.tick_count;
        if ((Phase)p == Phase::PreSim) {
            // Current becomes previous, before anything moves. On a paused tick too: what
            // the Always systems write is then detected as motion by the extract, and the
            // sim columns do not change, so no edge is lost.
            KV_ZONE("prev copy");
            for (Table& t : engine.tables) t.begin_tick();
        }
        events.begin_phase();
        snapshot_bytes += take_snapshots(*this, p); // the snapshot kinds' starting values, if this phase reads any early
        // Level by level: the units of a level have no path between them and run at once
        // on the pool; each level waits for the last.
        Array<ScheduledSystem>& phase = systems[p];
        for (u32 i = 0; i < phase.len;) {
            u32 j = i + 1;
            while (j < phase.len && phase[j].level == phase[i].level) ++j;
            u32 n = j - i;
            merge_for_level(*this, events, p, i, j);
            if (n == 1 || jobs_worker_count() == 0 || engine.hash_systems || paused) {
                for (u32 k = i; k < j; ++k) {
                    if (!runs(phase[k])) continue;
                    run_unit(ctx, events, phase[k]);
                    if (engine.hash_systems && !paused)
                        KV_INFO("hash", "tick %llu after %-24s %016llx", (unsigned long long)tick, phase[k].name(),
                                (unsigned long long)engine_hash_state(engine));
                }
            } else {
                // Units pinned to this thread run here first; the rest go to the pool at once.
                MemScope scope;
                Array<ScheduledSystem*> pooled = array_in<ScheduledSystem*>(mem_scratch());
                for (u32 k = i; k < j; ++k) {
                    if (!runs(phase[k])) continue;
                    if (main_thread(phase[k])) run_unit(ctx, events, phase[k]);
                    else pooled.push(&phase[k]);
                }
                if (pooled.len == 1) run_unit(ctx, events, *pooled[0]);
                else if (pooled.len) { LevelJob lj{&ctx, &events, pooled.data}; jobs_run(pooled.len, 1, level_job, &lj, "level"); }
            }
            i = j;
        }
        // The phase's events, its commands (spawns and destroys issued this phase take
        // effect), and what the boundary says: the commands' Spawned, the engine's writer.
        {
            KV_ZONE("boundary");
            events.merge_all(first_unit[p], last_unit[p]);
            engine_apply_commands(engine);
            events.merge_outside(false);
            engine_trace_delivery(engine, tick, p);
        }
    }
    if (g_prof_on && snapshot_bytes) prof_counter("snapshot KB", snapshot_bytes / 1024);
    if (!paused) {
        KV_ZONE("triggers");
        engine_fire_triggers(engine, tick); // edges this tick, as pending StateEdge events for the next
    }
    // Each stream keeps what a consumer has not reached.
    {
        MemScope scope;
        Array<u64> keep = array_in<u64>(mem_scratch());
        keep.resize_fill(events.streams.len, ~0ull);
        for (const ScheduledSystem* u : by_unit)
            for (const ScheduledSystem::Cursor& c : u->cursors)
                if (c.type < keep.len && c.at < keep[c.type]) keep[c.type] = c.at;
        events.end_tick(Span<const u64>(keep.data, keep.len));
    }
    if ((engine.hash_ticks || engine.hash_systems) && !paused) {
        KV_ZONE("hash");
        engine.last_hash = engine_hash_state(engine);
        KV_INFO("hash", "tick %llu %016llx", (unsigned long long)tick, (unsigned long long)engine.last_hash);
    }
}

void TickContext::more() const { if (engine) engine->work_more = true; }
f64  TickContext::time_left() const {
    if (!engine || engine->work_deadline <= 0.0) return 0.0;
    f64 left = engine->work_deadline - time_now();
    return left > 0.0 ? left : 0.0;
}

void PhaseList::run_cycle(Engine& engine, EventBus& events, Phase phase, f64 dt) {
    u32 p = (u32)phase;
    if (p < KV_SIM_PHASES || p >= (u32)Phase::Count) return;
    TickContext ctx;
    ctx.engine = &engine;
    ctx.events = &events;
    ctx.dt     = dt;
    ctx.tick   = engine.clock.tick_count;
    ctx.paused = false;
    ctx.state  = nullptr;
    ctx.state_type = nullptr;
    auto main_thread = [](const ScheduledSystem& s) { return s.desc && s.desc->main_thread; };
    // What came from outside the schedule since the last boundary.
    events.merge_outside(false);
    events.begin_phase();
    usize snapshot_bytes = take_snapshots(*this, p);
    Array<ScheduledSystem>& units = systems[p];
    for (u32 i = 0; i < units.len;) {
        u32 j = i + 1;
        while (j < units.len && units[j].level == units[i].level) ++j;
        merge_for_level(*this, events, p, i, j);
        if (j - i == 1 || jobs_worker_count() == 0) {
            for (u32 k = i; k < j; ++k) run_unit(ctx, events, units[k]);
        } else {
            MemScope scope;
            Array<ScheduledSystem*> pooled = array_in<ScheduledSystem*>(mem_scratch());
            for (u32 k = i; k < j; ++k) {
                if (main_thread(units[k])) run_unit(ctx, events, units[k]);
                else pooled.push(&units[k]);
            }
            if (pooled.len == 1) run_unit(ctx, events, *pooled[0]);
            else if (pooled.len) { LevelJob lj{&ctx, &events, pooled.data}; jobs_run(pooled.len, 1, level_job, &lj, "level"); }
        }
        i = j;
    }
    {
        KV_ZONE("boundary");
        events.merge_all(first_unit[p], last_unit[p]);
        engine_apply_commands(engine);
        events.merge_outside(false);
        engine_trace_delivery(engine, ctx.tick, p);
    }
    if (g_prof_on && snapshot_bytes) prof_counter("snapshot KB", snapshot_bytes / 1024);
    MemScope scope;
    Array<u64> keep = array_in<u64>(mem_scratch());
    keep.resize_fill(events.streams.len, ~0ull);
    for (const ScheduledSystem* u : by_unit)
        for (const ScheduledSystem::Cursor& c : u->cursors)
            if (c.type < keep.len && c.at < keep[c.type]) keep[c.type] = c.at;
    events.trim(Span<const u64>(keep.data, keep.len));
}

void PhaseList::free() {
    for (Array<ScheduledSystem>& p : systems) {
        for (ScheduledSystem& s : p) s.free_buffers();
        p.free();
    }
    for (Array<KindSnapshot>& p : snapshots) {
        for (KindSnapshot& k : p) {
            k.gens.free();
            for (KindSnapshot::Rows& r : k.columns) r.bytes.free();
            k.columns.free();
        }
        p.free();
    }
    by_unit.free();
}

// ---- the accessors behind TickContext ---------------------------------------------

namespace {

const char* system_name(const TickContext& ctx) { return ctx.system ? ctx.system->name() : "(no system)"; }

// Declared access to a resource, by the unit's one expanded list: the same list the
// schedule was built from. A read is any mode but append; a write is a write. Outside the
// schedule (no system) everything is allowed. Ship builds skip this.
bool allowed(const TickContext& ctx, u32 resource, bool write) {
#if KV_DEV
    if (!ctx.system) return true;
    Access m;
    if (!ctx.system->mode(resource, &m)) return false;
    return write ? m == Access::Write : m != Access::Append;
#else
    KV_UNUSED(ctx); KV_UNUSED(resource); KV_UNUSED(write);
    return true;
#endif
}

void deny(const TickContext& ctx, const char* what, const char* name, bool write) {
    KV_ERROR("engine", "system '%s' %s %s '%s' without declaring it", system_name(ctx),
             write ? "writes" : "reads", what, name);
    KV_VERIFY(!"undeclared access: see the log");
}

// The unit's view of a kind: for a snapshot kind it reads early, the kind as the phase
// found it (liveness, and the columns it reads early); the table itself
// otherwise.
Table* view_of(const TickContext& ctx, Table* t) {
    if (!t || !ctx.system || !ctx.system->early.len) return t;
    for (Table& v : ((ScheduledSystem*)ctx.system)->early)
        if (v.index == t->index) return &v;
    return t;
}

Column* checked_column(TickContext& ctx, Column* c, const void* type, bool write) {
    if (c->desc->type != type) {
        KV_ERROR("engine", "system '%s' accesses column '%s' as another type", system_name(ctx), c->resource_name);
        KV_VERIFY(!"column type mismatch: see the log");
        return nullptr;
    }
    if (!allowed(ctx, c->resource, write)) { deny(ctx, "column", c->resource_name, write); return nullptr; }
    return c;
}

// Ids from the context carry the engine's serial in their top byte; ids from anywhere
// else (a table's own) have none. One from another engine is refused.
constexpr u32 k_id_bits = 24;

u32 tag_id(const TickContext& ctx, u32 id) { return id ? id | ctx.engine->serial << k_id_bits : 0; }

u32 untag_id(const TickContext& ctx, u32 id) {
    u32 serial = id >> k_id_bits;
    if (serial && serial != ctx.engine->serial) {
        KV_ERROR("engine", "system '%s' uses an id from another engine (kept in a static across engines?)", system_name(ctx));
        KV_VERIFY(!"id from another engine");
        return 0;
    }
    return id & ((1u << k_id_bits) - 1);
}

} // namespace

const char* ctx_qualify(TickContext& ctx, const char* name);

Table* ctx_kind(TickContext& ctx, const char* kind, bool write) {
    kind = ctx_qualify(ctx, kind);
    Table* t = engine_table_now(*ctx.engine, engine_kind_id(*ctx.engine, kind));
    if (!t) { KV_ERROR("engine", "system '%s': unknown kind '%s'", system_name(ctx), kind); KV_VERIFY(!"unknown kind"); return nullptr; }
    if (!allowed(ctx, t->resource, write)) { deny(ctx, "kind", kind, write); return nullptr; }
    return write ? t : view_of(ctx, t);
}

Table* ctx_kind(TickContext& ctx, u32 kind_id, bool write) {
    Table* t = engine_table_now(*ctx.engine, kind_id);
    if (!t) { KV_ERROR("engine", "system '%s': unknown kind id %u", system_name(ctx), kind_id); KV_VERIFY(!"unknown kind"); return nullptr; }
    if (!allowed(ctx, t->resource, write)) { deny(ctx, "kind", t->name, write); return nullptr; }
    return write ? t : view_of(ctx, t);
}

Column* ctx_column(TickContext& ctx, u32 resource_id, const void* type, bool write, u32* rows) {
    resource_id = untag_id(ctx, resource_id);
    const ResourceEntry* r = resource_id && resource_id <= ctx.engine->resources.len ? &ctx.engine->resources[resource_id - 1] : nullptr;
    if (r && r->cls == ResourceClass::Column) {
        Table* t = view_of(ctx, &ctx.engine->tables[r->table - 1]);
        for (Column& c : t->columns)
            if (c.resource == resource_id) {
                *rows = t->rows();
                return checked_column(ctx, &c, type, write);
            }
    }
    KV_ERROR("engine", "system '%s': resource %u is not a column", system_name(ctx), resource_id);
    KV_VERIFY(!"unknown column");
    return nullptr;
}

Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows) {
    column = ctx_qualify(ctx, column);
    u32 id = engine_resource_id(*ctx.engine, column);
    if (!id) { KV_ERROR("engine", "system '%s': unknown column '%s'", system_name(ctx), column); KV_VERIFY(!"unknown column"); return nullptr; }
    return ctx_column(ctx, id, type, write, rows);
}

Column* ctx_column_in(TickContext& ctx, Table& table, u32 component_id, const void* type, bool write) {
    Column* c = table.column(component_id);
    if (!c) return nullptr; // the view said the kind has it; a stray call just gets nothing
    return checked_column(ctx, c, type, write);
}

void* ctx_resource(TickContext& ctx, u32 resource_id, const void* type, bool write) {
    resource_id = untag_id(ctx, resource_id);
    if (!resource_id || resource_id > ctx.engine->resources.len) { KV_ERROR("engine", "system '%s': unknown resource id %u", system_name(ctx), resource_id); KV_VERIFY(!"unknown resource"); return nullptr; }
    ResourceEntry& r = ctx.engine->resources[resource_id - 1];
    if (!allowed(ctx, resource_id, write)) { deny(ctx, "resource", r.name, write); return nullptr; }
    if (!r.ptr) { KV_ERROR("engine", "system '%s': resource '%s' is declared but nothing is published under it", system_name(ctx), r.name); return nullptr; }
    if (r.type != type) { KV_ERROR("engine", "system '%s' reaches resource '%s' as another type", system_name(ctx), r.name); KV_VERIFY(!"resource type mismatch: see the log"); return nullptr; }
    return r.ptr;
}

bool ctx_state_allowed(const TickContext& ctx, bool write) {
#if KV_DEV
    if (!ctx.system || !ctx.system->module) return true;
    if (allowed(ctx, ctx.system->module->state_resource, write)) return true;
    KV_ERROR("engine", "system '%s' %s its module's state, but declares state = %s", system_name(ctx), write ? "writes" : "reads",
             ctx.system->desc && ctx.system->desc->state == StateUse::Read ? "Read (state_as<const T> reads it)" : "None");
    KV_VERIFY(!"undeclared state access: see the log");
    return false;
#else
    KV_UNUSED(ctx); KV_UNUSED(write);
    return true;
#endif
}

bool ctx_alive(TickContext& ctx, Handle h) {
    Table* t = engine_table_of_now(*ctx.engine, h);
    if (!t) return false;
    if (!allowed(ctx, t->alive_resource, false)) { deny(ctx, "liveness", t->name, false); return false; }
    return view_of(ctx, t)->valid(h);
}

void ctx_emit_check(const TickContext& ctx, u32 type_id) {
#if KV_DEV
    if (!ctx.system || !type_id || type_id >= ctx.engine->streams.len) return;
    u32 r = ctx.engine->streams[type_id];
    Access m;
    if (ctx.system->mode(r, &m) && m == Access::Append) return;
    KV_ERROR("engine", "system '%s' emits '%s' without declaring it in `emits`", system_name(ctx), ctx.engine->resources[r - 1].name);
    KV_VERIFY(!"undeclared emit: see the log");
#else
    KV_UNUSED(ctx); KV_UNUSED(type_id);
#endif
}

const void* ctx_received(TickContext& ctx, u32 type_id, u32* count) {
    *count = 0;
    if (ctx.system)
        for (const ScheduledSystem::Cursor& c : ctx.system->cursors)
            if (c.type == type_id) return ctx.events->batch(type_id, c.from, c.to, count);
    const char* name = type_id && type_id < ctx.engine->streams.len ? ctx.engine->resources[ctx.engine->streams[type_id] - 1].name : "?";
    KV_ERROR("engine", "system '%s' reads the '%s' it received without declaring it in `reads`", system_name(ctx), name);
    KV_VERIFY(!"undeclared received events: see the log");
    return nullptr;
}

Table* ctx_rows(TickContext& ctx, u32 kind_id) {
    Table* t = engine_table_now(*ctx.engine, kind_id);
    if (!t) { KV_ERROR("engine", "system '%s': unknown kind id %u", system_name(ctx), kind_id); KV_VERIFY(!"unknown kind"); return nullptr; }
    if (!allowed(ctx, t->alive_resource, false)) { deny(ctx, "liveness", t->name, false); return nullptr; }
    return view_of(ctx, t);
}

bool ctx_may_append(TickContext& ctx, u32 resource_id) {
    resource_id = untag_id(ctx, resource_id);
    const ResourceEntry* r = resource_id && resource_id <= ctx.engine->resources.len ? &ctx.engine->resources[resource_id - 1] : nullptr;
    if (!r || r->cls != ResourceClass::Append) {
        KV_ERROR("engine", "system '%s' appends to '%s', which is not an append resource", system_name(ctx), r ? r->name : "?");
        KV_VERIFY(!"append to a resource without apply");
        return false;
    }
#if KV_DEV
    Access m;
    if (ctx.system && !(ctx.system->mode(resource_id, &m) && (m == Access::Append || m == Access::Write))) {
        KV_ERROR("engine", "system '%s' appends to '%s' without declaring it in `appends`", system_name(ctx), r->name);
        KV_VERIFY(!"undeclared append: see the log");
        return false;
    }
#endif
    return true;
}

bool ctx_append(TickContext& ctx, u32 resource_id, const void* record, u32 size) {
    if (!ctx_may_append(ctx, resource_id)) return false;
    command_append(ctx.commands ? *ctx.commands : ctx.engine->commands, untag_id(ctx, resource_id), record, size);
    return true;
}

// A name starting with '.' is relative to the running unit's module, as in the tables.
const char* ctx_qualify(TickContext& ctx, const char* name) {
    if (name && name[0] == '.' && ctx.system && ctx.system->module) return engine_qualify(ctx.system->module->desc->name, name);
    return name;
}

u32 ctx_resource_id(TickContext& ctx, const char* name, bool column) {
    name = ctx_qualify(ctx, name);
    u32 id = engine_resource_id(*ctx.engine, name);
    if (!id) { KV_ERROR("engine", "system '%s': unknown resource '%s'", system_name(ctx), name); KV_VERIFY(!"unknown resource"); return 0; }
    if (column && ctx.engine->resources[id - 1].cls != ResourceClass::Column) {
        KV_ERROR("engine", "system '%s': resource '%s' is not a column", system_name(ctx), name);
        KV_VERIFY(!"not a column");
        return 0;
    }
    return tag_id(ctx, id);
}

u32 TickContext::kind_id(const char* name) {
    name = ctx_qualify(*this, name);
    u32 id = engine_kind_id(*engine, name);
    if (!id) { KV_ERROR("engine", "system '%s': unknown kind '%s'", system_name(*this), name); KV_VERIFY(!"unknown kind"); }
    return id;
}

u32    ctx_table_count(TickContext& ctx) { return ctx.engine->tables.len; }
Table* ctx_table_at(TickContext& ctx, u32 i) { return view_of(ctx, &ctx.engine->tables[i]); }

// Tickets: a unit's are (unit << 20) | its own counter, unique and the same run to run
// whatever thread the unit was on; the engine's buffer and wheel number their own.
// Twenty bits is a million per unit; past that a warning, and uniqueness is best effort.
namespace {

u32 unit_ticket(TickContext& ctx, u32* counter) {
    if (!ctx.system) return 0; // the engine's buffer or the wheel assigns
    ScheduledSystem& s = *(ScheduledSystem*)ctx.system;
    u32 local = ++*counter;
    if (local == (1u << 20)) KV_WARN("engine", "unit '%s' issued a million tickets; they may repeat now", s.name());
    return (s.unit << 20) | (local & 0xFFFFFu);
}

CommandBuffer& buffer_of(TickContext& ctx, CommandBuffer* cb) { return cb ? *cb : ctx.engine->commands; }

} // namespace

u32 ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, const char* kind, u32 component_count, bool ticketed) {
    kind = ctx_qualify(ctx, kind);
    u32 id = engine_kind_id(*ctx.engine, kind);
    if (!id) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind '%s'", system_name(ctx), kind); return 0; }
    return ctx_spawn_begin(ctx, cb, id, component_count, ticketed);
}

u32 ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, u32 kind_id, u32 component_count, bool ticketed) {
    if (!engine_table_now(*ctx.engine, kind_id)) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind id %u", system_name(ctx), kind_id); return 0; }
    CommandBuffer& b = buffer_of(ctx, cb);
    u32 ticket = 0;
    if (ticketed) ticket = ctx.system ? unit_ticket(ctx, &((ScheduledSystem*)ctx.system)->spawn_tickets) : 0;
    else b.next_ticket = 0; // a chunk buffer: never numbers its own
    u32 got = command_spawn_begin(b, kind_id, component_count, ticket);
    return ticketed ? got : 1; // the chunk path only needs "written"
}

void ctx_spawn_component(CommandBuffer* cb, u32 component_id, const void* data, u32 size) {
    KV_ASSERT(cb);
    command_spawn_component(*cb, component_id, data, size);
}

void ctx_destroy(TickContext& ctx, CommandBuffer* cb, Handle h) { command_destroy(buffer_of(ctx, cb), h); }

// ---- attachments and bags -----------------------------------------------------------------------

namespace {

Attachment* checked_store(TickContext& ctx, Table* t, u32 component_id, bool write) {
    Attachment* a = t ? t->attachment(component_id) : nullptr;
    if (!a) {
        const ComponentDesc* cd = engine_component(*ctx.engine, component_id);
        KV_ERROR("engine", "system '%s': kind '%s' does not attach '%s'", system_name(ctx), t ? t->name : "?", cd ? cd->name : "?");
        return nullptr;
    }
    if (!allowed(ctx, a->resource, write)) { deny(ctx, "attachments", a->resource_name, write); return nullptr; }
    return a;
}

} // namespace

void* ctx_attached(TickContext& ctx, Handle h, u32 component_id, const void* type, bool write) {
    Table* t = engine_table_of_now(*ctx.engine, h);
    if (!t || !t->valid(h)) return nullptr;
    Attachment* a = checked_store(ctx, t, component_id, write);
    if (!a || a->desc->type != type) return nullptr;
    return t->attached(h, component_id);
}

void* ctx_attach(TickContext& ctx, Handle h, u32 component_id, const void* type, const void* data) {
    Table* t = engine_table_of_now(*ctx.engine, h);
    if (!t || !t->valid(h)) return nullptr;
    Attachment* a = checked_store(ctx, t, component_id, true);
    if (!a || a->desc->type != type) return nullptr;
    return t->attach(h, component_id, data);
}

bool ctx_detach(TickContext& ctx, Handle h, u32 component_id) {
    Table* t = engine_table_of_now(*ctx.engine, h);
    if (!t || !t->valid(h)) return false;
    return checked_store(ctx, t, component_id, true) && t->detach(h, component_id);
}

Table* ctx_attachments(TickContext& ctx, u32 kind_id, u32 component_id, bool write) {
    Table* t = engine_table_now(*ctx.engine, kind_id);
    return checked_store(ctx, t, component_id, write) ? t : nullptr;
}

Table* ctx_props(TickContext& ctx, Handle h, bool write) {
    Table* t = engine_table_of_now(*ctx.engine, h);
    if (!t || !t->valid(h)) return nullptr;
    if (!t->props_resource) { KV_ERROR("engine", "system '%s': kind '%s' has no property bags", system_name(ctx), t->name); return nullptr; }
    if (!allowed(ctx, t->props_resource, write)) { deny(ctx, "bags", t->props_resource_name, write); return nullptr; }
    return t;
}

namespace {

bool set_prop(TickContext& ctx, Handle h, const char* key, const PropValue& v) {
    Table* t = ctx_props(ctx, h, true);
    return t && key && key[0] && t->prop_set(h, name_intern(key), v);
}

const PropValue* get_prop(TickContext& ctx, Handle h, const char* key) {
    Table* t = ctx_props(ctx, h, false);
    Name n = key ? name_lookup(key) : Name{};
    return t && !n.is_null() ? t->prop(h, n) : nullptr;
}

} // namespace

bool TickContext::prop_set_int(Handle h, const char* key, i64 v) { PropValue p; p.type = PropType::Int; p.i = v; return set_prop(*this, h, key, p); }
bool TickContext::prop_set_float(Handle h, const char* key, f64 v) { PropValue p; p.type = PropType::Float; p.f = v; return set_prop(*this, h, key, p); }
bool TickContext::prop_set_bool(Handle h, const char* key, bool v) { PropValue p; p.type = PropType::Bool; p.i = v ? 1 : 0; return set_prop(*this, h, key, p); }
bool TickContext::prop_set_str(Handle h, const char* key, const char* v) { PropValue p; p.type = PropType::String; p.bits = name_intern(v ? v : "").hash; return set_prop(*this, h, key, p); }
bool TickContext::prop_set_handle(Handle h, const char* key, Handle v) { PropValue p; p.type = PropType::Handle; p.bits = v.bits; return set_prop(*this, h, key, p); }
bool TickContext::prop_remove(Handle h, const char* key) {
    Table* t = ctx_props(*this, h, true);
    Name n = key ? name_lookup(key) : Name{};
    return t && !n.is_null() && t->prop_remove(h, n);
}
bool TickContext::prop_has(Handle h, const char* key) { return get_prop(*this, h, key) != nullptr; }
i64 TickContext::prop_int(Handle h, const char* key, i64 dflt) {
    const PropValue* p = get_prop(*this, h, key);
    return !p ? dflt : p->type == PropType::Int || p->type == PropType::Bool ? p->i : p->type == PropType::Float ? (i64)p->f : dflt;
}
f64 TickContext::prop_float(Handle h, const char* key, f64 dflt) {
    const PropValue* p = get_prop(*this, h, key);
    return !p ? dflt : p->type == PropType::Float ? p->f : p->type == PropType::Int ? (f64)p->i : dflt;
}
bool TickContext::prop_bool(Handle h, const char* key, bool dflt) {
    const PropValue* p = get_prop(*this, h, key);
    return p && (p->type == PropType::Bool || p->type == PropType::Int) ? p->i != 0 : dflt;
}
const char* TickContext::prop_str(Handle h, const char* key, const char* dflt) {
    const PropValue* p = get_prop(*this, h, key);
    return p && p->type == PropType::String ? name_str(Name{p->bits}) : dflt;
}
Handle TickContext::prop_handle(Handle h, const char* key) {
    const PropValue* p = get_prop(*this, h, key);
    return p && p->type == PropType::Handle ? Handle{p->bits} : Handle{};
}

void ctx_name(TickContext& ctx, CommandBuffer* cb, Handle h, const char* name) {
    command_name(buffer_of(ctx, cb), h, name && name[0] ? name_intern(name).hash : 0);
}

bool ctx_cancel(TickContext& ctx, u32 ticket) { return ctx.engine->timers.cancel(ticket); }

void ctx_destroy_all(TickContext& ctx, CommandBuffer* cb, u32 kind_id) {
    if (!engine_table_now(*ctx.engine, kind_id)) { KV_ERROR("engine", "system '%s': destroy_all of unknown kind id %u", system_name(ctx), kind_id); return; }
    command_destroy_all(buffer_of(ctx, cb), kind_id);
}

u32 ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick) {
    if (type_id == 0) { KV_ERROR("engine", "system '%s' schedules an unregistered event type", system_name(ctx)); KV_VERIFY(!"unregistered event"); return 0; }
    u32 ticket = ctx.system ? unit_ticket(ctx, &((ScheduledSystem*)ctx.system)->timer_tickets) : 0;
    return ctx.engine->timers.schedule(tick, type_id, size, event, system_name(ctx), ticket);
}

const char* ctx_unit_name(TickContext& ctx) { return ctx.system ? ctx.system->name() : "parallel_for"; }

ParallelBuffers ctx_parallel_begin(TickContext& ctx, u32 chunks) {
    ParallelBuffers pb;
    pb.chunks   = chunks;
    pb.writers  = alloc_items<EventWriter>(mem_scratch(), chunks);
    pb.commands = alloc_items<CommandBuffer>(mem_scratch(), chunks);
    const char* emitter = ctx.writer ? ctx.writer->emitter : "";
    for (u32 i = 0; i < chunks; ++i) {
        pb.writers[i]  = EventWriter{};  // inner arrays on the general allocator: chunks run on any thread
        pb.writers[i].emitter = emitter;
        pb.commands[i] = CommandBuffer{};
        pb.commands[i].next_ticket = 0;
    }
    return pb;
}

// Chunk order, whatever ran where. The inner arrays go back to the general allocator.
void ctx_parallel_end(TickContext& ctx, ParallelBuffers& pb) {
    EventWriter& w = ctx.writer ? *ctx.writer : ctx.events->writer(0);
    CommandBuffer& c = buffer_of(ctx, ctx.commands);
    for (u32 i = 0; i < pb.chunks; ++i) {
        w.append(pb.writers[i]);
        pb.writers[i].free();
        CommandBuffer& cc = pb.commands[i];
        if (cc.count) {
            memcpy(c.bytes.push_n(cc.bytes.len), cc.bytes.data, cc.bytes.len);
            c.count += cc.count;
        }
        cc.bytes.free();
    }
}

} // namespace kv
