// Korvid core: the bootstrapped core. Owns the event bus, the phase list and the
// tick clock, and starts the registered modules in order.
#pragma once
#include "korvid/core/event.h"
#include "korvid/core/module.h"
#include "korvid/core/phase.h"
#include "korvid/core/tick.h"

namespace kv {

struct Engine {
    EventBus              events;
    PhaseList             phases;
    TickClock             clock;
    Span<const ModuleDesc> modules;
    u32                   started_modules = 0;
    u64                   frame_count     = 0;
    bool                  quit_requested  = false;
};

bool engine_init(Engine& e, Span<const ModuleDesc> modules);
void engine_shutdown(Engine& e);

// Advances the clock by frame_dt and runs the resulting ticks. Returns the tick count.
u32  engine_frame(Engine& e, f64 frame_dt);
void engine_request_quit(Engine& e);

} // namespace kv
