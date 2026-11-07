// Korvid core: the update schedule. A fixed, ordered list of tick phases; modules place
// systems in them. Systems declare read/write sets by resource name; at init the names
// become ids (a "*.Component" wildcard becomes that column in every kind). Within a
// phase the order is derived: a reader runs after every writer of what it reads, `after`
// and `before` add edges by hand, two writers with no path between them or a cycle is a
// registration error, and ties break by module order then declaration order. In dev
// builds every access through the TickContext is checked against the declared sets.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/core/handle.h"
#include "korvid/core/kind.h"

namespace kv {

struct Engine;
struct ModuleInstance;
struct ScheduledSystem;
struct TickContext;

// Fixed order. Add phases here, never at runtime.
enum class Phase : u8 {
    Input,   // sample devices into per-tick input state
    PreSim,  // snapshot previous state, advance timers
    Sim,     // gameplay
    PostSim, // reactions to this tick's results
    Count
};
const char* phase_name(Phase p);

// Emitted by the engine when a command spawn is applied, so the issuer learns the handle.
struct Spawned {
    u32    ticket; // what spawn_later returned
    u32    kind;   // kind id
    Handle entity; // null if the kind was full
};

// Emitted by the engine at the end of a tick for every state trigger that fired,
// delivered at the start of the next.
struct StateEdge {
    Handle entity;
    u32    trigger; // trigger id
    u32    pad;
    f64    value;   // the field's value after the edge
};

// A handler: subscribed to one event type, run at phase boundaries on each new batch of
// it, in waves, under the same read/write rules as a system. Referenced by its id in the
// table, never by function pointer in sim state.
//   void on_hit(TickContext& ctx, Span<const RockHit> hits);
//   {.name = "rocks.on_hit", KV_ON(RockHit, on_hit), .reads = ..., .writes = ...}
typedef void (*HandlerFn)(TickContext& ctx, const void* events, u32 count);
template <class E, void (*F)(TickContext&, Span<const E>)>
void handler_thunk(TickContext& ctx, const void* events, u32 count) { F(ctx, Span<const E>((const E*)events, count)); }

struct HandlerDesc {
    const char*             name       = nullptr;
    u32*                    event      = nullptr; // &EventType<E>::id
    const char*             event_name = nullptr;
    HandlerFn               fn         = nullptr;
    Span<const char* const> reads      = {};
    Span<const char* const> writes     = {};
    Span<const char* const> after      = {}; // handler names
    Span<const char* const> before     = {};
};

// A state trigger: an edge on one scalar field of a component that keeps a previous
// copy, evaluated at the end of every tick over every live row, emitting StateEdge.
enum class TriggerEdge : u8 {
    Changed,      // the field differs from the previous tick
    CrossedBelow, // previous >= value, current < value
    CrossedAbove, // previous <= value, current > value
};
struct TriggerDesc {
    const char* name  = nullptr;
    const char* field = nullptr; // "kind.Component.field"
    TriggerEdge edge  = TriggerEdge::Changed;
    f64         value = 0.0;
    u32*        id    = nullptr; // optional: filled with the trigger id at init
};

template <class T> struct RemoveConst          { typedef T type; static constexpr bool is_const = false; };
template <class T> struct RemoveConst<const T> { typedef T type; static constexpr bool is_const = true; };

// ---- the pieces behind the templates below; plain functions in phase.cc -------------
// Each returns null (or false) after logging and, in dev builds, asserting, when the
// system did not declare the access or the type does not match the schema.
Table*  ctx_kind(TickContext& ctx, const char* kind, bool write);
Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows);
Column* ctx_column_in(TickContext& ctx, Table& table, u32 component_id, const void* type, bool write);
u32     ctx_table_count(TickContext& ctx);
Table*  ctx_table_at(TickContext& ctx, u32 i);
u32     ctx_spawn_begin(TickContext& ctx, const char* kind, u32 component_count); // ticket; 0 if unknown kind
void    ctx_spawn_component(TickContext& ctx, u32 component_id, const void* data, u32 size);
void    ctx_destroy(TickContext& ctx, Handle h);
u32     ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick);

// One kind's rows for a view: the live mask and every column the view named.
struct KindRows {
    TickContext* ctx;
    Table*       table;

    u32    rows() const { return table->rows(); }
    bool   alive(u32 i) const { return table->is_alive(i); }
    Handle handle_at(u32 i) const { return table->handle_at(i); }
    const KindDesc& kind() const { return *table->desc; }

    // T const is a read, T is a write; checked against the system's sets in dev builds.
    template <class T>
    Span<T> col() const {
        typedef typename RemoveConst<T>::type U;
        Column* c = ctx_column_in(*ctx, *table, ComponentType<U>::id, &TypeKey<U>::key, !RemoveConst<T>::is_const);
        return c ? Span<T>((T*)c->data, table->rows()) : Span<T>();
    }
    // The previous-tick rows, for components that keep them. Always a read.
    template <class T>
    Span<const T> prev() const {
        typedef typename RemoveConst<T>::type U;
        Column* c = ctx_column_in(*ctx, *table, ComponentType<U>::id, &TypeKey<U>::key, false);
        return c && c->prev ? Span<const T>((const T*)c->prev, table->rows()) : Span<const T>();
    }
};

