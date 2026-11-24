// Korvid core: the Engine, and the calls that run it, control it and look things up in it.
//
// One Engine holds everything a running program has: the modules and their state, the
// ktables, the schedule, the event streams, the timers and the clock.
// engine_init reads the module list once, assigns every id, builds the schedule and
// starts the modules in order; engine_shutdown stops them in reverse.
//
// A game rarely calls the first half of this header. frameloop_main (or tool_main) makes
// the Engine, calls engine_init, runs the loop and shuts it down. What a game calls is
// the rest: engine_publish in startup, engine_request_quit to leave, engine_pause and
// engine_hold to stop the sim, and the lookups. Inside a system the engine is
// *ctx.engine, but reach data through the context (ctx.view, ctx.resource): the context
// is what the scheduler checks. The functions here are for startup, shutdown, the CLI and
// code outside the schedule.
//
// A test or a custom loop drives the engine itself:
//   Engine e;
//   if (!engine_init(e, kv_module_list())) return 1;
//   for (u32 i = 0; i < 600; ++i) engine_frame(e, 1.0 / 60.0);   // ten seconds of ticks
//   engine_shutdown(e);
//   names_shutdown();
//   mem_shutdown();
#pragma once
#include "korvid/core/event.h"
#include "korvid/core/map.h"
#include "korvid/core/module.h"
#include "korvid/core/phase.h"
#include "korvid/core/tick.h"
#include "korvid/core/timer.h"

