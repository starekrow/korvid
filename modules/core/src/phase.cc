#include "korvid/core/phase.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/module.h"

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
            mem_set_owner((u16)(s.module->index + 1)); // attribution follows the schedule
            s.desc->fn(ctx);
        }
    }
    mem_set_owner(0);
    events.end_tick();
}

void PhaseList::free() {
    for (Array<ScheduledSystem>& p : systems) p.free();
}

} // namespace kv
