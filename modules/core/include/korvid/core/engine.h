// Korvid core: the bootstrapped core. Owns the event bus, the schedule, the tick clock
// and the module instances. engine_init reads the module list once, assigns every id,
// builds the schedule, and starts the modules in order; engine_shutdown undoes it all.
#pragma once
#include "korvid/core/event.h"
#include "korvid/core/map.h"
#include "korvid/core/module.h"
#include "korvid/core/phase.h"
#include "korvid/core/tick.h"
#include "korvid/core/timer.h"

namespace kv {

// What the root decides and no module does: the heaps and which module lives in which.
// A heap's soft limit warns once per run at the frame sample; its hard limit fails the
// allocation that would pass it, with the heap, module and category named. Modules not
// assigned live in the default heap, which has no limits.
struct HeapDesc {
    const char* name;
    usize       soft = 0;
    usize       hard = 0;
};
struct ModuleHeapDesc {
    const char* module;
    const char* heap;
};
struct EngineConfig {
    Span<const HeapDesc>       heaps        = {};
    Span<const ModuleHeapDesc> module_heaps = {};
};

struct ResourceEntry {
    const char* name;
    u32         module;         // owning module's index
    void*       ptr  = nullptr; // what the owner published for systems to reach through the context
    const void* type = nullptr; // its type key
    bool        implicit = false; // a module's state: ordered by rank among its systems, never a conflict
    bool        sink     = false; // ResourceDesc::sink: two writers never conflict
};

struct ComponentEntry {
    const ComponentDesc* desc;
    u32                  module; // declaring module's index
};

// A state trigger resolved to its column and field.
struct Trigger {
    const TriggerDesc* desc;
    Table*             table;
    const Column*      column;
    const FieldDesc*   field;
    u32                module;
    const char*        name       = nullptr; // the declared name with the module prefix expanded
    const char*        field_name = nullptr; // likewise for the field
};

// The dev trace: the last events delivered, with who emitted them. Printed as it fills
// when `print` is set (--trace-events); the later CLI and replay read the ring.
struct TraceRecord {
    u64         tick;
    const char* emitter;
    u32         type;
    u16         wave;
    u8          size;      // bytes kept, at most sizeof(data)
    u8          pad;
    u8          data[40];
};
struct EventTrace {
    Array<TraceRecord> ring;      // capacity fixed at init; oldest overwritten
    u32                head  = 0; // next slot to write
    u64                total = 0; // records ever written
    bool               print = false;
};


// ---- idle tasks ------------------------------------------------------------------------
// Work that runs when the frame has time to spare: the frame loop measures its slack and
// hands it to engine_idle_run, which calls the registered tasks round-robin until the
// budget is used or nobody has anything to do. A task slices its own work: it is called
// with the time left and answers More (call me again if there is time), Nothing (nothing
// now, keep me) or Done (remove me). Main thread; a task that wants workers uses the pool.
enum class IdleResult : u8 { Nothing, More, Done };
typedef IdleResult (*IdleFn)(Engine& e, f64 budget_seconds, void* user);

struct IdleTask {
    const char* name;
    IdleFn      fn;
    void*       user;
    u32         id;
    u16         owner;   // memory owner (module index + 1) at registration; set around each call
    u64         calls;
    f64         seconds;
};

struct IdleStats {
    u64 runs    = 0;   // engine_idle_run calls that found a task
    u64 calls   = 0;   // task calls
    f64 seconds = 0.0; // time spent, ever
    f64 last    = 0.0; // the last run
};

struct Engine {
    EventBus              events;
    PhaseList             phases;
    TickClock             clock;
    Array<ModuleInstance> modules;   // module-list order
    Array<ResourceEntry>  resources; // id = index + 1; 0 is invalid
    Array<ComponentEntry> components; // id = index + 1 = ComponentType<T>::id; 0 is invalid
    Array<Table>          tables;     // kind id = index + 1; addresses stable after init
    Array<const EventTypeDesc*> event_types; // by id; slot 0 unused
    Array<ScheduledSystem> handlers;  // in wave order
    Array<Trigger>        triggers;   // trigger id = index + 1
    CommandBuffer         commands;
    TimerWheel            timers;
    EventTrace            trace;
    Array<IdleTask>       idle;        // in registration order; served round-robin from idle_cursor
    u32                   idle_cursor  = 0;
    u32                   idle_next_id = 1;
    IdleStats             idle_stats;
    u32                   wave_cap     = 4; // handler waves per boundary; leftovers wait for the next
    u32                   threads      = 0; // threads the schedule may use, main included; 0: every core
    u32                   units        = 0; // systems plus handlers
    u64                   waves_capped = 0; // boundaries that hit the cap
    u64                   edges_fired  = 0;
    u32                   event_type_count = 0;
    u64                   frame_count      = 0;
    bool                  quit_requested   = false;
    bool                  hash_ticks       = false; // log the state hash after every tick
    bool                  hash_systems     = false; // and after every system, to locate drift
    u64                   last_hash        = 0;     // the state hash after the last tick, when hashing
    Map<u64, Handle>      entity_names;              // Name::hash -> the entity that has it (or had: checked against the table)
    u32                   entity_names_lock = 0;     // a spin lock: names change outside the schedule and at boundaries
};

// The name language of the registration tables. A name starting with '.' means
// "<module>." plus the rest ("?." keeps the '?' in front); anything else is itself. The
// result is interned, so it is stable for the process. Lists are comma-separated:
// name_list_next takes the next trimmed name out of `cursor` into `out` (false when
// there is none). Empty names and a null list are nothing.
const char* engine_qualify(const char* module, const char* name);
bool        name_list_next(const char*& cursor, char* out, usize cap);

// Registers and starts every module. On any failure, everything already started is
// shut down in reverse and false is returned; the engine is then safe to shut down again.
bool engine_init(Engine& e, Span<const ModuleDesc* const> modules, const EngineConfig& config = EngineConfig{});
void engine_shutdown(Engine& e);

// Advances the clock by frame_dt and runs the resulting ticks, paused or not as the clock
// says. Returns the tick count, paused ticks included.
u32  engine_frame(Engine& e, f64 frame_dt);
// Sim control, from anywhere on the main thread (a system, a CLI command, the loop); a
// change applies from the next tick. Pause holds the sim: paused ticks run only the
// RunWhen::Always systems and the handlers. Step lets n sim ticks through a pause (and
// pauses first if the clock was running). Rate scales time while unpaused; <= 0 is refused.
void engine_pause(Engine& e, bool paused); // the HoldUser hold
// Sets or clears one reason for holding the sim (HoldReason bits); paused while any is set.
void engine_hold(Engine& e, u32 reason, bool on);
u32  engine_holds(const Engine& e);
void engine_step(Engine& e, u32 n);
bool engine_rate(Engine& e, f64 rate);
// Applies and clears every command buffer, the engine's own first and then each unit's
// in unit order: the tick runner calls it after every phase and every handler wave.
void engine_apply_commands(Engine& e);
// Evaluates every state trigger over this tick's previous and current rows and emits
// StateEdge events, pending. The tick runner calls it at the end of the tick.
void engine_fire_triggers(Engine& e, u64 tick);
// Records (and prints, if asked) what the last deliver() added. Dev builds only.
void engine_trace_delivery(Engine& e, u64 tick, u32 wave);
// Describes one event through its schema: "Type{...}", or just the name without one.
int  engine_describe_event(const Engine& e, u32 type_id, const void* event, char* out, usize cap);
void engine_request_quit(Engine& e);

// Idle tasks. Registration is startup-ish work from any module; the id removes it.
u32  engine_idle_add(Engine& e, const char* name, IdleFn fn, void* user);
bool engine_idle_remove(Engine& e, u32 id);
// Runs the tasks within the budget and returns the seconds used. The frame loop's call.
f64  engine_idle_run(Engine& e, f64 budget_seconds);

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

// A module hands the engine the object behind one of its declared resources, at startup,
// so systems reach it through ctx.resource<T>() under their declared access instead of
// through an accessor the scheduler cannot see. The resource must be the module's own.
bool engine_publish(Engine& e, const ModuleInstance& self, const char* name, void* ptr, const void* type);
template <class T>
bool engine_publish(Engine& e, const ModuleInstance& self, const char* name, T* ptr) {
    return engine_publish(e, self, name, (void*)ptr, type_key<T>());
}

// Component schemas, by name or id. Null for unknown.
u32                  engine_component_id(const Engine& e, const char* name);
const ComponentDesc* engine_component(const Engine& e, u32 id);
template <class T>
const ComponentDesc* engine_component(const Engine& e) { return engine_component(e, ComponentType<T>::id); }

// Trigger id by name; 0 if unknown.
u32   engine_trigger_id(const Engine& e, const char* name);
// The phase a system was declared in; Phase::Count for an unknown name.
Phase engine_system_phase(const Engine& e, const char* name);
// The hash of every sim column of every live entity, fixed across runs and platforms and
// whatever order the kinds registered in (tables by name, handles in fields by kind name).
u64   engine_hash_state(const Engine& e);

// Entity names. A name is unique among living entities across every kind, and belongs to
// its entity: hashed with its kind, saved and restored with it, gone when it dies. Names
// are for scripts, tools and saves, a few lookups a tick: the map is behind a lock.
//   engine_entity_name_set   names an entity now. For code outside the schedule (startup,
//                            the CLI, a load) or a main-thread unit; a system names with
//                            ctx.name_later, which lands at the boundary in unit order.
//                            False, logged, for a stale handle or a name a living entity
//                            has; an empty or null name takes the entity's name away.
//   engine_entity_find       the living entity with the name, or null; any thread, any time.
//   engine_entity_name       the entity's name, or null.
bool        engine_entity_name_set(Engine& e, Handle entity, const char* name);
Handle      engine_entity_find(Engine& e, const char* name);
Handle      engine_entity_find(Engine& e, Name name);
const char* engine_entity_name(Engine& e, Handle entity);
// Every living named entity, in no particular order; fn returns false to stop.
void        engine_entity_names_each(Engine& e, bool (*fn)(Handle entity, Name name, void* user), void* user);

// Entity kinds, by name or id. Null for unknown.
u32    engine_kind_id(const Engine& e, const char* name);
Table* engine_table(Engine& e, u32 kind_id);
Table* engine_table(Engine& e, const char* name);
// The table an entity handle was issued by, or null if the handle's kind is not one of
// this engine's tables. Says nothing about whether the handle is still live: ask the table.
Table* engine_table_of(Engine& e, Handle h);

} // namespace kv
