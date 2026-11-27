// Korvid core: systems and handlers, and the TickContext they run with.
//
// A system is a function the engine calls every tick in one phase, and a row in the
// module's table saying what it touches:
//
//   void move(TickContext& ctx) {
//       for (KTableRows k : ctx.view<Transform2, const Velocity>()) { ... }
//   }
//   constinit const SystemDesc k_systems[] = {
//       {.name = ".move", .phase = Phase::Sim, .fn = move, .reads = "*.Velocity", .writes = "*.Transform2"},
//   };
//
// `reads` and `writes` name resources: a ktable's column (".rock.Velocity"), the same
// column in every ktable that has it ("*.Velocity"), a whole ktable (".rock"), a published
// resource ("input.state"), an event type ("RockHit"). The sets are the contract.
// The engine orders the systems of a phase from them (a reader after every writer of
// what it reads; two writers of one thing need an `after` or `before`), refuses at
// engine_init anything that would race, runs the systems with no order between them at
// the same time on the job pool, and in dev builds stops on any access through the
// context that the sets don't cover. A handler is a system on one event type, handed its
// batch (HandlerDesc).
//
// Everything a system does goes through its TickContext: rows and resources (view, read,
// write, resource), events (emit, received), commands that take effect at the end of
// the phase (spawn_later, destroy), and timers (schedule_in).
//
// The results don't depend on the schedule. Every unit (system or handler) has its own
// event writer and command buffer, and each chunk of a parallel_for has its own, merged
// in chunk order. Units are numbered by where they are declared, not by when they ran,
// and commands and events come out in that order. So --threads 1 and --threads 16 make
// the same game.
//
// docs/concepts/scheduling.md has the ordering rules in full;
// docs/guides/frame-loop-and-scheduling.md walks through them with a game.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/core/handle.h"
#include "korvid/core/job.h"
#include "korvid/core/ktable.h"
#include "korvid/core/arena.h"
#include "korvid/core/random.h"

