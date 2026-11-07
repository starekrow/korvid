// The registry. Reads the module list once, in order, and turns the declarations into
// ids, a schedule and started instances. Every check here names what clashed.
#include "korvid/core/engine.h"
#include "korvid/core/arena.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

bool names_equal(const char* a, const char* b) { return strcmp(a, b) == 0; }

// The engine's own event types, registered after every module's so module ids are
// unaffected by what the core adds.
constinit const EventTypeDesc k_core_events[] = {KV_EVENT(Spawned)};

bool check_module_names(Span<const ModuleDesc* const> modules) {
    if (modules.len > KV_MEM_MAX_MODULES) {
        KV_ERROR("engine", "%u modules; memory accounting supports %u", modules.len, KV_MEM_MAX_MODULES);
        return false;
    }
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc* m = modules[i];
        if (!m || !m->name || !m->name[0]) {
            KV_ERROR("engine", "module list entry %u is null or unnamed", i);
            return false;
        }
        for (u32 j = 0; j < i; ++j) {
            if (names_equal(modules[j]->name, m->name)) {
                KV_ERROR("engine", "module '%s' appears twice in the module list (entries %u and %u)",
                         m->name, j, i);
                return false;
            }
        }
    }
    return true;
}

// Event ids: module order, then declaration order, starting at 1.
bool register_events(Engine& e, Span<const ModuleDesc* const> modules) {
    u32 next = 1;
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const EventTypeDesc& t : m.events) {
            for (u32 j = 0; j < i; ++j) {
                for (const EventTypeDesc& o : modules[j]->events) {
                    if (names_equal(o.name, t.name)) {
                        KV_ERROR("engine", "event type '%s' declared by both '%s' and '%s'",
                                 t.name, modules[j]->name, m.name);
                        return false;
                    }
                }
            }
        }
        u32 n = event_types_register(m.events, next);
        if (n == 0 && m.events.len) {
            KV_ERROR("engine", "module '%s': event registration failed", m.name);
            return false;
        }
        if (n) next = n;
    }
    for (const EventTypeDesc& t : k_core_events)
        for (const ModuleDesc* m : modules)
            for (const EventTypeDesc& o : m->events)
                if (names_equal(o.name, t.name)) {
                    KV_ERROR("engine", "event type '%s' in module '%s' is the engine's", t.name, m->name);
                    return false;
                }
    u32 n = event_types_register(k_core_events, next);
    if (n == 0) { KV_ERROR("engine", "core event registration failed"); return false; }
    e.event_type_count = n - 1;
    return true;
}

// Component ids: module order, then declaration order, starting at 1. Every schema is
// checked for consistency here, so nothing downstream has to.
bool register_components(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const ComponentDesc& c : m.components) {
            const char* field = nullptr;
            if (const char* problem = component_check(c, &field)) {
                if (field) KV_ERROR("engine", "component '%s' in module '%s': field '%s' %s", c.name, m.name, field, problem);
                else       KV_ERROR("engine", "component '%s' in module '%s' %s", c.name ? c.name : "?", m.name, problem);
                return false;
            }
            if (u32 id = engine_component_id(e, c.name)) {
                KV_ERROR("engine", "component '%s' declared by both '%s' and '%s'", c.name,
                         modules[e.components[id - 1].module]->name, m.name);
                return false;
            }
            if (*c.id != 0) {
                KV_ERROR("engine", "component '%s' in module '%s' is the type already registered as '%s' by '%s'",
                         c.name, m.name, e.components[*c.id - 1].desc->name, modules[e.components[*c.id - 1].module]->name);
                return false;
            }
            e.components.push(ComponentEntry{&c, i});
            *c.id = e.components.len;
        }
    }
    return true;
}

void unregister_components(Engine& e) {
    for (const ComponentEntry& c : e.components) *c.desc->id = 0;
    e.components.free();
}

