// Korvid core: the update schedule. A fixed, ordered list of tick phases; modules place
// systems in them. Systems declare read/write sets by resource name; at init the names
// become ids (a "*.Component" wildcard becomes that column in every kind). Within a
// phase the order is derived: a reader runs after every writer of what it reads, `after`
// and `before` add edges by hand, two writers with no path between them or a cycle is a
// registration error, and ties break by module order then declaration order. Systems
// with no path between them share a level and run concurrently on the job pool. A
// module's own state is an implicit resource every system of the module writes unless
// it says otherwise (`state`), so two systems of one module never race on it. In dev
// builds every access through the TickContext is checked against the declared sets.
//
// Every system and handler (a "unit") owns an event writer and a command buffer, and
// each chunk of a parallel_for owns a pair of its own, merged into the unit's in chunk
// order, so what a tick emits and spawns comes out in the same order whatever ran where.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/event.h"
#include "korvid/core/handle.h"
#include "korvid/core/job.h"
#include "korvid/core/kind.h"
#include "korvid/core/arena.h"

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

// What a system does with its module's state (ModuleInstance::state). Write, the default,
// serializes it against every other system of the module; None lets it run beside them.
enum class StateUse : u8 { Write, Read, None };

// When a system runs. Unpaused, the default, is gameplay: skipped on a paused tick along
// with the timer wheel and the triggers. Always is for what keeps a paused game alive:
// input sampling, asset pumps, the hierarchy, text, animation on the real clock, audio,
// the extract, the CLI. Handlers always run: events come only from systems that ran.
enum class RunWhen : u8 { Unpaused, Always };

// Emitted by the engine when a command spawn is applied, so the issuer learns the handle.
struct Spawned {
    u32    ticket; // what spawn_later returned; 0 for a spawn issued from a parallel chunk
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

// Spawns and destroys issued through a context, applied at phase boundaries in issue
// order: the engine's own buffer first (code outside the schedule), then every unit's
// in unit order.
struct CommandBuffer {
    Array<u8> bytes;
    u32       count       = 0;
    u32       next_ticket = 1; // for the engine's own buffer; units bring their own tickets
};
u32  command_spawn_begin(CommandBuffer& cb, u32 kind_id, u32 component_count, u32 ticket); // ticket 0: the buffer assigns
void command_spawn_component(CommandBuffer& cb, u32 component_id, const void* data, u32 size);
void command_destroy(CommandBuffer& cb, Handle h);
void command_destroy_all(CommandBuffer& cb, u32 kind_id);

template <class T> struct RemoveConst          { typedef T type; static constexpr bool is_const = false; };
template <class T> struct RemoveConst<const T> { typedef T type; static constexpr bool is_const = true; };

// ---- the pieces behind the templates below; plain functions in phase.cc -------------
// Each returns null (or false) after logging and, in dev builds, asserting, when the
// system did not declare the access or the type does not match the schema.
Table*  ctx_kind(TickContext& ctx, const char* kind, bool write);
Table*  ctx_kind(TickContext& ctx, u32 kind_id, bool write);
Column* ctx_column(TickContext& ctx, const char* column, const void* type, bool write, u32* rows);
Column* ctx_column(TickContext& ctx, u32 resource_id, const void* type, bool write, u32* rows);
Column* ctx_column_in(TickContext& ctx, Table& table, u32 component_id, const void* type, bool write);
void*   ctx_resource(TickContext& ctx, u32 resource_id, const void* type, bool write);
u32     ctx_resource_id(TickContext& ctx, const char* name, bool column);
u32     ctx_table_count(TickContext& ctx);
Table*  ctx_table_at(TickContext& ctx, u32 i);
u32     ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, const char* kind, u32 component_count, bool ticketed); // ticket; 0 if unknown or unticketed
u32     ctx_spawn_begin(TickContext& ctx, CommandBuffer* cb, u32 kind_id, u32 component_count, bool ticketed);
void    ctx_spawn_component(CommandBuffer* cb, u32 component_id, const void* data, u32 size);
void    ctx_destroy(TickContext& ctx, CommandBuffer* cb, Handle h);
void    ctx_destroy_all(TickContext& ctx, CommandBuffer* cb, u32 kind_id);
u32     ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick);
bool    ctx_cancel(TickContext& ctx, u32 ticket);