// Every kind that has all of C...: for (KindRows k : ctx.view<Transform2, const Velocity>()).
template <class... C>
struct KindRange {
    TickContext* ctx;

    struct Iter {
        TickContext* ctx;
        u32          i, n;
        void settle() {
            for (; i < n; ++i) {
                Table* t = ctx_table_at(*ctx, i);
                if ((t->has(ComponentType<typename RemoveConst<C>::type>::id) && ...)) return;
            }
        }
        KindRows operator*() const { return KindRows{ctx, ctx_table_at(*ctx, i)}; }
        Iter&    operator++() { ++i; settle(); return *this; }
        bool     operator!=(const Iter& o) const { return i != o.i; }
    };
    Iter begin() const { Iter it{ctx, 0, ctx_table_count(*ctx)}; it.settle(); return it; }
    Iter end() const { return Iter{ctx, ctx_table_count(*ctx), ctx_table_count(*ctx)}; }
};

struct TickContext {
    Engine*                engine;
    EventBus*              events;
    f64                    dt;
    u64                    tick;
    void*                  state;      // the running system's module state (ModuleInstance::state)
    const void*            state_type; // its type key, recorded when startup stored it
    const ScheduledSystem* system = nullptr; // the running system or handler, whose sets gate the accessors; null: anything goes
    u32                    wave   = 0;       // for a handler: which wave of the boundary this is

    // The module state as the type startup stored. Verified by a pointer compare.
    template <class T>
    T& state_as() const {
        KV_VERIFY(state && state_type == type_key<T>());
        return *(T*)state;
    }

    // ---- data access ---------------------------------------------------------------
    // A kind by name. Write access (declared write on "kind") is the table itself: spawn,
    // destroy and every column. Read access is the const view.
    Table*       write_kind(const char* kind) { return ctx_kind(*this, kind, true); }
    const Table* read_kind(const char* kind) { return ctx_kind(*this, kind, false); }

    // One column by resource name, "kind.Component", over [0, rows()) by slot.
    template <class T>
    Span<const T> read(const char* column) {
        u32 n = 0;
        Column* c = ctx_column(*this, column, &TypeKey<T>::key, false, &n);
        return c ? Span<const T>((const T*)c->data, n) : Span<const T>();
    }
    template <class T>
    Span<T> write(const char* column) {
        u32 n = 0;
        Column* c = ctx_column(*this, column, &TypeKey<T>::key, true, &n);
        return c ? Span<T>((T*)c->data, n) : Span<T>();
    }
    // Every kind that has all of C...; the constness of each C is the access mode.
    template <class... C>
    KindRange<C...> view() { return KindRange<C...>{this}; }

    // ---- commands: applied at the phase boundary, in issue order ---------------------
    // Spawn without holding the write: the entity exists after this phase, and a Spawned
    // event with this ticket carries its handle. Components not given stay zero.
    template <class... C>
    u32 spawn_later(const char* kind, const C&... c) {
        u32 ticket = ctx_spawn_begin(*this, kind, sizeof...(C));
        if (ticket) (ctx_spawn_component(*this, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
        return ticket;
    }
    // Destroy at the phase boundary: readers in this phase still see the entity.
    void destroy(Handle h) { ctx_destroy(*this, h); }

    // ---- scheduled events: delivered at the first boundary of `tick` -------------------
    // A tick at or before the current one means the next tick. Returns a ticket.
    template <class E>
    u32 schedule(const E& e, u64 at_tick) { return ctx_schedule(*this, EventType<E>::id, (u32)sizeof(E), &e, at_tick); }
    template <class E>
    u32 schedule_in(const E& e, u64 ticks) { return schedule(e, tick + ticks); }
};

typedef void (*SystemFn)(TickContext& ctx);

// One entry in a module's system table. Plain data; constinit-able; named fields.
struct SystemDesc {
    const char*             name   = nullptr;
    Phase                   phase  = Phase::Count;
    SystemFn                fn     = nullptr;
    Span<const char* const> reads  = {};
    Span<const char* const> writes = {};
    Span<const char* const> after  = {}; // system names; needed between two writers, optional otherwise
    Span<const char* const> before = {};
};

// A system or handler placed in the schedule, with the module it came from and its sets
// as ids. Exactly one of desc and handler is set.
struct ScheduledSystem {
    const SystemDesc*  desc;
    ModuleInstance*    module;
    Array<u32>         reads;  // resource ids; wildcards expanded
    Array<u32>         writes;
    const HandlerDesc* handler = nullptr;

    const char* name() const { return desc ? desc->name : handler->name; }
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];

    u32  system_count() const;

    // Runs one tick: for each phase, the boundary (fire timers at the first, deliver, run
    // handlers in waves, apply commands), then its systems and their commands. At the end,
    // triggers, the hash and end_tick.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick);
    void free();
};

} // namespace kv

// The event and function of a handler, inside its designated initializer:
//   {.name = "rocks.on_hit", KV_ON(RockHit, on_hit), .reads = ...}
#define KV_ON(E, FN) .event = &::kv::EventType<E>::id, .event_name = #E, .fn = &::kv::handler_thunk<E, FN>