// Kinds: one table each, in module then declaration order, created here so their
// resources exist before any system references them. Table addresses never move after
// this: the handle registry holds them as contexts.
bool register_kinds(Engine& e, Span<const ModuleDesc* const> modules) {
    u32 count = 0;
    for (const ModuleDesc* m : modules) count += m->kinds.len;
    e.tables.reserve(count);
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const KindDesc& k : m.kinds) {
            if (!k.name || !k.name[0] || strlen(k.name) >= KV_KIND_NAME_MAX) {
                KV_ERROR("engine", "module '%s' has a kind with no name or a name over %u characters", m.name, KV_KIND_NAME_MAX - 1);
                return false;
            }
            if (k.capacity == 0) { KV_ERROR("engine", "kind '%s' in module '%s' has zero capacity", k.name, m.name); return false; }
            if (u32 id = engine_kind_id(e, k.name)) {
                KV_ERROR("engine", "kind '%s' declared by both '%s' and '%s'", k.name, modules[e.tables[id - 1].module]->name, m.name);
                return false;
            }
            if (engine_resource_id(e, k.name)) { KV_ERROR("engine", "kind '%s' in module '%s' collides with a resource of that name", k.name, m.name); return false; }
            const ComponentDesc* comps[32];
            if (k.components.len > KV_COUNTOF(comps)) { KV_ERROR("engine", "kind '%s' has more than %u components", k.name, KV_COUNTOF(comps)); return false; }
            for (u32 c = 0; c < k.components.len; ++c) {
                u32 cid = engine_component_id(e, k.components[c]);
                if (!cid) { KV_ERROR("engine", "kind '%s' in module '%s' uses unknown component '%s'", k.name, m.name, k.components[c]); return false; }
                if (strlen(k.components[c]) >= KV_KIND_NAME_MAX) { KV_ERROR("engine", "component '%s' name is too long for a kind resource", k.components[c]); return false; }
                for (u32 d = 0; d < c; ++d)
                    if (comps[d] == e.components[cid - 1].desc) { KV_ERROR("engine", "kind '%s' lists component '%s' twice", k.name, k.components[c]); return false; }
                comps[c] = e.components[cid - 1].desc;
            }
            mem_set_owner((u16)(i + 1));
            MemTag tag;
            tag.module = (u16)(i + 1);
            Table& t = e.tables.push(Table{});
            bool ok = t.init(k, Span<const ComponentDesc* const>(comps, k.components.len), e.tables.len - 1, i, tag);
            mem_set_owner(0);
            if (!ok) { KV_ERROR("engine", "kind '%s' in module '%s': table creation failed", k.name, m.name); return false; }
            e.resources.push(ResourceEntry{k.name, i});
            t.resource = e.resources.len;
            for (Column& c : t.columns) {
                e.resources.push(ResourceEntry{c.resource_name, i});
                c.resource = e.resources.len;
            }
            if (k.id) *k.id = e.tables.len;
        }
    }
    return true;
}

void free_tables(Engine& e) {
    for (Table& t : e.tables) {
        if (t.desc && t.desc->id) *t.desc->id = 0;
        mem_set_owner((u16)(t.module + 1));
        t.free();
        mem_set_owner(0);
    }
    e.tables.free();
}

bool register_resources(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        for (const ResourceDesc& r : modules[i]->resources) {
            if (u32 id = engine_resource_id(e, r.name)) {
                KV_ERROR("engine", "resource '%s' declared by both '%s' and '%s'", r.name,
                         modules[e.resources[id - 1].module]->name, modules[i]->name);
                return false;
            }
            e.resources.push(ResourceEntry{r.name, i});
        }
    }
    return true;
}

// Names to ids. "*.Component" is that column in every kind, and must exist somewhere.
bool resolve_resource_refs(const Engine& e, const ModuleDesc& m, const SystemDesc& s,
                           Span<const char* const> refs, const char* kind, Array<u32>& out) {
    for (const char* name : refs) {
        if (name[0] == '*' && name[1] == '.') {
            u32 cid = engine_component_id(e, name + 2);
            bool any = false;
            for (const Table& t : e.tables)
                if (const Column* c = t.column(cid)) { out.push(c->resource); any = true; }
            if (!any) {
                KV_ERROR("engine", "system '%s' in module '%s' %s '%s' but no kind has component '%s'",
                         s.name, m.name, kind, name, name + 2);
                return false;
            }
            continue;
        }
        u32 id = engine_resource_id(e, name);
        if (!id) {
            KV_ERROR("engine", "system '%s' in module '%s' %s unknown resource '%s'",
                     s.name, m.name, kind, name);
            return false;
        }
        out.push(id);
    }
    return true;
}

