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
// A kind's capacity, set by the root over what its KindDesc says: a stress run that wants
// more of a kind than the game ships with. Capacity is address space; rows commit as used.
struct KindCapacity {
    const char* kind;     // the full name, "arcfire.bullet"
    u32         capacity;
};
struct EngineConfig {
    Span<const HeapDesc>       heaps        = {};
    Span<const ModuleHeapDesc> module_heaps = {};
    Span<const KindCapacity>   capacities   = {};
};

// What a resource is. A kind's resources sit together: the kind, then its liveness, its
// columns, its attachments and its bags, so the kind covers the `children` ids after its
// own, and an early read of it the first `early` of them (liveness and columns).
// An event type is a stream: emitted into (appended), consumed (read).
enum class ResourceClass : u8 { State, Published, Append, Stream, Kind, Liveness, Column, Attachment, Bags };

struct ResourceEntry {
    const char*   name;
    u32           module;           // owning module's index
    ResourceClass cls      = ResourceClass::Published;
    void*         ptr      = nullptr; // what the owner published for systems to reach through the context
    const void*   type     = nullptr; // its type key
    u32           children = 0;     // a kind: the resources after it that it covers
    u32           early    = 0;     // a kind: how many of those an early read covers: its liveness and columns
    u32           table    = 0;     // a kind's resources: its kind id
    bool          cosmetic = false; // a column or attachment of a cosmetic component: never feeds the sim
    AppendFn      apply    = nullptr;
    u32           event    = 0;     // a stream: its event type id
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
    u16         phase;     // the segment it was merged in: a phase, or Phase::Count for the tick start and the boundaries
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



struct Engine {
    EventBus              events;
    PhaseList             phases;
    TickClock             clock;
    Array<ModuleInstance> modules;   // module-list order
    Array<ResourceEntry>  resources; // id = index + 1; 0 is invalid
    Array<ComponentEntry> components; // id = index + 1 = ComponentType<T>::id; 0 is invalid
    Array<Table>          tables;     // kind id = index + 1; addresses stable after init
    Array<const EventTypeDesc*> event_types; // by id; slot 0 unused
    Array<Trigger>        triggers;   // trigger id = index + 1
    CommandBuffer         commands;
    TimerWheel            timers;
    EventTrace            trace;
    u32                   threads      = 0; // threads the schedule may use, main included; 0: every core
    EarlyReadsChoice      early_reads  = EarlyReadsChoice::PerKind; // set before init: as each kind says, or one way for all
    u32                   serial       = 0; // this engine among the process's, 1..255: tags the ids the context hands out
    u32                   units        = 0; // systems and handlers
    Array<u32>            streams;            // by event type id: its stream's resource id
    u64                   edges_fired  = 0;
    u32                   event_type_count = 0;
    u64                   frame_count      = 0;
    bool                  quit_requested   = false;
    bool                  frame_wanted     = false; // engine_want_frame: a loop that paces on demand draws another frame
    f64                   frame_due        = 0.0;   // engine_want_frame_at: the earliest time one is wanted; 0 none
    // engine_add_waker: what a loop pacing on demand asks each time it polls the platform.
    struct Waker { bool (*fn)(Engine&, void*); void* user; };
    Waker                 wakers[8]        = {};
    u32                   waker_count      = 0;
    // The Work cycle (engine_work): whether a unit of the running cycle said more, whether
    // the last cycle ended with more to do, and when the running budget ends.
    bool                  work_more        = false;
    bool                  work_pending     = false;
    f64                   work_deadline    = 0.0;
    u64                   work_cycles      = 0, frame_cycles = 0;
    bool                  hash_ticks       = false; // log the state hash after every tick
    bool                  hash_systems     = false; // and after every system, to locate drift
    u64                   last_hash        = 0;     // the state hash after the last tick, when hashing
    u32                   sim_resets       = 0;     // engine_sim_reset calls
    Map<u64, Handle>      entity_names;              // Name::hash -> the entity that has it (or had: checked against the table)
    u32                   entity_names_lock = 0;     // a spin lock: names change outside the schedule and at boundaries
};

// How early reads of a kind's resource get their value, the engine's choice applied.
EarlyReads engine_early_reads(const Engine& e, u32 resource_id);

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
// says, stopping before the next once engine_request_quit has been called. Returns the
// ticks run, paused ticks included.
u32  engine_frame(Engine& e, f64 frame_dt);
// Sim control, from anywhere on the main thread (a system, a CLI command, the loop); a
// change applies from the next tick. Pause holds the sim: paused ticks run only the
// RunWhen::Always systems and the handlers that have events. Step lets n sim ticks through a pause (and
// pauses first if the clock was running). Rate scales time while unpaused; <= 0 is refused.
// Time scale is the game's factor on top of the rate (slow motion that the game ramps
// tick by tick); 0 runs no sim ticks without being a pause, negative is refused. Neither
// changes a tick's length, only how many run a second.
void engine_pause(Engine& e, bool paused); // the HoldUser hold
// Sets or clears one reason for holding the sim (HoldReason bits); paused while any is set.
void engine_hold(Engine& e, u32 reason, bool on);
u32  engine_holds(const Engine& e);
void engine_step(Engine& e, u32 n);
bool engine_rate(Engine& e, f64 rate);
bool engine_time_scale(Engine& e, f32 scale);
f32  engine_time_scale_get(const Engine& e);
// Applies and clears every command buffer, the engine's own first and then each unit's
// in unit order (spawns, destroys, names and appends, each buffer in issue order): the
// tick runner calls it after every phase.
void engine_apply_commands(Engine& e);
// Evaluates every state trigger over this tick's previous and current rows and emits
// StateEdge events, pending. The tick runner calls it at the end of the tick.
void engine_fire_triggers(Engine& e, u64 tick);
// Records (and prints, if asked) what the streams took since the last call. Dev builds only.
void engine_trace_delivery(Engine& e, u64 tick, u32 phase);
// Describes one event through its schema: "Type{...}", or just the name without one.
int  engine_describe_event(const Engine& e, u32 type_id, const void* event, char* out, usize cap);
void engine_request_quit(Engine& e);

// A fresh sim: the sim as it stood when the engine started, for a game's second run to be
// the same run as its first in a new process. Every kind that is not cosmetic is emptied
// with its slots and generations as at startup (the next spawn gets the handle a fresh
// engine would give), the sim tick goes back to 0, and the timer wheel, the events not yet
// delivered, the commands not yet applied and every unit's ticket counters are dropped.
// Then SimReset is emitted, pending, for the modules whose own state follows the sim.
// Cosmetic kinds, assets, module state and the clock's holds are left alone: a cosmetic
// row parented to a sim entity is its owner's to clear, since the handle will be reused.
// Main thread, on a held tick (a cover, a menu), from the system or handler that decides
// it: the units after it in the tick see the fresh sim, and every stream starts over (no
// consumer sees an event about the old run). Spawn the new run after.
void engine_sim_reset(Engine& e);

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

// Entity kinds, by name or id. Null for unknown. A table reached this way is the table as
// it is now, beside every declared check: inside a system that reads the kind early from a
// snapshot, that is a read the schedule never saw, so dev builds refuse it there (reach the
// kind through the context). Elsewhere these are for code outside the schedule, and for a
// system that only wants what never changes (a kind's handle kind, its name).
u32    engine_kind_id(const Engine& e, const char* name);
Table* engine_table(Engine& e, u32 kind_id);
Table* engine_table(Engine& e, const char* name);
// The table an entity handle was issued by, or null if the handle's kind is not one of
// this engine's tables. Says nothing about whether the handle is still live: ask the table.
Table* engine_table_of(Engine& e, Handle h);
// The same without the check: the engine's own, on behalf of a system whose access the
// context has already checked.
Table* engine_table_now(Engine& e, u32 kind_id);
Table* engine_table_of_now(Engine& e, Handle h);

// The cycles outside the tick (docs/concepts/cycles.md). engine_frame_cycle runs the Frame
// phase once, `dt` the time since its last run: a loop calls it before each frame it
// draws. engine_work runs Work cycles back to back while a unit of the last one said
// ctx.more() and `budget` seconds last (at least one cycle), and returns the seconds it
// used; a unit sizes its slice by ctx.time_left(). What used to be idle tasks are Work
// units. engine_work_pending says whether the last Work cycle had more to do, so a loop
// knows not to sleep. None of these reset the scratch arena: engine_frame does.
void engine_frame_cycle(Engine& e, f64 dt);
f64  engine_work(Engine& e, f64 budget);
inline bool engine_work_pending(const Engine& e) { return e.work_pending; }

// Asks the loop for another frame: a loop that draws only on demand (the frame loop's
// Pacing::OnDemand) runs a tick and draws once more; one that draws every vsync ignores it.
// For whatever changed what is on screen without input to show it (a UI's new list, a load
// that landed).
inline void engine_want_frame(Engine& e) { e.frame_wanted = true; }
// A frame at a time (time_now's clock): a caret's next blink. The earliest asked for stands.
inline void engine_want_frame_at(Engine& e, f64 when) { if (e.frame_due <= 0.0 || when < e.frame_due) e.frame_due = when; }

// Something the engine's events do not see, that a loop pacing on demand looks at each time it
// polls the platform (every few milliseconds while idle): a socket a module reads (asset_live's
// connection to goldberg). `fn` does what it does with what arrived and says true when anything
// did; the loop then draws a frame. It must be cheap and never block. Eight at most; false when
// full. Remove it before the state `user` names goes.
bool engine_add_waker(Engine& e, bool (*fn)(Engine&, void*), void* user);
void engine_remove_waker(Engine& e, bool (*fn)(Engine&, void*), void* user);
// Asks each waker; true when one had something. The frame loop calls it; a loop of its own may.
bool engine_wake(Engine& e);

} // namespace kv