namespace kv {

struct Engine;
struct ModuleInstance;
struct ScheduledSystem;
struct TickContext;

// Where a system runs. The first four are a tick, in this order, with a boundary after
// each where commands apply and events merge; Frame and Work are cycles of their own,
// run when the loop asks (docs/concepts/cycles.md). Game code goes in the first four.
enum class Phase : u8 {
    Input,   // devices sampled into the tick's input; handlers for what the last tick sent on
    PreSim,  // CompPrev copies are taken as it starts; housekeeping before gameplay (asset pumps, pause)
    Sim,     // gameplay: control, movement, collision
    PostSim, // reactions to this tick's results: hits, scoring, the HUD
    Frame,   // presentation: once per frame drawn (engine_frame_cycle); never in a replay
    Work,    // throughput: over and over while a unit says ctx.more() (engine_work)
    Count
};
constexpr u32 KV_SIM_PHASES = 4; // Input..PostSim: what a tick runs
// "Input", "PreSim", ...: for logs.
const char* phase_name(Phase p);

// What a system does with its module's state (what startup stored with set_state), which
// is a resource like any other, "<module>.state". Write, the default, reaches it as
// ctx.state_as<T>(), and so is ordered against every other user of it in the module, in
// declaration order. Read reaches it as state_as<const T>() only, and runs beside the
// other readers. None doesn't reach it at all: a system that works only on columns (a
// mover) says None, and is then free to run beside the module's other systems.
// Handlers always have the state.
enum class StateUse : u8 { Write, Read, None };

// How a unit touches one resource, as the schedule records it. A unit has one mode per
// resource; a write includes reading it as it stands.
enum class Access : u8 {
    Read,   // as it stands after every writer in the phase
    Early,  // as the phase found it ('^' in the reads)
    Write,
    Append, // records for the owner, applied at the boundary (ResourceDesc::apply)
};
const char* access_name(Access a); // "read", "early", ...: for logs
struct UnitAccess {
    u32    resource;
    Access mode;
};

// How early reads get their value: as each ktable says (KTableDesc::early_reads), or one
// way for every ktable (Engine::early_reads, --early-reads), to check that the game
// hashes the same either way.
enum class EarlyReadsChoice : u8 { PerKTable, Order, Snapshot };
const char* early_reads_name(EarlyReadsChoice m);

// Whether a system runs on a paused tick (engine_pause, or any hold). Unpaused, the
// default, is gameplay: skipped while paused, along with the timers and the triggers.
// Always is for what keeps a paused game alive: input sampling, asset pumps, the
// hierarchy, text, animation on the real clock, audio, the CLI, and a game's pause menu.
// Handlers run whenever they have events, paused or not.
enum class RunWhen : u8 { Unpaused, Always };

// Emitted by the engine when a spawn_later is applied, so the system that asked learns
// the handle. Keep the ticket spawn_later returned and match it in a handler on Spawned:
//   g.blink_ticket = ctx.spawn_later(g_blinker_ktable, transform2_at(p), look);
//   void on_spawned(TickContext& ctx, Span<const Spawned> spawns) {
//       for (const Spawned& s : spawns) if (s.ticket == g.blink_ticket) g.blinker = s.entity;
//   }
// Tickets are unique across units. A spawn by a unit that holds the ktable's write
// (ctx.write_ktable(k)->spawn(...)) has its handle at once and needs none of this.
struct Spawned {
    u32    ticket; // what spawn_later returned; 0 for a spawn issued from a parallel chunk
    u32    ktable; // ktable id
    Handle entity; // null if the ktable was full
};

// Emitted by the engine at the end of a tick for every state trigger (TriggerDesc) that
// fired, delivered at the start of the next. Compare `trigger` with the id the
// TriggerDesc's `id` slot received to tell triggers apart.
struct StateEdge {
    Handle entity;
    u32    trigger; // trigger id
    u32    pad;
    f64    value;   // the field's value after the edge
};

// Emitted by engine_sim_reset, for the module state that follows the sim (a broadphase's
// last pairs, a cache keyed by handle): the handles it knew are about to be issued again.
struct SimReset {
    u32 resets; // how many there have been, this one included
    u32 pad;
};

// A handler: a system on one event type, called with its batch, the events of that type
// emitted since it last ran. It is scheduled in its phase like any system, after every
// emitter of its event there, and is called only when its batch isn't empty (paused
// ticks included). If the event type has a schema (KV_EVENT_EX), handles in the batch are
// live: an event naming an entity destroyed since it was emitted is dropped first.
// Handlers run on the main thread unless they say `.any_thread = true`, so a handler may
// call main-thread-only functions (sprite_ui's) without thinking about it.
//
//   void on_expire(TickContext& ctx, Span<const Expire> expiries) {
//       for (const Expire& e : expiries) ctx.destroy(e.bullet);
//   }
//   constinit const HandlerDesc k_handlers[] = {
//       {.name = ".on_expire", KV_ON(Expire, on_expire), .phase = Phase::Input},
//   };
//
// Put a handler in the phase that emits its event, or the one after. When the choice
// matters (what the handler writes is read earlier in the emitting phase), init fails
// and says so; docs/concepts/modules.md#handlers has the rule.
typedef void (*HandlerFn)(TickContext& ctx, const void* events, u32 count);
template <class E, void (*F)(TickContext&, Span<const E>)>
void handler_thunk(TickContext& ctx, const void* events, u32 count) { F(ctx, Span<const E>((const E*)events, count)); }

// Names in these tables are strings in the engine's name language. A name that starts
// with '.' is the module's own: in module "rocks", ".on_hit" is "rocks.on_hit" and
// ".rock.Health" that ktable's column; "?name" in `after`/`before` is an order that holds
// only if that module is in the build. A set is one string of comma-separated names
// (".ship, .bullet.Transform2, *.Velocity, input.state"). The engine expands them at init;
// an unknown name fails engine_init, naming the unit and the name.
struct HandlerDesc {
    const char* name       = nullptr;
    u32*        event      = nullptr; // KV_ON fills these three
    const char* event_name = nullptr;
    HandlerFn   fn         = nullptr;
    Phase       phase      = Phase::Count; // required
    const char* reads      = nullptr; // as for a system; its own event is read as it stands unless named with '^'
    const char* writes     = nullptr;
    const char* appends    = nullptr;
    const char* emits      = nullptr; // event types it emits
    const char* after      = nullptr; // system or handler names, as for a system
    const char* before     = nullptr;
    bool        any_thread = false;   // false: on the main thread, where main-thread APIs (ui) are safe; true: any worker, for a heavy one
};

// A state trigger: the engine watches one scalar field across ticks, for every live row
// of a ktable, and emits StateEdge when it changes or crosses a value. The component must
// keep a previous copy (CompPrev), since the edge is between last tick and this one.
// Evaluated at the end of every unpaused tick.
//   constinit u32 g_death = 0;
//   constinit const TriggerDesc k_triggers[] = {
//       {.name = ".death", .field = ".ship.Health.hp", .edge = TriggerEdge::CrossedBelow, .value = 1.0, .id = &g_death},
//   };
//   // a handler on StateEdge: if (e.trigger == g_death) ...
enum class TriggerEdge : u8 {
    Changed,      // the field differs from the previous tick
    CrossedBelow, // previous >= value, current < value
    CrossedAbove, // previous <= value, current > value
};
struct TriggerDesc {
    const char* name  = nullptr;
    const char* field = nullptr; // "ktable.Component.field"; ".ship.Health.hp" for the module's own ktable
    TriggerEdge edge  = TriggerEdge::Changed;
    f64         value = 0.0;
    u32*        id    = nullptr; // optional: filled with the trigger id at init
};

// ---- internal: command buffers, and the pieces behind the templates below -----------
// Systems issue commands through the TickContext (spawn_later, destroy, ...); these are
// what it writes them into. Spawns and destroys are applied at phase boundaries in issue
// order: the engine's own buffer first (code outside the schedule), then every unit's
// in unit order.
struct CommandBuffer {
    Array<u8> bytes;
    u32       count       = 0;
    u32       next_ticket = 1; // for the engine's own buffer; units bring their own tickets
};
u32  command_spawn_begin(CommandBuffer& cb, u32 ktable_id, u32 component_count, u32 ticket); // ticket 0: the buffer assigns
void command_spawn_component(CommandBuffer& cb, u32 component_id, const void* data, u32 size);
void command_destroy(CommandBuffer& cb, Handle h);
void command_destroy_all(CommandBuffer& cb, u32 ktable_id);
// Names an entity at the boundary; a null entity names the last one this buffer spawned.
void command_name(CommandBuffer& cb, Handle h, u64 name_hash);
// A record for an append resource's owner, handed to its apply function at the boundary.
void command_append(CommandBuffer& cb, u32 resource_id, const void* record, u32 size);

template <class T> struct RemoveConst          { typedef T type; static constexpr bool is_const = false; };
template <class T> struct RemoveConst<const T> { typedef T type; static constexpr bool is_const = true; };

// The functions behind TickContext's templates, in phase.cc. Each returns null (or false)
// after logging and, in dev builds, asserting, when the system did not declare the access
// or the type does not match the schema.
KTable* ctx_ktable(TickContext& ctx, const char* name, bool write);
KTable* ctx_ktable(TickContext& ctx, u32 ktable_id, bool write);
Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows);
Column* ctx_column(TickContext& ctx, u32 resource_id, const void* type, bool write, u32* rows);
Column* ctx_column_in(TickContext& ctx, KTable& table, u32 component_id, const void* type, bool write);
void*   ctx_resource(TickContext& ctx, u32 resource_id, const void* type, bool write);
u32     ctx_resource_id(TickContext& ctx, const char* name, bool column);
u32     ctx_ktable_count(TickContext& ctx);
KTable* ctx_ktable_at(TickContext& ctx, u32 i);
u32     ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, const char* name, u32 component_count, bool ticketed); // ticket; 0 if unknown or unticketed
u32     ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, u32 ktable_id, u32 component_count, bool ticketed);
void    ctx_spawn_component(CommandBuffer* cb, u32 component_id, const void* data, u32 size);
void    ctx_destroy(TickContext& ctx, CommandBuffer* cb, Handle h);
void    ctx_destroy_all(TickContext& ctx, CommandBuffer* cb, u32 ktable_id);
void    ctx_name(TickContext& ctx, CommandBuffer* cb, Handle h, const char* name);
u32     ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick);
void*   ctx_attached(TickContext& ctx, Handle h, u32 component_id, const void* type, bool write);
void*   ctx_attach(TickContext& ctx, Handle h, u32 component_id, const void* type, const void* data);
bool    ctx_detach(TickContext& ctx, Handle h, u32 component_id);
KTable* ctx_attachments(TickContext& ctx, u32 ktable_id, u32 component_id, bool write); // the ktable, its store's access checked
KTable* ctx_props(TickContext& ctx, Handle h, bool write);                            // the entity's ktable, its bags' access checked
bool    ctx_cancel(TickContext& ctx, u32 ticket);
bool    ctx_state_allowed(const TickContext& ctx, bool write); // logs and asserts when not
bool    ctx_may_append(TickContext& ctx, u32 resource_id); // declared in `appends` (or the owner's write); logs and asserts when not
bool    ctx_alive(TickContext& ctx, Handle h);             // a read of the ktable's liveness (ktable.@alive), checked
void    ctx_emit_check(const TickContext& ctx, u32 type_id);  // dev builds: declared in `emits`
const void* ctx_received(TickContext& ctx, u32 type_id, u32* count); // the unit's batch of a type it consumes
KTable* ctx_rows(TickContext& ctx, u32 ktable_id);         // likewise: the unit's view of the ktable, for its rows
// The unit running on this thread (a system or handler, or a parallel_for chunk of one);
// null outside the schedule. Returns the one it replaces.
const ScheduledSystem* ctx_running(const ScheduledSystem* unit);
const ScheduledSystem* ctx_running();
bool    ctx_append(TickContext& ctx, u32 resource_id, const void* record, u32 size);

