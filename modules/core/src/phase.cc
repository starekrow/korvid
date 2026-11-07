#include "korvid/core/phase.h"
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
    default:             return "?";
    }
}

u32 PhaseList::system_count() const {
    u32 n = 0;
    for (const Array<ScheduledSystem>& p : systems) n += p.len;
    return n;
}

namespace {

// A unit runs with its own writer and command buffer; attribution follows it onto
// whatever thread it is on.
void run_unit(TickContext ctx, EventBus& events, ScheduledSystem& s, const void* events_data, u32 event_count) {
    ctx.state      = s.module->state;
    ctx.state_type = s.module->state_type;
    ctx.system     = &s;
    ctx.writer     = &events.writer(s.unit);
    ctx.commands   = &s.commands;
    u16 was = mem_owner();
    mem_set_owner((u16)(s.module->index + 1));
    {
        KV_ZONE(s.name());
        if (s.desc) s.desc->fn(ctx);
        else s.handler->fn(ctx, events_data, event_count);
    }
    mem_set_owner(was);
}

struct LevelJob {
    TickContext*      ctx;
    EventBus*         events;
    ScheduledSystem** list;
};

void level_job(u32 chunk, u32, u32, void* user) {
    LevelJob& lj = *(LevelJob*)user;
    run_unit(*lj.ctx, *lj.events, *lj.list[chunk], nullptr, 0);
}

// A phase boundary: at the tick's first one the timers fire; then pending events are
// delivered and the handlers with new events run, in waves, each wave delivering what
// the last emitted, up to the cap. Leftovers wait for the next boundary.
void boundary(Engine& engine, EventBus& events, TickContext& ctx, u64 tick, bool tick_start) {
    KV_ZONE("boundary");
    if (tick_start) {
        u32 fired = engine.timers.fired;
        engine.timers.fire(tick, events);
        if (engine.timers.fired != fired) KV_MARK_V("timers fired", engine.timers.fired - fired);
    }
    events.deliver(tick_start);
    if (events.dropped_last) KV_MARK_V("stale events dropped", events.dropped_last);
    engine_trace_delivery(engine, tick, 0);
    for (u32 wave = 0;; ++wave) {
        ctx.wave = wave;
        for (ScheduledSystem& h : engine.handlers) {
            u32 type = *h.handler->event;
            u32 n = events.new_count(type);
            if (!n) continue;
            run_unit(ctx, events, h, events.new_data(type), n);
        }
        {
            KV_ZONE("apply commands");
            engine_apply_commands(engine);
        }
        if (events.pending_now() == 0) break;
        if (wave + 1 >= engine.wave_cap) { ++engine.waves_capped; KV_MARK_V("wave capped", events.pending_now()); break; }
        events.deliver(false);
        if (events.dropped_last) KV_MARK_V("stale events dropped", events.dropped_last);
        engine_trace_delivery(engine, tick, wave + 1);
    }
    ctx.wave = 0;
}

} // namespace