bool overlaps(Span<const char* const> a, Span<const char* const> b) {
    for (const char* x : a)
        for (const char* y : b)
            if (names_equal(x, y)) return true;
    return false;
}

// Schedule: for each phase, module order, then declaration order.
bool build_schedule(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const SystemDesc& s : m.systems) {
            if (!s.name || !s.fn || (u32)s.phase >= (u32)Phase::Count) {
                KV_ERROR("engine", "module '%s' has a system with no name, no function or a bad phase", m.name);
                return false;
            }
            ScheduledSystem sched{&s, &e.modules[i], {}, {}};
            if (!resolve_resource_refs(e, m, s, s.reads, "reads", sched.reads) ||
                !resolve_resource_refs(e, m, s, s.writes, "writes", sched.writes)) {
                sched.reads.free();
                sched.writes.free();
                return false;
            }

            Array<ScheduledSystem>& phase = e.phases.systems[(u32)s.phase];
            for (const ScheduledSystem& other : phase) {
                if (names_equal(other.desc->name, s.name)) {
                    KV_ERROR("engine", "system '%s' declared by both '%s' and '%s' in phase '%s'",
                             s.name, other.module->desc->name, m.name, phase_name(s.phase));
                    sched.reads.free();
                    sched.writes.free();
                    return false;
                }
                // Enforcement is stubbed in slice 1: conflicts are reported, not rejected.
                const SystemDesc& o = *other.desc;
                if (overlaps(o.writes, s.writes) || overlaps(o.writes, s.reads) || overlaps(o.reads, s.writes))
                    KV_WARN("engine", "systems '%s' and '%s' conflict in phase '%s' (not enforced yet)",
                            o.name, s.name, phase_name(s.phase));
            }
            phase.push(sched);
        }
    }
    return true;
}

void log_components(const Engine& e) {
    for (u32 i = 0; i < e.components.len; ++i) {
        const ComponentDesc& c = *e.components[i].desc;
        KV_INFO("engine", "component %-2u %-16s %3u bytes, %u fields%s%s", i + 1, c.name, c.size, c.fields.len,
                c.is_sim() ? "" : ", cosmetic", c.has_prev() ? ", prev" : "");
    }
}

void log_kinds(const Engine& e) {
    for (const Table& t : e.tables) {
        char line[512];
        u32 at = 0;
        for (const Column& c : t.columns) {
            int n = snprintf(line + at, sizeof line - at, "%s%s", at ? ", " : "", c.desc->name);
            if (n < 0 || at + (u32)n >= sizeof line) break;
            at += (u32)n;
        }
        KV_INFO("engine", "kind %-2u %-16s %u slots: %s", t.index + 1, t.desc->name, t.desc->capacity, at ? line : "(no components)");
    }
}

void log_schedule(const Engine& e) {
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        char line[512];
        u32 at = 0;
        for (const ScheduledSystem& s : e.phases.systems[p]) {
            int n = snprintf(line + at, sizeof line - at, "%s%s", at ? ", " : "", s.desc->name);
            if (n < 0 || at + (u32)n >= sizeof line) break;
            at += (u32)n;
        }
        KV_INFO("engine", "phase %-7s %s", phase_name((Phase)p), at ? line : "(empty)");
    }
}

} // namespace