// Per-chunk buffers for a parallel_for, in the caller's scratch: made before, merged after.
struct ParallelBuffers {
    EventWriter*   writers;
    CommandBuffer* commands;
    u32            chunks;
};
ParallelBuffers ctx_parallel_begin(TickContext& ctx, u32 chunks);
const char*     ctx_unit_name(TickContext& ctx); // the running unit's name, for the profiler
void            ctx_parallel_end(TickContext& ctx, ParallelBuffers& pb);

// ---- the context: what a system or handler reaches the engine through ------------------

// One ktable's rows, from ctx.view or ctx.rows. Rows are slots: [0, rows()) includes dead
// ones, so a loop checks alive(i). A column's index i is the entity at handle_at(i), and a
// live handle's index() is its row.
struct KTableRows {
    TickContext* ctx;
    KTable*      ktable;

    u32    rows() const { return ktable->rows(); }
    bool   alive(u32 i) const { return ktable->is_alive(i); }
    Handle handle_at(u32 i) const { return ktable->handle_at(i); }
    const KTableDesc& desc() const { return *ktable->desc; }

    // One column over every row. T const is a read, T is a write; checked against the
    // system's sets in dev builds. Empty if the ktable has no T.
    template <class T>
    Span<T> col() const {
        typedef typename RemoveConst<T>::type U;
        Column* c = ctx_column_in(*ctx, *ktable, ComponentType<U>::id, &TypeKey<U>::key, !RemoveConst<T>::is_const);
        return c ? Span<T>((T*)c->data, ktable->rows()) : Span<T>();
    }
    // The previous-tick rows, for components that keep them. T const is a read; T is a
    // write (checked against the write set): a system writes the previous copy to say an
    // entity teleported, so nothing interpolates to where it is now.
    template <class T>
    Span<T> prev() const {
        typedef typename RemoveConst<T>::type U;
        Column* c = ctx_column_in(*ctx, *ktable, ComponentType<U>::id, &TypeKey<U>::key, !RemoveConst<T>::is_const);
        return c && c->prev ? Span<T>((T*)c->prev, ktable->rows()) : Span<T>();
    }
};

