#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"

namespace kv {

bool engine_init(Engine& e, Span<const ModuleDesc> modules) {
    e.events.init(1);
    e.modules = modules;
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = modules[i];
        if (m.startup && !m.startup(e)) {
            KV_ERROR("engine", "module '%s' failed to start", m.name);
            engine_shutdown(e);
            return false;
        }
        ++e.started_modules;
        KV_INFO("engine", "module '%s' up", m.name);
    }
    return true;
}

void engine_shutdown(Engine& e) {
    while (e.started_modules > 0) {
        const ModuleDesc& m = e.modules[--e.started_modules];
        if (m.shutdown) m.shutdown(e);
        KV_INFO("engine", "module '%s' down", m.name);
    }
    e.phases.free();
    e.events.free();
    KV_INFO("engine", "shutdown after %llu frames, %llu ticks, %llu live allocations",
            (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_count,
            (unsigned long long)mem_live_allocations());
}

u32 engine_frame(Engine& e, f64 frame_dt) {
    ++e.frame_count;
    u64 first_tick = e.clock.tick_count;
    u32 ticks = e.clock.advance(frame_dt);
    for (u32 i = 0; i < ticks; ++i)
        e.phases.run_tick(e, e.events, e.clock.tick_dt, first_tick + i);
    return ticks;
}

void engine_request_quit(Engine& e) { e.quit_requested = true; }

} // namespace kv