void PhaseList::run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick) {
    TickContext ctx;
    ctx.engine = &engine;
    ctx.events = &events;
    ctx.dt     = dt;
    ctx.tick   = tick;
    ctx.state  = nullptr;
    ctx.state_type = nullptr;
    if (g_prof_on) prof_tick(tick);
    KV_ZONE("tick");
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        boundary(engine, events, ctx, tick, p == 0);
        if ((Phase)p == Phase::PreSim) {
            KV_ZONE("prev copy");
            for (Table& t : engine.tables) t.begin_tick(); // current becomes previous, before anything moves
        }
        // Level by level: the systems of a level have no path between them and run at
        // once on the pool; each level waits for the last.
        Array<ScheduledSystem>& phase = systems[p];
        for (u32 i = 0; i < phase.len;) {
            u32 j = i + 1;
            while (j < phase.len && phase[j].level == phase[i].level) ++j;
            u32 n = j - i;
            if (n == 1 || jobs_worker_count() == 0 || engine.hash_systems) {
                for (u32 k = i; k < j; ++k) {
                    run_unit(ctx, events, phase[k], nullptr, 0);
                    if (engine.hash_systems)
                        KV_INFO("hash", "tick %llu after %-24s %016llx", (unsigned long long)tick, phase[k].name(),
                                (unsigned long long)engine_hash_state(engine));
                }
            } else {
                // Systems pinned to this thread run here first; the rest go to the pool at once.
                MemScope scope;
                Array<ScheduledSystem*> pooled = array_in<ScheduledSystem*>(mem_scratch());
                for (u32 k = i; k < j; ++k) {
                    if (phase[k].desc->main_thread) run_unit(ctx, events, phase[k], nullptr, 0);
                    else pooled.push(&phase[k]);
                }
                if (pooled.len == 1) run_unit(ctx, events, *pooled[0], nullptr, 0);
                else if (pooled.len) { LevelJob lj{&ctx, &events, pooled.data}; jobs_run(pooled.len, 1, level_job, &lj, "level"); }
            }
            i = j;
        }
        {
            KV_ZONE("apply commands");
            engine_apply_commands(engine); // spawns and destroys issued this phase take effect
        }
    }
    {
        KV_ZONE("triggers");
        engine_fire_triggers(engine, tick); // edges this tick, as pending StateEdge events for the next
    }
    events.end_tick();
    if (engine.hash_ticks || engine.hash_systems) {
        KV_ZONE("hash");
        engine.last_hash = engine_hash_state(engine);
        KV_INFO("hash", "tick %llu %016llx", (unsigned long long)tick, (unsigned long long)engine.last_hash);
    }
}

void PhaseList::free() {
    for (Array<ScheduledSystem>& p : systems) {
        for (ScheduledSystem& s : p) s.free_buffers();
        p.free();
    }
}

// ---- the accessors behind TickContext ---------------------------------------------

namespace {

bool contains(const Array<u32>& ids, u32 id) {
    for (u32 x : ids) if (x == id) return true;
    return false;
}

const char* system_name(const TickContext& ctx) { return ctx.system ? ctx.system->name() : "(no system)"; }

// Declared access to a resource, or to the kind that owns it (write on a kind covers its
// columns). Outside the schedule (no system) everything is allowed. Ship builds skip this.
bool allowed(const TickContext& ctx, u32 resource, u32 kind_resource, bool write) {
#if KV_DEV
    if (!ctx.system) return true;
    const ScheduledSystem& s = *ctx.system;
    if (contains(s.writes, resource) || (kind_resource && contains(s.writes, kind_resource))) return true;
    if (!write && (contains(s.reads, resource) || (kind_resource && contains(s.reads, kind_resource)))) return true;
    return false;
#else
    KV_UNUSED(ctx); KV_UNUSED(resource); KV_UNUSED(kind_resource); KV_UNUSED(write);
    return true;
#endif
}

void deny(const TickContext& ctx, const char* what, const char* name, bool write) {
    KV_ERROR("engine", "system '%s' %s %s '%s' without declaring it", system_name(ctx),
             write ? "writes" : "reads", what, name);
    KV_VERIFY(!"undeclared access: see the log");
}

Column* checked_column(TickContext& ctx, Table& t, Column* c, const void* type, bool write) {
    if (c->desc->type != type) {
        KV_ERROR("engine", "system '%s' accesses column '%s' as another type", system_name(ctx), c->resource_name);
        KV_VERIFY(!"column type mismatch: see the log");
        return nullptr;
    }
    if (!allowed(ctx, c->resource, t.resource, write)) { deny(ctx, "column", c->resource_name, write); return nullptr; }
    return c;
}

} // namespace

Table* ctx_kind(TickContext& ctx, const char* kind, bool write) {
    Table* t = engine_table(*ctx.engine, kind);
    if (!t) { KV_ERROR("engine", "system '%s': unknown kind '%s'", system_name(ctx), kind); KV_VERIFY(!"unknown kind"); return nullptr; }
    if (!allowed(ctx, t->resource, 0, write)) { deny(ctx, "kind", kind, write); return nullptr; }
    return t;
}

Table* ctx_kind(TickContext& ctx, u32 kind_id, bool write) {
    Table* t = engine_table(*ctx.engine, kind_id);
    if (!t) { KV_ERROR("engine", "system '%s': unknown kind id %u", system_name(ctx), kind_id); KV_VERIFY(!"unknown kind"); return nullptr; }
    if (!allowed(ctx, t->resource, 0, write)) { deny(ctx, "kind", t->desc->name, write); return nullptr; }
    return t;
}

