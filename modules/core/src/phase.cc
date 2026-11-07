#include "korvid/core/phase.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"
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

void enter(TickContext& ctx, EventBus& events, ScheduledSystem& s) {
    ctx.state      = s.module->state;
    ctx.state_type = s.module->state_type;
    ctx.system     = &s;
    events.set_emitter(s.name());
    mem_set_owner((u16)(s.module->index + 1)); // attribution follows the schedule
}

void leave(EventBus& events) {
    events.set_emitter("");
    mem_set_owner(0);
}

// A phase boundary: at the tick's first one the timers fire; then pending events are
// delivered and the handlers with new events run, in waves, each wave delivering what
// the last emitted, up to the cap. Leftovers wait for the next boundary.
void boundary(Engine& engine, EventBus& events, TickContext& ctx, u64 tick, bool tick_start) {
    if (tick_start) engine.timers.fire(tick, events);
    events.deliver(tick_start);
    engine_trace_delivery(engine, tick, 0);
    for (u32 wave = 0;; ++wave) {
        ctx.wave = wave;
        for (ScheduledSystem& h : engine.handlers) {
            u32 type = *h.handler->event;
            u32 n = events.new_count(type);
            if (!n) continue;
            enter(ctx, events, h);
            h.handler->fn(ctx, events.new_data(type), n);
            leave(events);
        }
        engine_apply_commands(engine);
        if (events.pending_now() == 0) break;
        if (wave + 1 >= engine.wave_cap) { ++engine.waves_capped; break; }
        events.deliver(false);
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
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        boundary(engine, events, ctx, tick, p == 0);
        if ((Phase)p == Phase::PreSim)
            for (Table& t : engine.tables) t.begin_tick(); // current becomes previous, before anything moves
        for (ScheduledSystem& s : systems[p]) {
            enter(ctx, events, s);
            s.desc->fn(ctx);
            leave(events);
            if (engine.hash_systems)
                KV_INFO("hash", "tick %llu after %-24s %016llx", (unsigned long long)tick, s.desc->name,
                        (unsigned long long)engine_hash_state(engine));
        }
        engine_apply_commands(engine); // spawns and destroys issued this phase take effect
    }
    engine_fire_triggers(engine, tick); // edges this tick, as pending StateEdge events for the next
    events.end_tick();
    if (engine.hash_ticks || engine.hash_systems) {
        engine.last_hash = engine_hash_state(engine);
        KV_INFO("hash", "tick %llu %016llx", (unsigned long long)tick, (unsigned long long)engine.last_hash);
    }
}

void PhaseList::free() {
    for (Array<ScheduledSystem>& p : systems) {
        for (ScheduledSystem& s : p) { s.reads.free(); s.writes.free(); }
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

u32 ctx_spawn_begin(TickContext& ctx, const char* kind, u32 component_count) {
    u32 id = engine_kind_id(*ctx.engine, kind);
    if (!id) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind '%s'", system_name(ctx), kind); return 0; }
    return command_spawn_begin(ctx.engine->commands, id, component_count);
}

u32 ctx_spawn_begin(TickContext& ctx, u32 kind_id, u32 component_count) {
    if (!engine_table(*ctx.engine, kind_id)) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind id %u", system_name(ctx), kind_id); return 0; }
    return command_spawn_begin(ctx.engine->commands, kind_id, component_count);
}

void ctx_spawn_component(TickContext& ctx, u32 component_id, const void* data, u32 size) {
    command_spawn_component(ctx.engine->commands, component_id, data, size);
}

void ctx_destroy(TickContext& ctx, Handle h) { command_destroy(ctx.engine->commands, h); }

u32 ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick) {
    if (type_id == 0) { KV_ERROR("engine", "system '%s' schedules an unregistered event type", system_name(ctx)); KV_VERIFY(!"unregistered event"); return 0; }
    return ctx.engine->timers.schedule(tick, type_id, size, event, system_name(ctx));
}

} // namespace kv