// What ctx.view returns: every ktable that has all of C..., one KTableRows each.
template <class... C>
struct KTableRange {
    TickContext* ctx;

    struct Iter {
        TickContext* ctx;
        u32          i, n;
        void settle() {
            for (; i < n; ++i) {
                KTable* t = ctx_ktable_at(*ctx, i);
                if ((t->has(ComponentType<typename RemoveConst<C>::type>::id) && ...)) return;
            }
        }
        KTableRows operator*() const { return KTableRows{ctx, ctx_ktable_at(*ctx, i)}; }
        Iter&    operator++() { ++i; settle(); return *this; }
        bool     operator!=(const Iter& o) const { return i != o.i; }
    };
    Iter begin() const { Iter it{ctx, 0, ctx_ktable_count(*ctx)}; it.settle(); return it; }
    Iter end() const { return Iter{ctx, ctx_ktable_count(*ctx), ctx_ktable_count(*ctx)}; }
};

// What one chunk of a parallel_for may do besides read and write rows: emit events and
// issue spawns and destroys, into the chunk's own buffers. A chunk's spawn has no ticket,
// and timers are not available here: schedule from the system body, or emit an event and
// let a handler do it.
struct ChunkContext {
    TickContext*   ctx;
    EventWriter*   writer;
    CommandBuffer* commands;
    u32            chunk;