Column* ctx_column(TickContext& ctx, u32 resource_id, const void* type, bool write, u32* rows) {
    for (Table& t : ctx.engine->tables)
        for (Column& c : t.columns)
            if (c.resource == resource_id) {
                *rows = t.rows();
                return checked_column(ctx, t, &c, type, write);
            }
    KV_ERROR("engine", "system '%s': resource %u is not a column", system_name(ctx), resource_id);
    KV_VERIFY(!"unknown column");
    return nullptr;
}

Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows) {
    u32 id = engine_resource_id(*ctx.engine, column);
    if (!id) { KV_ERROR("engine", "system '%s': unknown column '%s'", system_name(ctx), column); KV_VERIFY(!"unknown column"); return nullptr; }
    return ctx_column(ctx, id, type, write, rows);
}

Column* ctx_column_in(TickContext& ctx, Table& table, u32 component_id, const void* type, bool write) {
    Column* c = table.column(component_id);
    if (!c) return nullptr; // the view said the kind has it; a stray call just gets nothing
    return checked_column(ctx, table, c, type, write);
}

void* ctx_resource(TickContext& ctx, u32 resource_id, const void* type, bool write) {
    if (!resource_id || resource_id > ctx.engine->resources.len) { KV_ERROR("engine", "system '%s': unknown resource id %u", system_name(ctx), resource_id); KV_VERIFY(!"unknown resource"); return nullptr; }
    ResourceEntry& r = ctx.engine->resources[resource_id - 1];
    if (!allowed(ctx, resource_id, 0, write)) { deny(ctx, "resource", r.name, write); return nullptr; }
    if (!r.ptr) { KV_ERROR("engine", "system '%s': resource '%s' is declared but nothing is published under it", system_name(ctx), r.name); return nullptr; }
    if (r.type != type) { KV_ERROR("engine", "system '%s' reaches resource '%s' as another type", system_name(ctx), r.name); KV_VERIFY(!"resource type mismatch: see the log"); return nullptr; }
    return r.ptr;
}

u32 ctx_resource_id(TickContext& ctx, const char* name, bool column) {
    u32 id = engine_resource_id(*ctx.engine, name);
    if (!id) { KV_ERROR("engine", "system '%s': unknown resource '%s'", system_name(ctx), name); KV_VERIFY(!"unknown resource"); return 0; }
    if (column) {
        bool is_column = false;
        for (const Table& t : ctx.engine->tables)
            for (const Column& c : t.columns) is_column = is_column || c.resource == id;
        if (!is_column) { KV_ERROR("engine", "system '%s': resource '%s' is not a column", system_name(ctx), name); KV_VERIFY(!"not a column"); return 0; }
    }
    return id;
}

u32 TickContext::kind_id(const char* name) {
    u32 id = engine_kind_id(*engine, name);
    if (!id) { KV_ERROR("engine", "system '%s': unknown kind '%s'", system_name(*this), name); KV_VERIFY(!"unknown kind"); }
    return id;
}

u32    ctx_table_count(TickContext& ctx) { return ctx.engine->tables.len; }
Table* ctx_table_at(TickContext& ctx, u32 i) { return &ctx.engine->tables[i]; }

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
    u32 id = engine_kind_id(*ctx.engine, kind);
    if (!id) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind '%s'", system_name(ctx), kind); return 0; }
    return ctx_spawn_begin(ctx, cb, id, component_count, ticketed);
}

u32 ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, u32 kind_id, u32 component_count, bool ticketed) {
    if (!engine_table(*ctx.engine, kind_id)) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind id %u", system_name(ctx), kind_id); return 0; }
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

bool ctx_cancel(TickContext& ctx, u32 ticket) { return ctx.engine->timers.cancel(ticket); }

void ctx_destroy_all(TickContext& ctx, CommandBuffer* cb, u32 kind_id) {
    if (!engine_table(*ctx.engine, kind_id)) { KV_ERROR("engine", "system '%s': destroy_all of unknown kind id %u", system_name(ctx), kind_id); return; }
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
