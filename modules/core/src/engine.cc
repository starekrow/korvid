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
    e.event_type_count = next - 1;
    return true;
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

bool check_resource_refs(const Engine& e, const ModuleDesc& m, const SystemDesc& s,
                         Span<const char* const> refs, const char* kind) {
    for (const char* name : refs) {
        if (!engine_resource_id(e, name)) {
            KV_ERROR("engine", "system '%s' in module '%s' %s unknown resource '%s'",
                     s.name, m.name, kind, name);
            return false;
        }
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
            if (!check_resource_refs(e, m, s, s.reads, "reads") ||
                !check_resource_refs(e, m, s, s.writes, "writes")) return false;

            Array<ScheduledSystem>& phase = e.phases.systems[(u32)s.phase];
            for (const ScheduledSystem& other : phase) {
                if (names_equal(other.desc->name, s.name)) {
                    KV_ERROR("engine", "system '%s' declared by both '%s' and '%s' in phase '%s'",
                             s.name, other.module->desc->name, m.name, phase_name(s.phase));
                    return false;
                }
                // Enforcement is stubbed in slice 1: conflicts are reported, not rejected.
                const SystemDesc& o = *other.desc;
                if (overlaps(o.writes, s.writes) || overlaps(o.writes, s.reads) || overlaps(o.reads, s.writes))
                    KV_WARN("engine", "systems '%s' and '%s' conflict in phase '%s' (not enforced yet)",
                            o.name, s.name, phase_name(s.phase));
            }
            phase.push(ScheduledSystem{&s, &e.modules[i]});
        }
    }
    return true;
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

    bool ok = register_events(e, modules) && register_resources(e, modules) && build_schedule(e, modules);
    if (ok) {
        e.events.init(1);
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
        inst.started = false;
        inst.state   = nullptr;
        arena_destroy(inst.arena); // the module's state goes with it
        mem_set_owner(0);
    }
    for (const ModuleInstance& inst : e.modules) event_types_unregister(inst.desc->events);
    e.phases.free();
    e.events.free();
    e.resources.free();
    // The leak report wants module names; take them before the list goes.
    const char* names[KV_MEM_MAX_MODULES];
    u32 count = e.modules.len < KV_MEM_MAX_MODULES ? e.modules.len : KV_MEM_MAX_MODULES;
    for (u32 i = 0; i < count; ++i) names[i] = e.modules[i].desc->name;
    e.modules.free();
    e.event_type_count = 0;
    KV_INFO("engine", "shutdown after %llu frames, %llu ticks, %llu live allocations",
            (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_count,
            (unsigned long long)mem_live_allocations());
    if (mem_live_allocations()) mem_report(names, count);
}

u32 engine_frame(Engine& e, f64 frame_dt) {
    mem_scratch_reset(); // frame lifetime: everything the last frame put in scratch is gone
    ++e.frame_count;
    u64 first_tick = e.clock.tick_count;
    u32 ticks = e.clock.advance(frame_dt);
    for (u32 i = 0; i < ticks; ++i)
        e.phases.run_tick(e, e.events, e.clock.tick_dt, first_tick + i);
    return ticks;
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

} // namespace kv