    template <class E>
    void emit(const E& e) { ctx_emit_check(*ctx, EventType<E>::id); writer->emit(e); }
    template <class K, class... C>
    void spawn_later(K ktable, const C&... c) {
        if (ctx_spawn_begin(*ctx, commands, ktable, sizeof...(C), false) || sizeof...(C) == 0)
            (ctx_spawn_component(commands, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
    }
    void destroy(Handle h) { ctx_destroy(*ctx, commands, h); }
};

// One ktable's attachments of a component, live entities only, in slot order (from
// ctx.attached_all):
//   for (Attached<Talk> a : ctx.attached_all<Talk>(".npc")) use(a.entity, *a.value);
template <class T>
struct Attached { Handle entity; T* value; };
template <class T>
struct AttachedRange {
    KTable*      ktable = nullptr;
    Attachment*  store = nullptr;
    struct Iter {
        KTable* t; Attachment* a; u32 i;
        void settle() { while (a && i < a->rows.count() && !t->sparse_live(a->rows, i)) ++i; }
        Attached<T> operator*() const { return Attached<T>{t->handle_at(a->rows.slots[i]), (T*)a->rows.at(i)}; }
        Iter& operator++() { ++i; settle(); return *this; }
        bool  operator!=(const Iter& o) const { return i != o.i; }
    };
    Iter begin() const { Iter it{ktable, store, 0}; it.settle(); return it; }
    Iter end() const { return Iter{ktable, store, store ? store->rows.count() : 0}; }
};

// ---- the TickContext ---------------------------------------------------------------
// What every system and handler is called with: its way to the engine's data, events,
// commands and timers, checked against what it declared. A context is valid for the call
// it was passed to; don't keep one.
struct TickContext {
    Engine*                engine;
    EventBus*              events;
    f64                    dt;   // seconds: the clock's tick_dt (1/60) in a tick, the time since the last run in Frame
    u64                    tick; // the sim tick: advances only on unpaused ticks
    void*                  state;      // the running system's module state (ModuleInstance::state)
    const void*            state_type; // its type key, recorded when startup stored it
    const ScheduledSystem* system   = nullptr; // the running system or handler, whose sets gate the accessors; null: anything goes
    EventWriter*           writer   = nullptr; // the unit's; null: the bus's writer 0 (outside the schedule)
    CommandBuffer*         commands = nullptr; // the unit's; null: the engine's
    bool                   paused   = false;   // a paused tick: the sim stands still, only RunWhen::Always systems run

    // ---- the module's random stream -----------------------------------------------------
    // The module's own Rng, owned by the engine: seeded from the module's name and the
    // engine's seed (EngineConfig::seed, --seed N), part of the state hash, and seeded
    // again by engine_sim_reset. It belongs to the module's state, so the scheduler already
    // orders every unit that draws from it; a system with `.state = StateUse::Read` or
    // `None` may not draw (dev builds stop it). Draw the sim's randomness from here and it
    // is seeded, hashed and replayed with nothing more to do:
    //   Velocity v{dir * speed, ctx.rng().range(-1.5f, 1.5f)};
    // Randomness that is only for show (sparks, a shake) keeps an Rng of its own in the
    // module's state, so drawing it never shifts the sim's. Not from a parallel_for chunk,
    // whose order across threads is not fixed: draw before the loop, or per chunk from a
    // stream seeded by the chunk's index.
    Rng& rng() const;

    // ---- the Work cycle ---------------------------------------------------------------
    // A Work unit with more to do says so, and the cycle runs again while time remains.
    //   void crunch(TickContext& ctx) {
    //       Job& j = ctx.state_as<Job>();
    //       while (j.next < j.count && ctx.time_left() > 0.0) step(j);
    //       if (j.next < j.count) ctx.more();
    //   }
    void more() const;
    // Seconds left of the running unit's share of what engine_work was given: the cycle's
    // time split evenly among the units still to run in it, so one that takes all its share
    // leaves the rest theirs. 0 outside the Work cycle.
    f64  time_left() const;

    // The module's state, as the type startup stored with set_state (checked).
    // state_as<const Game>() is a read and state_as<Game>() a write, checked against the
    // system's `state` in dev builds.
    template <class T>
    T& state_as() const {
        typedef typename RemoveConst<T>::type U;
        static_assert(sizeof(U) > 0, "a complete type");
        KV_VERIFY(state && state_type == type_key<U>());
        ctx_state_allowed(*this, !RemoveConst<T>::is_const);
        return *(T*)state;
    }

    // ---- events -----------------------------------------------------------------------
    // Emits an event, declared in the unit's `emits`. Units in this phase that read the type
    // run after this one and see it; later phases and ticks see it too, each consumer once.
    template <class E>
    void emit(const E& e) {
        if (!writer) { events->emit(e); return; }
        ctx_emit_check(*this, EventType<E>::id);
        writer->emit(e);
    }
    // The events of a type this unit reads (named in `reads`, as "RockHit") that were
    // emitted since it last ran. A handler gets its own type as its argument instead.
    //   for (const OverlapBegin& o : ctx.received<OverlapBegin>()) ...
    template <class E>
    Span<const E> received() {
        u32 n = 0;
        const void* p = ctx_received(*this, EventType<E>::id, &n);
        return Span<const E>((const E*)p, n);
    }

    // ---- data access ---------------------------------------------------------------
    // A ktable, by name (".rock") or by the id its KTableDesc::id slot received. Write
    // access (a declared write on the ktable) is the KTable itself: spawn and destroy now,
    // with the handle at once, and every column. Read access is the const view. Null if
    // not declared.
    //   Handle b = ctx.write_ktable(g_bullet_ktable)->spawn(transform2_at(at), look, Velocity{v});
    KTable*       write_ktable(const char* name) { return ctx_ktable(*this, name, true); }
    KTable*       write_ktable(u32 ktable_id) { return ctx_ktable(*this, ktable_id, true); }
    const KTable* read_ktable(const char* name) { return ctx_ktable(*this, name, false); }
    const KTable* read_ktable(u32 ktable_id) { return ctx_ktable(*this, ktable_id, false); }

    // One ktable's column by name, ".rock.Health" (or its id), over every row by slot. A
    // handle from a delivered event is live, so its index() is a valid row:
    //   static const u32 k_hp = ctx.column_id(".rock.Health");
    //   Span<Health> hp = ctx.write<Health>(k_hp);
    //   hp[hit.rock.index()].hp -= 1;
    template <class T, class K>
    Span<const T> read(K column) {
        u32 n = 0;
        Column* c = ctx_column(*this, column, &TypeKey<T>::key, false, &n);
        return c ? Span<const T>((const T*)c->data, n) : Span<const T>();
    }
    template <class T, class K>
    Span<T> write(K column) {
        u32 n = 0;
        Column* c = ctx_column(*this, column, &TypeKey<T>::key, true, &n);
        return c ? Span<T>((T*)c->data, n) : Span<T>();
    }
    // Every ktable that has all of C..., one KTableRows each; a const C is read, the others
    // written. The usual way a system walks entities, whichever module declared the ktables:
    //   for (KTableRows k : ctx.view<Transform2, const Velocity>()) {
    //       Span<Transform2> t = k.col<Transform2>();
    //       Span<const Velocity> v = k.col<const Velocity>();
    //       for (u32 i = 0; i < k.rows(); ++i) if (k.alive(i)) t[i].position += v[i].v * (f32)ctx.dt;
    //   }
    template <class... C>
    KTableRange<C...> view() { return KTableRange<C...>{this}; }
    // Whether an entity is alive: a read of its ktable's liveness ("ktable.@alive", or the
    // ktable).
    bool alive(Handle h) { return ctx_alive(*this, h); }
    // One ktable's rows, as a view would give them: its liveness read here, each column as
    // it is taken (k.col<const T>()). For a snapshot ktable read early, the rows the phase
    // began with.
    template <class K>
    KTableRows rows(K ktable) { KTable* t = ctx_rows(*this, ktable_of(ktable)); return KTableRows{this, t}; }

    // How many entities of a ktable (name or id) are alive: a read of its liveness
    // ("ktable.@alive", or the ktable), checked like any read. For counts and stats without
    // reaching around the context to the KTable.
    template <class K>
    u32 live(K ktable) { KTable* t = ctx_rows(*this, ktable_of(ktable)); return t ? t->live() : 0; }

    // A published resource (engine_publish), by name or id; T const is a read, T a write.
    // Null, with a log line, if nothing is published under that name yet.
    //   const Input* in = ctx.resource<const Input>("input.state");
    template <class T, class K>
    T* resource(K name) {
        typedef typename RemoveConst<T>::type U;
        return (T*)ctx_resource(*this, id_of(name), &TypeKey<U>::key, !RemoveConst<T>::is_const);
    }

    // A record for an append resource (declared in `appends`), handed to its owner at the
    // phase boundary in unit order. By name or id; false if not declared. Modules with an
    // append resource usually wrap this (audio_play).
    template <class T, class K>
    bool append(K resource, const T& record) {
        static_assert(__is_trivially_copyable(T), "append plain data");
        return ctx_append(*this, id_of(resource), &record, (u32)sizeof(T));
    }
    // Whether this unit may append to the resource: for a wrapper that checks first.
    template <class K>
    bool may_append(K resource) { return ctx_may_append(*this, id_of(resource)); }

    // Names to ids, for code that would rather not look a name up every call:
    //   static const u32 k_bounces = ctx.column_id(".ball.Bounces");
    // Ids are stable for the life of the engine and carry it: one kept across into
    // another engine is caught (dev builds) rather than reaching the wrong resource.
    // A program that runs one engine can keep them in statics, as above.
    u32 resource_id(const char* name) { return ctx_resource_id(*this, name, false); }
    u32 column_id(const char* name) { return ctx_resource_id(*this, name, true); } // asserts if not a column
    u32 ktable_id(const char* name);

    // ---- per entity: attachments and property bags -------------------------------------
    // For data one entity of a ktable has and the rest don't (a quest on one villager, what
    // a designer typed on one map object). An attachment is one component stored beside
    // the ktable's columns, for the ktables whose KTableDesc::attachable lists it; a bag is
    // named values, for ktables with KTableDesc::props. Both are declared in the sets as
    // "ktable.@Component" and "ktable.@props" (or "*.@..."), and a write on the whole ktable
    // covers them. Changes are immediate, not commands. Both are slower than a column:
    // what every entity has belongs in the ktable.
    //   if (Talk* t = ctx.attached<Talk>(npc)) start_dialogue(t->line);
    //   ctx.attach(npc, Quest{.stage = 1});
    //   ctx.prop_set_int(chest, "gold", 30);
    //   i64 gold = ctx.prop_int(chest, "gold");      // 0 if unset
    template <class T>
    T* attached(Handle h) {
        typedef typename RemoveConst<T>::type U;
        return (T*)ctx_attached(*this, h, ComponentType<U>::id, &TypeKey<U>::key, !RemoveConst<T>::is_const);
    }
    template <class T>
    T* attach(Handle h, const T& v) { return (T*)ctx_attach(*this, h, ComponentType<T>::id, &TypeKey<T>::key, &v); }
    template <class T>
    bool detach(Handle h) { return ctx_detach(*this, h, ComponentType<T>::id); }
    template <class T, class K>
    AttachedRange<T> attached_all(K ktable) {
        typedef typename RemoveConst<T>::type U;
        AttachedRange<T> r;
        r.ktable = ctx_attachments(*this, ktable_of(ktable), ComponentType<U>::id, !RemoveConst<T>::is_const);
        r.store = r.ktable ? r.ktable->attachment(ComponentType<U>::id) : nullptr;
        return r;
    }
    // The bag. Setters return false if the entity is dead, its ktable has no bags, or the
    // unit didn't declare them. Getters
    // convert between int and float and answer `dflt` for a missing key or another type;
    // prop_handle answers a null handle.
    bool        prop_set_int(Handle h, const char* key, i64 v);
    bool        prop_set_float(Handle h, const char* key, f64 v);
    bool        prop_set_bool(Handle h, const char* key, bool v);
    bool        prop_set_str(Handle h, const char* key, const char* v); // interned: for words and ids, not prose that changes every tick
    bool        prop_set_handle(Handle h, const char* key, Handle v);
    bool        prop_remove(Handle h, const char* key);
    bool        prop_has(Handle h, const char* key);
    i64         prop_int(Handle h, const char* key, i64 dflt = 0);
    f64         prop_float(Handle h, const char* key, f64 dflt = 0.0);
    bool        prop_bool(Handle h, const char* key, bool dflt = false);
    const char* prop_str(Handle h, const char* key, const char* dflt = "");
    Handle      prop_handle(Handle h, const char* key);

    // ---- commands: applied at the phase boundary, in unit order ----------------------
    // Commands need no declaration: they take effect at the end of the phase, when nothing
    // runs, so they can't race.
    //
    // Spawns an entity of a ktable (name or id) without holding the ktable's write. It
    // exists after this phase; the Spawned event with the returned ticket carries its handle.
    // Components not given are zeroed. Returns 0 if the ktable is unknown.
    //   ctx.spawn_later(g_debris_ktable, transform2_at(p), sprite_of(look), Velocity{v, spin});
    template <class K, class... C>
    u32 spawn_later(K ktable, const C&... c) {
        u32 ticket = ctx_spawn_begin(*this, commands, ktable, sizeof...(C), true);
        if (ticket) (ctx_spawn_component(commands, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
        return ticket;
    }
    // Spawn and name: the name lands with the entity (see engine_entity_name_set).
    template <class K, class... C>
    u32 spawn_named_later(K ktable, const char* name, const C&... c) {
        u32 ticket = spawn_later(ktable, c...);
        if (ticket) ctx_name(*this, commands, Handle{}, name);
        return ticket;
    }
    // Names an entity at the phase boundary, in unit order; empty takes its name away.
    void name_later(Handle h, const char* name) { if (!h.is_null()) ctx_name(*this, commands, h, name); }
    // Destroys an entity at the phase boundary. Readers in this phase still see it;
    // events naming it are dropped from batches after it. A dead or null handle is ignored.
    void destroy(Handle h) { ctx_destroy(*this, commands, h); }
    // Every entity of a ktable, at the phase boundary. Takes a name or a ktable id.
    template <class K>
    void destroy_all(K ktable) { ctx_destroy_all(*this, commands, ktable_of(ktable)); }

    // ---- timers: events delivered at a later tick --------------------------------------
    // Emits `e` at the start of sim tick `at_tick`; a tick at or before the current one
    // means the next. schedule_in counts from now. Returns a ticket for cancel. Paused
    // ticks don't count. Like any event, one with a schema that names an entity that died
    // in the meantime is dropped rather than delivered.
    //   ctx.schedule_in(Expire{bullet}, 72);                    // 1.2 s at 60 Hz
    template <class E>
    u32 schedule(const E& e, u64 at_tick) { return ctx_schedule(*this, EventType<E>::id, (u32)sizeof(E), &e, at_tick); }
    template <class E>
    u32 schedule_in(const E& e, u64 ticks) { return schedule(e, tick + ticks); }
    // Withdraws a scheduled event by its ticket. False if it already fired, or never was.
    bool cancel(u32 ticket) { return ctx_cancel(*this, ticket); }

private:
    u32 id_of(u32 id) { return id; }
    u32 id_of(const char* name) { return ctx_resource_id(*this, name, false); }
    u32 ktable_of(u32 id) { return id; }
    u32 ktable_of(const char* name) { return ktable_id(name); }
};

// ---- parallel loops ----------------------------------------------------------------------
// Runs fn(ChunkContext& cc, u32 begin, u32 end) over [0, count) in chunks of `grain`, on
// the job pool, and returns when every chunk has run (the calling thread works too).
// Chunks may write the rows the system declared, each its own range, and emit and issue
// commands through cc; the buffers merge in chunk order, so the result does not depend on
// which thread ran what. Pick a grain of a few hundred to a few thousand rows: a chunk
// should be worth a few microseconds.
//   parallel_for(ctx, k.rows(), 512, [&](ChunkContext&, u32 begin, u32 end) {
//       for (u32 i = begin; i < end; ++i) if (k.alive(i)) t[i].position += v[i].v * dt;
//   });
template <class F>
void parallel_for(TickContext& ctx, u32 count, u32 grain, F fn) {
    u32 chunks = jobs_chunks(count, grain);
    if (chunks == 0) return;
    MemScope scope;
    ParallelBuffers pb = ctx_parallel_begin(ctx, chunks);
    struct Closure { TickContext* ctx; ParallelBuffers* pb; F* fn; } cl{&ctx, &pb, &fn};
    jobs_run(count, grain, [](u32 chunk, u32 begin, u32 end, void* user) {
        Closure& c = *(Closure*)user;
        ChunkContext cc{c.ctx, &c.pb->writers[chunk], &c.pb->commands[chunk], chunk};
        const ScheduledSystem* was = ctx_running(c.ctx->system);
        (*c.fn)(cc, begin, end);
        ctx_running(was);
    }, &cl, ctx_unit_name(ctx));
    ctx_parallel_end(ctx, pb);
}

// Like parallel_for, where each chunk returns a T and `combine(acc, t)` folds them in
// chunk order starting from `init`, so a float sum is the same on any thread count. T is
// plain data.
//   u32 alive = parallel_reduce(ctx, k.rows(), 4096, 0u,
//       [&](ChunkContext&, u32 b, u32 e) { u32 n = 0; for (u32 i = b; i < e; ++i) n += k.alive(i); return n; },
//       [](u32 a, u32 b) { return a + b; });
template <class T, class F, class R>
T parallel_reduce(TickContext& ctx, u32 count, u32 grain, T init, F fn, R combine) {
    static_assert(__is_trivially_copyable(T), "reduce over plain data");
    u32 chunks = jobs_chunks(count, grain);
    if (chunks == 0) return init;
    MemScope scope;
    ParallelBuffers pb = ctx_parallel_begin(ctx, chunks);
    T* results = alloc_items<T>(mem_scratch(), chunks);
    struct Closure { TickContext* ctx; ParallelBuffers* pb; F* fn; T* results; } cl{&ctx, &pb, &fn, results};
    jobs_run(count, grain, [](u32 chunk, u32 begin, u32 end, void* user) {
        Closure& c = *(Closure*)user;
        ChunkContext cc{c.ctx, &c.pb->writers[chunk], &c.pb->commands[chunk], chunk};
        const ScheduledSystem* was = ctx_running(c.ctx->system);
        c.results[chunk] = (*c.fn)(cc, begin, end);
        ctx_running(was);
    }, &cl, ctx_unit_name(ctx));
    ctx_parallel_end(ctx, pb);
    T acc = init;
    for (u32 i = 0; i < chunks; ++i) acc = combine(acc, results[i]);
    return acc;
}

typedef void (*SystemFn)(TickContext& ctx);

// One entry in a module's system table (example at the top of this file). Plain data,
// written with designated initializers; only name, phase and fn are required.
struct SystemDesc {
    const char* name   = nullptr;       // ".move": the module's own, in the name language (HandlerDesc)
    Phase       phase  = Phase::Count;  // required
    SystemFn    fn     = nullptr;
    // Resource names, comma-separated; see HandlerDesc. A reader runs after every writer of
    // what it reads in the phase. A read marked '^' ("^.npc.Local2") is of the value the
    // phase started with: an AI that reads where things stand to write the intent a later
    // system moves them by reads their positions that way. It may be of a ktable or its
    // columns, never of something the unit writes itself. A filter may leave a component
    // out ("*[Collider2-Parent].Transform2"). One resource, one mode: two spellings that
    // cover it differently are refused, as is a reader that wants one thing early and
    // another as it stands from the same writer (say which it means). Two writers of one
    // resource need an order. `appends` names append resources (ResourceDesc::apply),
    // which need no order at all.
    // An event type is a stream: `emits` names the types the system emits, `reads` the ones
    // it consumes (by type name, "ShotHit"), which it does after every emitter of them in
    // its phase, or with '^' up to the phase start. ctx.received<E>() is its batch.
    const char* reads  = nullptr;
    const char* writes = nullptr;
    const char* appends = nullptr;
    const char* emits  = nullptr;
    const char* after  = nullptr; // system names; needed between two writers, optional otherwise; "?name": only if that module is in the build
    const char* before = nullptr;
    StateUse                state  = StateUse::Write; // what it does with its module's state
    bool                    main_thread = false;      // runs on the thread driving the engine: for code that talks to the OS, the GPU, or a module's own pools
    RunWhen                 runs   = RunWhen::Unpaused; // whether a paused tick runs it
};

// ---- internal: the schedule as built --------------------------------------------------
// A system or handler placed in the schedule, with the module it came from and its sets
// as ids. Exactly one of desc and handler is set.
struct ScheduledSystem {
    const SystemDesc*  desc;
    ModuleInstance*    module;
    Array<UnitAccess>  declared; // as written, a name per entry, unexpanded: for people
    Array<UnitAccess>  access;   // expanded over the resource tree, by resource id: what the checks and the schedule use
    const HandlerDesc* handler = nullptr;
    const char*        full_name = nullptr; // the declared name with its module prefix expanded
    u32                unit    = 0; // 1-based, by rank among all units: systems in phase order, then handlers
    u32                level   = 0; // 1 + the longest chain of edges before it; equal levels run concurrently
    u32                rank    = 0; // module order then declaration order
    CommandBuffer      commands;
    f64                work_deadline = 0.0; // a Work unit's share of the cycle's time: when it should stop (ctx.time_left)
    u32                spawn_tickets = 0; // the unit's own ticket counters (see phase.cc)
    u32                timer_tickets = 0;
    mutable u32        in_parallel   = 0; // inside a parallel_for: chunks may not draw from ctx.rng()
    Array<KTable>      early;    // the snapshot ktables it reads early, as the phase found them
    // A stream it consumes: where its last batch ended, and this run's: its range, and the
    // events themselves less those naming the dead, made before the unit's level ran.
    struct Cursor { u32 type; bool early; u64 at, from, to; const void* data = nullptr; u32 count = 0; };
    Array<Cursor>      cursors;

    const char* name() const { return full_name ? full_name : desc ? desc->name : handler->name; }
    // The unit's mode on a resource; false when it has none.
    bool        mode(u32 resource, Access* out) const;
    void        free_buffers();
};

// A snapshot ktable's copy at a phase start, of what that phase reads early and writes.
// Liveness only when a unit spawns or destroys there.
struct KTableSnapshot {
    struct Rows { Column* column; Array<u8> bytes; };
    KTable*    ktable   = nullptr;
    bool       liveness = false;
    Array<u32> gens;       // the slots' generations and alive bits
    u32        len = 0, live = 0;
    Array<Rows> columns;
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];   // in run order: by level, then rank
    Array<KTableSnapshot>  snapshots[(u32)Phase::Count]; // taken at each phase start
    Array<ScheduledSystem*> by_unit;                      // every unit, at unit - 1: phase by phase, by rank
    u32                    first_unit[(u32)Phase::Count] = {}; // each phase's units are [first, last]
    u32                    last_unit[(u32)Phase::Count]  = {};

    u32  system_count() const;

    // Runs one tick: the timers (unpaused) and the tick start's events; then for each phase
    // its units level by level, a level concurrently, each stream its consumers need
    // merged before their level; then the phase's events, its commands and the boundary's
    // events. At the end, triggers, the hash and the streams' end of tick. A paused tick
    // runs the Always systems and the handlers with events, skips the timers, the triggers
    // and the hash line; `tick` is then the sim tick that will run next.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick, bool paused = false);
    // Runs one cycle that is a single phase outside the tick (Frame, Work), as a tick runs a
    // phase: levels, then the boundary's events and commands. Then the streams keep what a
    // consumer has not reached, the tick's own window untouched.
    void run_cycle(Engine& engine, EventBus& events, Phase phase, f64 dt);
    void free();
};

} // namespace kv

// The event and function of a handler, inside its designated initializer; fn has the
// signature void(TickContext&, Span<const E>):
//   {.name = ".on_hit", KV_ON(RockHit, on_hit), .phase = Phase::PostSim, .writes = ".rock.Health"}
#define KV_ON(E, FN) .event = &::kv::EventType<E>::id, .event_name = #E, .fn = &::kv::handler_thunk<E, FN>
