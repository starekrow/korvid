// Korvid core: the bootstrapped core. Owns the event bus, the schedule, the tick clock
// and the module instances. engine_init reads the module list once, assigns every id,
// builds the schedule, and starts the modules in order; engine_shutdown undoes it all.
#pragma once
#include "korvid/core/event.h"
#include "korvid/core/module.h"
#include "korvid/core/phase.h"
#include "korvid/core/tick.h"

namespace kv {

struct ResourceEntry {
    const char* name;
    u32         module; // owning module's index
};

struct Engine {
    EventBus              events;
    PhaseList             phases;
    TickClock             clock;
    Array<ModuleInstance> modules;   // module-list order
    Array<ResourceEntry>  resources; // id = index + 1; 0 is invalid
    u32                   event_type_count = 0;
    u64                   frame_count      = 0;
    bool                  quit_requested   = false;
};

// Registers and starts every module. On any failure, everything already started is
// shut down in reverse and false is returned; the engine is then safe to shut down again.
bool engine_init(Engine& e, Span<const ModuleDesc* const> modules);
void engine_shutdown(Engine& e);

// Advances the clock by frame_dt and runs the resulting ticks. Returns the tick count.
u32  engine_frame(Engine& e, f64 frame_dt);
void engine_request_quit(Engine& e);

// The started instance of a module, by name; null if absent or not started.
ModuleInstance* engine_module(Engine& e, const char* name);
void*           engine_module_state(Engine& e, const char* name);
template <class T>
T* engine_module_state_as(Engine& e, const char* name) {
    ModuleInstance* inst = engine_module(e, name);
    return inst ? inst->state_as<T>() : nullptr;
}

// Resource id by name; 0 if unknown.
u32 engine_resource_id(const Engine& e, const char* name);

} // namespace kv