bool engine_init(Engine& e, Span<const ModuleDesc* const> modules) {
    KV_ASSERT(e.modules.len == 0);
    if (!check_module_names(modules)) return false;

    for (u32 i = 0; i < modules.len; ++i) {
        ModuleInstance inst;
        inst.desc  = modules[i];
        inst.index = i;
        e.modules.push(inst);
    }

    bool ok = register_events(e, modules) && register_components(e, modules) && register_kinds(e, modules) &&
              register_resources(e, modules) && build_schedule(e, modules);
    if (ok) {
        e.events.init(1);
        log_components(e);
        log_kinds(e);
        log_schedule(e);
        for (ModuleInstance& inst : e.modules) {
            const ModuleDesc& m = *inst.desc;
            // Allocations made from here on are the module's: its arena, and anything it
            // takes from the general allocator during startup.
            mem_set_owner((u16)(inst.index + 1));
            MemTag tag;
            tag.module = (u16)(inst.index + 1);
            mem_general_tagged(inst.general, tag, m.name);
            if (m.memory.reserve && !arena_create(inst.arena, m.memory.reserve, m.name, tag)) {
                ok = false;
                break;
            }
            if (m.startup && !m.startup(e, inst)) {
                KV_ERROR("engine", "module '%s' failed to start", m.name);
                ok = false;
                break;
            }
            inst.started = true;
            KV_INFO("engine", "module '%s' up", m.name);
        }
        mem_set_owner(0);
    }
    if (!ok) engine_shutdown(e);
    return ok;
}