// Per-chunk buffers for a parallel_for, in the caller's scratch: made before, merged after.
struct ParallelBuffers {
    EventWriter*   writers;
    CommandBuffer* commands;
    u32            chunks;
};
ParallelBuffers ctx_parallel_begin(TickContext& ctx, u32 chunks);
const char*     ctx_unit_name(TickContext& ctx); // the running unit's name, for the profiler
void            ctx_parallel_end(TickContext& ctx, ParallelBuffers& pb);

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
    // The previous-tick rows, for components that keep them. T const is a read; T is a
    // write (checked against the write set): a system writes the previous copy to say an
    // entity teleported, so nothing interpolates to where it is now.
    template <class T>
    Span<T> prev() const {
        typedef typename RemoveConst<T>::type U;
        Column* c = ctx_column_in(*ctx, *table, ComponentType<U>::id, &TypeKey<U>::key, !RemoveConst<T>::is_const);
        return c && c->prev ? Span<T>((T*)c->prev, table->rows()) : Span<T>();
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

// What one chunk of a parallel_for may do besides read and write rows: emit and issue
// commands, into its own buffers. A chunk's spawn has no ticket. Scheduling is not
// available here: do it from the system body, or emit an event and let a handler.
struct ChunkContext {
    TickContext*   ctx;
    EventWriter*   writer;
    CommandBuffer* commands;
    u32            chunk;

    template <class E>
    void emit(const E& e) { writer->emit(e); }
    template <class K, class... C>
    void spawn_later(K kind, const C&... c) {
        if (ctx_spawn_begin(*ctx, commands, kind, sizeof...(C), false) || sizeof...(C) == 0)
            (ctx_spawn_component(commands, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
    }
    void destroy(Handle h) { ctx_destroy(*ctx, commands, h); }
};

struct TickContext {
    Engine*                engine;
    EventBus*              events;
    f64                    dt;
    u64                    tick;
    void*                  state;      // the running system's module state (ModuleInstance::state)
    const void*            state_type; // its type key, recorded when startup stored it
    const ScheduledSystem* system   = nullptr; // the running system or handler, whose sets gate the accessors; null: anything goes
    EventWriter*           writer   = nullptr; // the unit's; null: the bus's writer 0 (outside the schedule)
    CommandBuffer*         commands = nullptr; // the unit's; null: the engine's
    u32                    wave     = 0;       // for a handler: which wave of the boundary this is
    bool                   paused   = false;   // a paused tick: the sim stands still, only RunWhen::Always systems run

    // The module state as the type startup stored. Verified by a pointer compare.
    template <class T>
    T& state_as() const {
        KV_VERIFY(state && state_type == type_key<T>());
        return *(T*)state;
    }

    // ---- events -----------------------------------------------------------------------
    // Into this unit's writer, delivered at the next boundary in unit order.
    template <class E>
    void emit(const E& e) { if (writer) writer->emit(e); else events->emit(e); }

    // ---- data access ---------------------------------------------------------------
    // A kind by name, or by the id its KindDesc::id slot received. Write access (declared
    // write on "kind") is the table itself: spawn, destroy and every column. Read access
    // is the const view.
    Table*       write_kind(const char* kind) { return ctx_kind(*this, kind, true); }
    Table*       write_kind(u32 kind_id) { return ctx_kind(*this, kind_id, true); }
    const Table* read_kind(const char* kind) { return ctx_kind(*this, kind, false); }
    const Table* read_kind(u32 kind_id) { return ctx_kind(*this, kind_id, false); }

    // One column by resource name, "kind.Component" (or its resource id), over
    // [0, rows()) by slot. A handle from a delivered event is live, so its index is valid.
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
    // Every kind that has all of C...; the constness of each C is the access mode.
    template <class... C>
    KindRange<C...> view() { return KindRange<C...>{this}; }

    // A published resource (engine_publish), by name or id; T const is a read, T a write.
    // Null, with a log line, if nothing is published under that name yet.
    template <class T, class K>
    T* resource(K name) {
        typedef typename RemoveConst<T>::type U;
        return (T*)ctx_resource(*this, id_of(name), &TypeKey<U>::key, !RemoveConst<T>::is_const);
    }

    // Names to ids, for code that would rather not look a name up every call:
    //   static const u32 k_bounces = ctx.column_id("hello.ball.Bounces");
    // Ids are stable for the life of the engine; a program that initializes several
    // engines in a row must not keep them across.
    u32 resource_id(const char* name) { return ctx_resource_id(*this, name, false); }
    u32 column_id(const char* name) { return ctx_resource_id(*this, name, true); } // asserts if not a column
    u32 kind_id(const char* name);

    // ---- commands: applied at the phase boundary, in issue order ---------------------
    // Spawn without holding the write: the entity exists after this phase, and a Spawned
    // event with this ticket carries its handle. Components not given stay zero.
    template <class K, class... C>
    u32 spawn_later(K kind, const C&... c) {
        u32 ticket = ctx_spawn_begin(*this, commands, kind, sizeof...(C), true);
        if (ticket) (ctx_spawn_component(commands, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
        return ticket;
    }
    // Destroy at the phase boundary: readers in this phase still see the entity.
    void destroy(Handle h) { ctx_destroy(*this, commands, h); }
    // Every entity of a kind, at the phase boundary. Takes a name or a kind id.
    template <class K>
    void destroy_all(K kind) { ctx_destroy_all(*this, commands, kind_of(kind)); }

    // ---- scheduled events: delivered at the first boundary of `tick` -------------------
    // A tick at or before the current one means the next tick. Returns a ticket.
    template <class E>
    u32 schedule(const E& e, u64 at_tick) { return ctx_schedule(*this, EventType<E>::id, (u32)sizeof(E), &e, at_tick); }
    template <class E>
    u32 schedule_in(const E& e, u64 ticks) { return schedule(e, tick + ticks); }
    // Withdraws a scheduled event by its ticket. False if it already fired, or never was.
    bool cancel(u32 ticket) { return ctx_cancel(*this, ticket); }

private:
    u32 id_of(u32 id) { return id; }
    u32 id_of(const char* name) { return ctx_resource_id(*this, name, false); }
    u32 kind_of(u32 id) { return id; }
    u32 kind_of(const char* name) { return kind_id(name); }
};

// ---- parallel loops ----------------------------------------------------------------------
// fn(ChunkContext& cc, u32 begin, u32 end) over [0, count) in chunks of grain, on the job
// pool, back when every chunk has run. Chunks emit and issue commands through cc; the
// buffers merge in chunk order, so the result does not depend on which thread ran what.
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
        (*c.fn)(cc, begin, end);
    }, &cl, ctx_unit_name(ctx));
    ctx_parallel_end(ctx, pb);
}

// Like parallel_for, where each chunk returns a T and `combine(acc, t)` folds them in
// chunk order starting from `init`. T is plain data.
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
        c.results[chunk] = (*c.fn)(cc, begin, end);
    }, &cl, ctx_unit_name(ctx));
    ctx_parallel_end(ctx, pb);
    T acc = init;
    for (u32 i = 0; i < chunks; ++i) acc = combine(acc, results[i]);
    return acc;
}

