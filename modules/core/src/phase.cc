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

void PhaseList::run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick) {
    TickContext ctx;
    ctx.engine = &engine;
    ctx.events = &events;
    ctx.dt     = dt;
    ctx.tick   = tick;
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        Array<ScheduledSystem>& phase = systems[p];
        events.deliver(); // phase boundary: pending events become visible
        if ((Phase)p == Phase::PreSim)
            for (Table& t : engine.tables) t.begin_tick(); // current becomes previous, before anything moves
        for (ScheduledSystem& s : phase) {
            ctx.state      = s.module->state;
            ctx.state_type = s.module->state_type;
            ctx.system     = &s;
            mem_set_owner((u16)(s.module->index + 1)); // attribution follows the schedule
            s.desc->fn(ctx);
            if (engine.hash_systems)
                KV_INFO("hash", "tick %llu after %-24s %016llx", (unsigned long long)tick, s.desc->name,
                        (unsigned long long)engine_hash_state(engine));
        }
        mem_set_owner(0);
        engine_apply_commands(engine); // spawns and destroys issued this phase take effect
    }
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

const char* system_name(const TickContext& ctx) { return ctx.system ? ctx.system->desc->name : "(no system)"; }

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

Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows) {
    u32 id = engine_resource_id(*ctx.engine, column);
    for (Table& t : ctx.engine->tables)
        for (Column& c : t.columns)
            if (c.resource == id) {
                *rows = t.rows();
                return checked_column(ctx, t, &c, type, write);
            }
    KV_ERROR("engine", "system '%s': unknown column '%s'", system_name(ctx), column);
    KV_VERIFY(!"unknown column");
    return nullptr;
}

Column* ctx_column_in(TickContext& ctx, Table& table, u32 component_id, const void* type, bool write) {
    Column* c = table.column(component_id);
    if (!c) return nullptr; // the view said the kind has it; a stray call just gets nothing
    return checked_column(ctx, table, c, type, write);
}

u32    ctx_table_count(TickContext& ctx) { return ctx.engine->tables.len; }
Table* ctx_table_at(TickContext& ctx, u32 i) { return &ctx.engine->tables[i]; }

u32 ctx_spawn_begin(TickContext& ctx, const char* kind, u32 component_count) {
    u32 id = engine_kind_id(*ctx.engine, kind);
    if (!id) { KV_ERROR("engine", "system '%s': spawn_later of unknown kind '%s'", system_name(ctx), kind); return 0; }
    return command_spawn_begin(ctx.engine->commands, id, component_count);
}

void ctx_spawn_component(TickContext& ctx, u32 component_id, const void* data, u32 size) {
    command_spawn_component(ctx.engine->commands, component_id, data, size);
}

void ctx_destroy(TickContext& ctx, Handle h) { command_destroy(ctx.engine->commands, h); }

} // namespace kv