void engine_shutdown(Engine& e) {
    for (u32 i = e.modules.len; i-- > 0;) {
        ModuleInstance& inst = e.modules[i];
        mem_set_owner((u16)(inst.index + 1));
        if (inst.started && inst.desc->shutdown) inst.desc->shutdown(e, inst);
        if (inst.started) KV_INFO("engine", "module '%s' down", inst.desc->name);
        if (inst.arena.base)
            KV_INFO("engine", "module '%s' arena: %llu bytes used, %llu peak, %llu committed of %llu",
                    inst.desc->name, (unsigned long long)inst.arena.used, (unsigned long long)inst.arena.peak,
                    (unsigned long long)inst.arena.committed, (unsigned long long)inst.arena.reserved);
        inst.started    = false;
        inst.state      = nullptr;
        inst.state_type = nullptr;
        arena_destroy(inst.arena); // the module's state goes with it
        mem_set_owner(0);
    }
    e.commands.bytes.free();
    e.commands.count = 0;
    free_tables(e); // after every module is down: nothing holds an entity any more
    for (const ModuleInstance& inst : e.modules) event_types_unregister(inst.desc->events);
    event_types_unregister(k_core_events);
    unregister_components(e);
    e.phases.free();
    e.events.free();
    e.resources.free();
    // The leak report runs in mem_shutdown, after tables that outlive the engine are
    // gone; it wants module names, which are constinit and stay valid.
    const char* names[KV_MEM_MAX_MODULES];
    u32 count = e.modules.len < KV_MEM_MAX_MODULES ? e.modules.len : KV_MEM_MAX_MODULES;
    for (u32 i = 0; i < count; ++i) names[i] = e.modules[i].desc->name;
    mem_set_module_names(names, count);
    e.modules.free();
    e.event_type_count = 0;
    KV_INFO("engine", "shutdown after %llu frames, %llu ticks",
            (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_count);
}

u32 engine_frame(Engine& e, f64 frame_dt) {
    mem_scratch_reset(); // frame lifetime: everything the last frame put in scratch is gone
    mem_sample(e.frame_count); // last frame's memory, for budgets and anyone reading pressure
    ++e.frame_count;
    u64 first_tick = e.clock.tick_count;
    u32 ticks = e.clock.advance(frame_dt);
    for (u32 i = 0; i < ticks; ++i)
        e.phases.run_tick(e, e.events, e.clock.tick_dt, first_tick + i);
    return ticks;
}

// ---- commands ------------------------------------------------------------------------
// Records in a byte stream: a header, then for a spawn one entry per component.
namespace {

enum CommandOp : u8 { CMD_SPAWN, CMD_DESTROY };

struct CommandHeader {
    u8     op;
    u32    kind;       // kind id (spawn)
    u32    ticket;     // spawn
    u32    components; // spawn: entries that follow
    Handle entity;     // destroy
};

struct ComponentEntryHeader {
    u32 component; // component id
    u32 size;      // bytes that follow, padded to 8
};

template <class T>
void put(Array<u8>& bytes, const T& v) { memcpy(bytes.push_n(sizeof(T)), &v, sizeof(T)); }
template <class T>
const T* take(const u8*& p) { const T* v = (const T*)p; p += sizeof(T); return v; }

} // namespace

u32 command_spawn_begin(CommandBuffer& cb, u32 kind_id, u32 component_count) {
    CommandHeader h{CMD_SPAWN, kind_id, cb.next_ticket++, component_count, Handle{}};
    put(cb.bytes, h);
    ++cb.count;
    return h.ticket;
}

void command_spawn_component(CommandBuffer& cb, u32 component_id, const void* data, u32 size) {
    ComponentEntryHeader h{component_id, size};
    put(cb.bytes, h);
    u32 padded = (size + 7) & ~7u;
    u8* dst = cb.bytes.push_n(padded);
    memcpy(dst, data, size);
    if (padded > size) memset(dst + size, 0, padded - size);
}

void command_destroy(CommandBuffer& cb, Handle h) {
    put(cb.bytes, CommandHeader{CMD_DESTROY, 0, 0, 0, h});
    ++cb.count;
}

void engine_apply_commands(Engine& e) {
    CommandBuffer& cb = e.commands;
    if (cb.count == 0) return;
    const u8* p   = cb.bytes.data;
    const u8* end = p + cb.bytes.len;
    while (p < end) {
        const CommandHeader* h = take<CommandHeader>(p);
        if (h->op == CMD_SPAWN) {
            Table* t = engine_table(e, h->kind);
            Handle entity = t ? t->spawn() : Handle{};
            for (u32 i = 0; i < h->components; ++i) {
                const ComponentEntryHeader* ch = take<ComponentEntryHeader>(p);
                const u8* data = p;
                p += (ch->size + 7) & ~7u;
                if (entity.is_null()) continue;
                Column* c = t->column(ch->component);
                if (!c || c->desc->size != ch->size) {
                    KV_ERROR("engine", "spawn_later of '%s': component '%s' is not one of its", t->desc->name,
                             engine_component(e, ch->component) ? engine_component(e, ch->component)->name : "?");
                    continue;
                }
                memcpy(c->data + (usize)entity.index() * c->stride, data, ch->size);
                if (c->prev) memcpy(c->prev + (usize)entity.index() * c->stride, data, ch->size);
            }
            e.events.emit(Spawned{h->ticket, entity, h->kind}); // null entity: the kind was full or unknown
        } else {
            for (Table& t : e.tables)
                if (t.kind == h->entity.kind()) { t.destroy(h->entity); break; } // stale: silently nothing
        }
    }
    cb.bytes.clear();
    cb.count = 0;
}

void engine_request_quit(Engine& e) { e.quit_requested = true; }

ModuleInstance* engine_module(Engine& e, const char* name) {
    for (ModuleInstance& inst : e.modules)
        if (inst.started && names_equal(inst.desc->name, name)) return &inst;
    return nullptr;
}

void* engine_module_state(Engine& e, const char* name) {
    ModuleInstance* inst = engine_module(e, name);
    return inst ? inst->state : nullptr;
}

u32 engine_resource_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.resources.len; ++i)
        if (names_equal(e.resources[i].name, name)) return i + 1;
    return 0;
}

u32 engine_component_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.components.len; ++i)
        if (names_equal(e.components[i].desc->name, name)) return i + 1;
    return 0;
}

const ComponentDesc* engine_component(const Engine& e, u32 id) {
    return id && id <= e.components.len ? e.components[id - 1].desc : nullptr;
}

u32 engine_kind_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.tables.len; ++i)
        if (names_equal(e.tables[i].desc->name, name)) return i + 1;
    return 0;
}

Table* engine_table(Engine& e, u32 kind_id) {
    return kind_id && kind_id <= e.tables.len ? &e.tables[kind_id - 1] : nullptr;
}

Table* engine_table(Engine& e, const char* name) { return engine_table(e, engine_kind_id(e, name)); }

} // namespace kv
