// Korvid core: the update schedule. Slice 1 is a fixed, ordered list of tick phases;
// modules place systems in them. Within a phase the order is module-list order, then
// declaration order. Systems declare read/write sets by resource name; enforcement is
// stubbed (recorded, conflicts warned, nothing rejected). The DAG scheduler comes later.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/core/handle.h"

namespace kv {

struct Engine;
struct ModuleInstance;

// Fixed order. Add phases here, never at runtime.
enum class Phase : u8 {
    Input,   // sample devices into per-tick input state
    PreSim,  // snapshot previous state, advance timers
    Sim,     // gameplay
    PostSim, // reactions to this tick's results
    Count
};
const char* phase_name(Phase p);

struct TickContext {
    Engine*     engine;
    EventBus*   events;
    f64         dt;
    u64         tick;
    void*       state;      // the running system's module state (ModuleInstance::state)
    const void* state_type; // its type key, recorded when startup stored it

    // The module state as the type startup stored. Verified by a pointer compare.
    template <class T>
    T& state_as() const {
        KV_VERIFY(state && state_type == type_key<T>());
        return *(T*)state;
    }
};

typedef void (*SystemFn)(TickContext& ctx);

// One entry in a module's system table. Plain data; constinit-able; named fields.
struct SystemDesc {
    const char*             name   = nullptr;
    Phase                   phase  = Phase::Count;
    SystemFn                fn     = nullptr;
    Span<const char* const> reads  = {};
    Span<const char* const> writes = {};
};

// A system placed in the schedule, with the module it came from.
struct ScheduledSystem {
    const SystemDesc* desc;
    ModuleInstance*   module;
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];

    u32  system_count() const;

    // Runs one tick: for each phase, deliver pending events, then run its systems.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick);
    void free();
};

} // namespace kv
