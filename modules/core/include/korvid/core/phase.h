// Korvid core: the update schedule. Slice 1 is a hard-coded ordered list of phases,
// each holding systems in registration order. Systems declare read/write sets;
// enforcement is stubbed (recorded, duplicates warned, nothing rejected).
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"

namespace kv {

struct Engine;

struct TickContext {
    Engine*   engine;
    EventBus* events;
    f64       dt;
    u64       tick;
    void*     user; // the system's own pointer from SystemDesc
};

typedef void (*SystemFn)(TickContext& ctx);

struct SystemDesc {
    const char*     name;
    SystemFn        fn;
    void*           user   = nullptr;
    Span<const u32> reads  = {};
    Span<const u32> writes = {};
};

struct Phase {
    const char*       name;
    Array<SystemDesc> systems;
};

struct PhaseList {
    Array<Phase> phases;

    u32  add_phase(const char* name);
    bool add_system(u32 phase, const SystemDesc& desc);
    u32  system_count() const;

    // Runs one tick: for each phase, deliver pending events, then run its systems.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick);
    void free();
};

} // namespace kv
