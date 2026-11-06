#include "korvid/core/phase.h"
#include "korvid/core/log.h"
#include <string.h>

namespace kv {

u32 PhaseList::add_phase(const char* name) {
    Phase p;
    p.name = name;
    phases.push(p);
    return phases.len - 1;
}

static bool overlaps(Span<const u32> a, Span<const u32> b) {
    for (u32 x : a)
        for (u32 y : b)
            if (x == y) return true;
    return false;
}

bool PhaseList::add_system(u32 phase, const SystemDesc& desc) {
    if (phase >= phases.len || !desc.fn || !desc.name) return false;
    Phase& p = phases[phase];
    for (const SystemDesc& s : p.systems) {
        if (strcmp(s.name, desc.name) == 0) {
            KV_ERROR("phase", "system '%s' registered twice in phase '%s'", desc.name, p.name);
            return false;
        }
        // Enforcement is stubbed in slice 1: conflicts are reported, not rejected.
        if (overlaps(s.writes, desc.writes) || overlaps(s.writes, desc.reads) ||
            overlaps(s.reads, desc.writes)) {
            KV_WARN("phase", "systems '%s' and '%s' conflict in phase '%s' (not enforced yet)",
                    s.name, desc.name, p.name);
        }
    }
    p.systems.push(desc);
    return true;
}

u32 PhaseList::system_count() const {
    u32 n = 0;
    for (const Phase& p : phases) n += p.systems.len;
    return n;
}

void PhaseList::run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick) {
    TickContext ctx;
    ctx.engine = &engine;
    ctx.events = &events;
    ctx.dt     = dt;
    ctx.tick   = tick;
    for (Phase& p : phases) {
        events.deliver(); // phase boundary: pending events become visible
        for (SystemDesc& s : p.systems) {
            ctx.user = s.user;
            s.fn(ctx);
        }
    }
    events.end_tick();
}

void PhaseList::free() {
    for (Phase& p : phases) p.systems.free();
    phases.free();
}

} // namespace kv