typedef void (*SystemFn)(TickContext& ctx);

// One entry in a module's system table. Plain data; constinit-able; named fields.
struct SystemDesc {
    const char*             name   = nullptr;
    Phase                   phase  = Phase::Count;
    SystemFn                fn     = nullptr;
    Span<const char* const> reads  = {};
    Span<const char* const> writes = {};
    Span<const char* const> after  = {}; // system names; needed between two writers, optional otherwise; "?name" only if that system exists
    Span<const char* const> before = {};
    StateUse                state  = StateUse::Write; // what it does with its module's state
    bool                    main_thread = false;      // runs on the thread driving the engine: for code that talks to the OS, the GPU, or a module's own pools
    RunWhen                 runs   = RunWhen::Unpaused; // whether a paused tick runs it
};

// A system or handler placed in the schedule, with the module it came from and its sets
// as ids. Exactly one of desc and handler is set.
struct ScheduledSystem {
    const SystemDesc*  desc;
    ModuleInstance*    module;
    Array<u32>         reads;  // resource ids; wildcards expanded
    Array<u32>         writes;
    const HandlerDesc* handler = nullptr;
    u32                unit    = 0; // 1-based position among all units: systems in phase order, then handlers
    u32                level   = 0; // 1 + the longest chain of edges before it; equal levels run concurrently
    u32                rank    = 0; // module order then declaration order
    CommandBuffer      commands;
    u32                spawn_tickets = 0; // the unit's own ticket counters (see phase.cc)
    u32                timer_tickets = 0;

    const char* name() const { return desc ? desc->name : handler->name; }
    void        free_buffers() { reads.free(); writes.free(); commands.bytes.free(); }
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];

    u32  system_count() const;

    // Runs one tick: for each phase, the boundary (fire timers at the first, deliver, run
    // handlers in waves, apply commands), then its systems level by level, the systems of
    // a level concurrently, and their commands. At the end, triggers, the hash and end_tick.
    // A paused tick skips the Unpaused systems, the timers, the triggers and the hash line;
    // `tick` is then the sim tick that will run next.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick, bool paused = false);
    void free();
};

} // namespace kv

// The event and function of a handler, inside its designated initializer:
//   {.name = "rocks.on_hit", KV_ON(RockHit, on_hit), .reads = ...}
#define KV_ON(E, FN) .event = &::kv::EventType<E>::id, .event_name = #E, .fn = &::kv::handler_thunk<E, FN>