namespace kv {

// ---- configuration: what the executable decides, not a module -------------------------
// Memory budgets and ktable sizes. Heaps are named budgets; each module's allocations count
// against the heap it is assigned to. A heap's soft limit warns once per run at the frame
// sample; its hard limit fails the allocation that would pass it, with the heap, module
// and category named. Modules not assigned live in the default heap, which has no limits.
//   constinit const HeapDesc       k_heaps[]        = {{"game", 16u << 20, 64u << 20}};
//   constinit const ModuleHeapDesc k_module_heaps[] = {{"rocks", "game"}};
//   // FrameLoopConfig{.engine = {.heaps = k_heaps, .module_heaps = k_module_heaps}}
struct HeapDesc {
    const char* name;
    usize       soft = 0; // bytes: warn once past this; 0 none
    usize       hard = 0; // bytes: refuse past this; 0 none
};
struct ModuleHeapDesc {
    const char* module; // a module's name, "rocks"
    const char* heap;   // a HeapDesc's name
};
// A ktable's capacity, set by the root over what its KTableDesc says: a stress run that
// wants more of a ktable than the game ships with. Capacity is address space; rows commit as used.
struct KTableCapacity {
    const char* name;     // the ktable's full name, "arcfire.bullet"
    u32         capacity;
};
// Everything engine_init takes besides the module list. FrameLoopConfig::engine is one.
struct EngineConfig {
    Span<const HeapDesc>       heaps        = {};
    Span<const ModuleHeapDesc> module_heaps = {};
    Span<const KTableCapacity> capacities   = {};
    u64                        seed         = 0; // every module's ctx.rng() is seeded from this and its name; --seed N
};

// ---- internal: the engine's records of what was registered -------------------------
// What a resource is. A ktable's resources sit together: the ktable, then its liveness,
// its columns, its attachments and its bags, so the ktable covers the `children` ids
// after its own, and an early read of it the first `early` of them (liveness and columns).
// An event type is a stream: emitted into (appended), consumed (read).
enum class ResourceClass : u8 { State, Published, Append, Stream, KTable, Liveness, Column, Attachment, Bags };

struct ResourceEntry {
    const char*   name;
    u32           module;           // owning module's index
    ResourceClass cls      = ResourceClass::Published;
    void*         ptr      = nullptr; // what the owner published for systems to reach through the context
    const void*   type     = nullptr; // its type key
    u32           children = 0;     // a ktable: the resources after it that it covers
    u32           early    = 0;     // a ktable: how many of those an early read covers: its liveness and columns
    u32           ktable   = 0;     // a ktable's resources: its ktable id
    bool          cosmetic = false; // a column or attachment of a cosmetic component: never feeds the sim
    AppendFn      apply    = nullptr;
    Span<const FieldDesc> fields = {}; // a published resource's field table: what describes it
    bool          hashed   = false; // fields that make it sim state: hashed, plain data (not ResourceDesc::cosmetic)
    u32           event    = 0;     // a stream: its event type id
};

struct ComponentEntry {
    const ComponentDesc* desc;
    u32                  module; // declaring module's index
};

// A state trigger resolved to its column and field.
struct Trigger {
    const TriggerDesc* desc;
    KTable*            ktable;
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
// What the event hook is handed, one delivered event at a time (engine_set_event_hook).
// Everything is valid during the call only.
struct DeliveredEvent {
    u64                   tick;
    u32                   type;      // its id: e.event_types[type]
    const char*           type_name; // the declared name, "Spawned"
    Span<const FieldDesc> fields;    // the type's field table; empty for a type without one
    const char*           emitter;   // the system or handler that emitted it; "" outside the schedule
    const char*           where;     // the phase whose boundary merged it, or "boundary" for the tick's start
    const void*           data;      // all of the event's bytes
    u32                   size;
};
typedef void (*EventHookFn)(void* user, const DeliveredEvent& ev);

struct EventTrace {
    Array<TraceRecord> ring;      // capacity fixed at init; oldest overwritten
    u32                head  = 0; // next slot to write
    u64                total = 0; // records ever written
    bool               print = false;
    EventHookFn        hook      = nullptr; // engine_set_event_hook
    void*              hook_user = nullptr;
};

// ---- the engine -----------------------------------------------------------------------
// One per running program, made by whoever runs the loop (frameloop_main, tool_main, a
// test) and passed by reference everywhere. Most of it is the engine's own bookkeeping.
// What other code reads: `clock` (tick counts, rate, holds), `events` (from code outside
// the schedule: e.events.read<E>()), `quit_requested`, and the flags a loop or a command
// line sets before init (`threads`, `early_reads`, `hash_ticks`, `hash_systems`).
struct Engine {
    EventBus              events;    // every event type's stream; e.events.emit(...) from outside the schedule
    PhaseList             phases;    // the schedule as built
    TickClock             clock;     // the fixed-step clock: tick_count, alpha(), rate, holds
    Array<ModuleInstance> modules;   // module-list order
    // The module records the engine works from: the list as given, except that a module
    // with parts is a joined copy (its tables, then its parts'), which ModuleInstance::desc
    // points at. Each joined table is a block of `joined_blocks`; freed at shutdown.
    struct JoinedBlock { void* data; usize size; };
    Array<const ModuleDesc*> module_list;
    Array<ModuleDesc>     joined;
    Array<JoinedBlock>    joined_blocks;
    Array<ResourceEntry>  resources; // id = index + 1; 0 is invalid
    Array<ComponentEntry> components; // id = index + 1 = ComponentType<T>::id; 0 is invalid
    Array<KTable>         ktables;   // by full name; ktable id = index + 1; addresses stable after init
    Array<const EventTypeDesc*> event_types; // by id; slot 0 unused
    Array<Trigger>        triggers;   // trigger id = index + 1
    CommandBuffer         commands;   // the engine's own: commands from code outside the schedule
    TimerWheel            timers;     // scheduled events (ctx.schedule_in)
    EventTrace            trace;      // --trace-events
    u32                   threads      = 0; // threads the schedule may use, main included; 0: every core
    u64                   seed         = 0; // EngineConfig::seed, kept for engine_sim_reset
    EarlyReadsChoice      early_reads  = EarlyReadsChoice::PerKTable; // set before init: as each ktable says, or one way for all
    u32                   serial       = 0; // this engine among the process's, 1..255: tags the ids the context hands out
    u32                   units        = 0; // systems and handlers
    Array<u32>            streams;            // by event type id: its stream's resource id
    u64                   edges_fired  = 0;
    u32                   event_type_count = 0;
    u64                   frame_count      = 0;
    bool                  quit_requested   = false; // engine_request_quit was called; no more ticks run
    bool                  frame_wanted     = false; // engine_want_frame: a loop that paces on demand draws another frame
    bool                  ticks_wanted     = false; // engine_want_ticks: a loop that paces apart runs the next tick, drawing nothing
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
    Map<u64, Handle>      entity_names;              // Name::hash -> the entity that has it (or had: checked against the ktable)
    u32                   entity_names_lock = 0;     // a spin lock: names change outside the schedule and at boundaries
};

// ---- running it -----------------------------------------------------------------------
// Registers and starts every module, in list order: assigns every id, builds every ktable
// and the schedule (logging it), then runs each startup. Any failure (an unknown
// name in a set, two writers with no order, a startup that returns false) is logged with
// the names involved; everything already started is shut down in reverse and false is
// returned. The engine is then safe to shut down again.
bool engine_init(Engine& e, Span<const ModuleDesc* const> modules, const EngineConfig& config = EngineConfig{});
// Stops every module in reverse order and frees what init made. Afterwards, a program
// that is done calls names_shutdown() and then mem_shutdown(), which reports leaks.
void engine_shutdown(Engine& e);

// Advances the clock by frame_dt seconds of real time and runs the ticks that pays for:
// zero or more, at most the clock's max_ticks_per_frame (4), each paused or not as the
// clock says. Stops before the next tick once engine_request_quit has been called.
// Returns the ticks run, paused ticks included. Resets the main thread's scratch arena.
u32  engine_frame(Engine& e, f64 frame_dt);
// Asks the loop to end. No tick runs after the one that asked; the frame loop finishes
// the frame and shuts down. A game calls it when it is ready to go, usually from a
// handler on ExitRequested.
void engine_request_quit(Engine& e);

// The cycles outside the tick (docs/concepts/cycles.md). engine_frame_cycle runs the Frame
// phase once, `dt` the time since its last run: a loop calls it before each frame it
// draws. engine_work runs Work cycles back to back while a unit of the last one said
// ctx.more() and `budget` seconds last (at least one cycle), and returns the seconds it
// used; a unit sizes its slice by ctx.time_left(). engine_work_pending says whether the
// last Work cycle had more to do, so a loop knows not to sleep. None of these reset the
// scratch arena: engine_frame does.
void engine_frame_cycle(Engine& e, f64 dt);
f64  engine_work(Engine& e, f64 budget);
inline bool engine_work_pending(const Engine& e) { return e.work_pending; }

// Asks the loop for another frame: a loop that draws only on demand (the frame loop's
// Pacing::OnDemand) runs a tick and draws once more; one that draws every vsync ignores it.
// For whatever changed what is on screen without input to show it (a UI's new list, a load
// that landed).
inline void engine_want_frame(Engine& e) { e.frame_wanted = true; }
// Asks the loop for the next tick, at the clock's rate, without a frame: a player feeding
// its audio path and publishing the position. Ask again in each tick it is still wanted:
// it lapses when a tick begins (engine_frame clears it), as a frame request does with each
// frame. The frame
// loop's Pacing::Decoupled runs the tick and draws nothing; OnDemand takes it as a frame
// request; Continuous ticks anyway. A loop of one's own reads e.ticks_wanted.
inline void engine_want_ticks(Engine& e) { e.ticks_wanted = true; }
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

// ---- sim control ----------------------------------------------------------------------
// From anywhere on the main thread: a system, a handler, a CLI command, the loop. A change
// applies from the next tick.
//
// The sim is held (paused) while any hold is set. A held clock still runs ticks at its
// rate, but they are paused ticks: the sim tick doesn't advance, timers and triggers
// wait, and only RunWhen::Always systems and the handlers that have events run, so input,
// menus and the CLI stay alive. Each hold has one owner and only the owner clears it, so
// a player closing the pause menu can't end a level load's hold:
//   engine_hold(e, HoldLoading, true);     // the level module, while it loads
//   engine_pause(e, true);                 // the pause menu: the HoldUser hold
//   engine_hold(e, HoldGame, true);        // a game's own reason (HoldGame << 1 for another)

// Sets or clears the HoldUser hold: a pause menu, the CLI's `pause`.
void engine_pause(Engine& e, bool paused);
// Sets or clears one reason for holding the sim (a HoldReason bit).
void engine_hold(Engine& e, u32 reason, bool on);
// The HoldReason bits set now; 0 means running.
u32  engine_holds(const Engine& e);
// Lets n sim ticks through a pause, pausing first if the clock was running: the CLI's
// `step`, for looking at a game tick by tick.
void engine_step(Engine& e, u32 n);
// The developer's time scale (the CLI's `rate`): 0.5 is half speed. <= 0 is refused,
// with a warning; pause instead.
bool engine_rate(Engine& e, f64 rate);
// The game's own time scale, multiplied with the rate: slow motion that the game ramps
// tick by tick, without fighting a rate the developer set. 0 runs no sim ticks without
// being a pause; negative is refused. Neither scale changes a tick's length, only how
// many run a second, so anything counted in ticks plays out the same.
//   engine_time_scale(e, 0.25f);   // a boss's death in slow motion; 1.0f to resume
bool engine_time_scale(Engine& e, f32 scale);
f32  engine_time_scale_get(const Engine& e);

// A fresh sim: the sim as it stood when the engine started, for a game's second run to be
// the same run as its first in a new process. Every ktable that is not cosmetic is emptied
// with its slots and generations as at startup (the next spawn gets the handle a fresh
// engine would give), the sim tick goes back to 0, and the timer wheel, the events not yet
// delivered, the commands not yet applied and every unit's ticket counters are dropped.
// Then SimReset is emitted, pending, for the modules whose own state follows the sim.
// Cosmetic ktables, assets, module state and the clock's holds are left alone: a cosmetic
// row parented to a sim entity is its owner's to clear, since the handle will be reused.
// Main thread, on a held tick (a cover, a menu), from the system or handler that decides
// it: the units after it in the tick see the fresh sim, and every stream starts over (no
// consumer sees an event about the old run). Spawn the new run after.
void engine_sim_reset(Engine& e);

// ---- modules and published resources --------------------------------------------------
// Hands the engine the object behind one of the module's declared resources (a
// ResourceDesc), so systems reach it through ctx.resource<T>() under their declared access
// rather than through an accessor the scheduler can't see. Call it in startup; the object
// must outlive the module (put it in the module's state). The resource must be the
// module's own. False, logged, for an unknown name or one another module declared.
//   if (!engine_publish(engine, self, ".score", &g->score)) return false;
//   // elsewhere: .reads = "rocks.score"; ctx.resource<const Score>("rocks.score")
// A resource declared with a field table must be plain data whose size the table fits:
// the typed form checks both, false and logged if not.
bool engine_publish(Engine& e, const ModuleInstance& self, const char* name, void* ptr, const void* type);
bool engine_publish_sized(Engine& e, const ModuleInstance& self, const char* name, void* ptr, const void* type, usize size, bool plain);
template <class T>
bool engine_publish(Engine& e, const ModuleInstance& self, const char* name, T* ptr) {
    return engine_publish_sized(e, self, name, (void*)ptr, type_key<T>(), sizeof(T), __is_trivially_copyable(T));
}

// For a module whose functions are main-thread only (ui): call it where they all pass, and
// a unit that calls one from a job worker is stopped in dev builds, named, rather than
// racing. Startup, shutdown and code outside the schedule are on the main thread.
//   SpuiState* spui_state(Engine& e) { engine_check_main_thread(e, "sprite_ui"); return ...; }
void engine_check_main_thread(const Engine& e, const char* api);

// The started instance of a module, by name; null if absent or not started. For code
// outside the schedule (a tool's command, a test) that needs a module's state; systems
// use ctx.state_as and published resources instead.
ModuleInstance* engine_module(Engine& e, const char* name);
void*           engine_module_state(Engine& e, const char* name);
template <class T>
T* engine_module_state_as(Engine& e, const char* name) {
    ModuleInstance* inst = engine_module(e, name);
    return inst ? inst->state_as<T>() : nullptr;
}

// ---- lookups by name ------------------------------------------------------------------
// Names to ids, for code outside the schedule. 0 (or null, or Phase::Count) for unknown.
// Inside a system, the context's own (ctx.resource_id, ctx.column_id, ctx.ktable_id) also
// expand '.' names.
u32 engine_resource_id(const Engine& e, const char* name);
u32                  engine_component_id(const Engine& e, const char* name);
const ComponentDesc* engine_component(const Engine& e, u32 id);
template <class T>
const ComponentDesc* engine_component(const Engine& e) { return engine_component(e, ComponentType<T>::id); }
u32   engine_trigger_id(const Engine& e, const char* name);
// The phase a system was declared in.
Phase engine_system_phase(const Engine& e, const char* name);

// A ktable, by name or id. Null for unknown. One reached this way is the KTable as it is
// now, beside every declared check: inside a system that reads the ktable early from a
// snapshot, that is a read the schedule never saw, so dev builds refuse it there (reach the
// ktable through the context). Elsewhere these are for code outside the schedule (startup,
// the first spawns, a tool), and for a system that only wants what never changes (a
// ktable's handle kind, its name).
//   Handle h = engine_ktable(e, "rocks.ship")->spawn(transform2_at({0, 0}), look);
u32    engine_ktable_id(const Engine& e, const char* name);
KTable* engine_ktable(Engine& e, u32 ktable_id);
KTable* engine_ktable(Engine& e, const char* name);
// The ktable an entity handle was issued by, or null if the handle's kind is not one of
// this engine's ktables. Says nothing about whether the handle is still live: ask the ktable.
KTable* engine_ktable_of(Engine& e, Handle h);
// The same without the snapshot check: the engine's own, on behalf of a system whose
// access the context has already checked, or for what never changes.
KTable* engine_ktable_now(Engine& e, u32 ktable_id);
KTable* engine_ktable_of_now(Engine& e, Handle h);

// ---- entity names ---------------------------------------------------------------------
// Any entity may have a name, unique among living entities across every ktable. The
// name belongs to its entity: hashed with its ktable, saved and restored with it, gone when
// it dies. Names are for scripts, tools, saves and designers (the CLI shows them as @name),
// a few lookups a tick: the map is behind a lock, so don't look one up per entity per tick.
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

// ---- checking and describing ----------------------------------------------------------
// The hash of the sim: every sim column of every live entity, each module's ctx.rng()
// stream, every published resource with a field table, the timers and the next-tick
// events still waiting, and the sim tick. Fixed across runs and platforms and whatever
// order the ktables registered in (ktables by name, handles in fields by kind name). Two runs
// with the same input hash the same every tick; --hash logs it. Module state outside the
// stream is not in it: keep sim state in ktables, or in a resource with a field table.
u64   engine_hash_state(const Engine& e);
// Describes one event through its schema: "Type{...}", or just the name without one.
// Returns what snprintf would.
int  engine_describe_event(const Engine& e, u32 type_id, const void* event, char* out, usize cap);
// The event trace as data, for a dev tool's events panel: fn is called with every event as
// it is delivered, at the boundary that merges it (the tick's start, then each phase's),
// in the order --trace-events prints them: by type, and in emission order within a type.
// On the main thread, between phases, while no unit runs. It is handed the bytes and may
// not keep them; it should not emit. One hook: setting another replaces it, null takes it
// away. Dev builds only: false in ship, and nothing is called.
bool engine_set_event_hook(Engine& e, EventHookFn fn, void* user);

// The name language of the registration tables. A name starting with '.' means
// "<module>." plus the rest ("?." keeps the '?' in front); anything else is itself. The
// result is interned, so it is stable for the process. Lists are comma-separated:
// name_list_next takes the next trimmed name out of `cursor` into `out` (false when
// there is none). Empty names and a null list are nothing.
//   engine_qualify("rocks", ".score")   // "rocks.score"
const char* engine_qualify(const char* module, const char* name);
bool        name_list_next(const char*& cursor, char* out, usize cap);

// How early reads of a ktable's resource get their value, the engine's choice applied.
EarlyReads engine_early_reads(const Engine& e, u32 resource_id);

// ---- internal: the tick runner's ------------------------------------------------------
// Applies and clears every command buffer, the engine's own first and then each unit's
// in unit order (spawns, destroys, names and appends, each buffer in issue order): the
// tick runner calls it after every phase.
void engine_apply_commands(Engine& e);
// Evaluates every state trigger over this tick's previous and current rows and emits
// StateEdge events, pending. The tick runner calls it at the end of the tick.
void engine_fire_triggers(Engine& e, u64 tick);
// Records (and prints, and hands the hook, if asked) what the streams took since the last
// call. Dev builds only.
void engine_trace_delivery(Engine& e, u64 tick, u32 phase);

} // namespace kv
