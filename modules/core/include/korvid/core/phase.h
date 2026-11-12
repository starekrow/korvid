// Korvid core: the update schedule. A fixed, ordered list of tick phases; modules place
// systems in them. Systems declare what they touch by resource name, each in one mode:
// read (the value after every writer in the phase), read early ("^x": the value the
// phase started with), write, or append (records handed to the resource's owner at the
// boundary). At init the names become ids, expanded once over the resource tree (a kind
// covers its liveness, columns, attachments and bags; an early read of a kind covers its
// liveness and columns), and that one expansion is what both the schedule and the dev
// checks at access time use. Within a phase the order is solved from the declarations:
// readers after writers, `after` and `before`, and a module's systems that share its
// state in declaration order unless something else orders them; writers of one resource
// need an order, cycles are refused, ties break by module then declaration order.
// Systems with no path between them share a level and run concurrently on the job pool.
// How an early read gets its value is chosen per kind (EarlyReads): by running the reader
// before the writers, or from a copy taken at the phase start. The values are the same
// either way, so a game means the same whatever the choice. docs/design/scheduler.md has
// the whole model.
//
// Every system and handler (a "unit") owns an event writer and a command buffer, and
// each chunk of a parallel_for owns a pair of its own, merged into the unit's in chunk
// order. Units are numbered by declaration rank, not by where the schedule puts them, so
// what a tick emits, spawns and appends comes out in the same order however it ran.
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

// What a system does with its module's state (ModuleInstance::state), which is a resource
// like any other ("<module>.state"). Write, the default, reaches it as state_as<T>(); Read
// as state_as<const T>() only, beside the module's other readers; None not at all.
// Handlers run one at a time and always have it.
enum class StateUse : u8 { Write, Read, None };

// How a unit touches one resource. A unit has one mode per resource; a write includes
// reading it as it stands.
enum class Access : u8 {
    Read,   // as it stands after every writer in the phase
    Early,  // as the phase found it ('^' in the reads)
    Write,
    Append, // records for the owner, applied at the boundary (ResourceDesc::apply)
};
const char* access_name(Access a);
struct UnitAccess {
    u32    resource;
    Access mode;
};

// How early reads get their value: as each kind says (KindDesc::early_reads), or one way
// for every kind, to compare the two. The game is the same either way.
enum class EarlyReadsChoice : u8 { PerKind, Order, Snapshot };
const char* early_reads_name(EarlyReadsChoice m);

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

// Emitted by engine_sim_reset, for the module state that follows the sim (a broadphase's
// last pairs, a cache keyed by handle): the handles it knew are about to be issued again.
struct SimReset {
    u32 resets; // how many there have been, this one included
    u32 pad;
};

// A handler: subscribed to one event type, run at phase boundaries on each new batch of
// it, in waves, under the same read/write rules as a system. Referenced by its id in the
// table, never by function pointer in sim state.
//   void on_hit(TickContext& ctx, Span<const RockHit> hits);
//   {.name = "rocks.on_hit", KV_ON(RockHit, on_hit), .reads = ..., .writes = ...}
typedef void (*HandlerFn)(TickContext& ctx, const void* events, u32 count);
template <class E, void (*F)(TickContext&, Span<const E>)>
void handler_thunk(TickContext& ctx, const void* events, u32 count) { F(ctx, Span<const E>((const E*)events, count)); }

// Names in these tables are strings in the engine's name language: a name may start with
// '.' for "<module>." (".on_hit" in module "rocks" is "rocks.on_hit"; "?.foo" is an
// optional order on "<module>.foo"), and a set is one string of comma-separated names
// (".ship, .bullet.Transform2, *.Velocity, input.state"). The engine expands them at
// init; an unknown name fails it with a message, as before.
struct HandlerDesc {
    const char* name       = nullptr;
    u32*        event      = nullptr; // &EventType<E>::id
    const char* event_name = nullptr;
    HandlerFn   fn         = nullptr;
    const char* reads      = nullptr; // resource names, comma-separated; no early reads ('^') at a boundary
    const char* writes     = nullptr;
    const char* appends    = nullptr;
    const char* after      = nullptr; // handler names
    const char* before     = nullptr;
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
// Names an entity at the boundary; a null entity names the last one this buffer spawned.
void command_name(CommandBuffer& cb, Handle h, u64 name_hash);
// A record for an append resource's owner, handed to its apply function at the boundary.
void command_append(CommandBuffer& cb, u32 resource_id, const void* record, u32 size);

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
void    ctx_name(TickContext& ctx, CommandBuffer* cb, Handle h, const char* name);
u32     ctx_schedule(TickContext& ctx, u32 type_id, u32 size, const void* event, u64 tick);
void*   ctx_attached(TickContext& ctx, Handle h, u32 component_id, const void* type, bool write);
void*   ctx_attach(TickContext& ctx, Handle h, u32 component_id, const void* type, const void* data);
bool    ctx_detach(TickContext& ctx, Handle h, u32 component_id);
Table*  ctx_attachments(TickContext& ctx, u32 kind_id, u32 component_id, bool write); // the table, its store's access checked
Table*  ctx_props(TickContext& ctx, Handle h, bool write);                          // the entity's table, its bags' access checked
bool    ctx_cancel(TickContext& ctx, u32 ticket);
bool    ctx_state_allowed(const TickContext& ctx, bool write); // logs and asserts when not
bool    ctx_may_append(TickContext& ctx, u32 resource_id); // declared in `appends` (or the owner's write); logs and asserts when not
bool    ctx_alive(TickContext& ctx, Handle h);             // a read of the kind's liveness (kind.@alive), checked
Table*  ctx_rows(TickContext& ctx, u32 kind_id);           // likewise: the unit's view of the kind, for its rows
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

// One kind's attachments of a component, live ones only, in slot order:
//   for (Attached<Talk> a : ctx.attached_all<Talk>(".npc")) use(a.entity, *a.value);
template <class T>
struct Attached { Handle entity; T* value; };
template <class T>
struct AttachedRange {
    Table*       table = nullptr;
    Attachment*  store = nullptr;
    struct Iter {
        Table* t; Attachment* a; u32 i;
        void settle() { while (a && i < a->rows.count() && !t->sparse_live(a->rows, i)) ++i; }
        Attached<T> operator*() const { return Attached<T>{t->handle_at(a->rows.slots[i]), (T*)a->rows.at(i)}; }
        Iter& operator++() { ++i; settle(); return *this; }
        bool  operator!=(const Iter& o) const { return i != o.i; }
    };
    Iter begin() const { Iter it{table, store, 0}; it.settle(); return it; }
    Iter end() const { return Iter{table, store, store ? store->rows.count() : 0}; }
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

    // The module state as the type startup stored, verified by a pointer compare. T const
    // is a read; T is a write. Checked against the system's `state` in dev builds.
    template <class T>
    T& state_as() const {
        typedef typename RemoveConst<T>::type U;
        static_assert(sizeof(U) > 0, "a complete type");
        KV_VERIFY(state && state_type == type_key<U>());
        ctx_state_allowed(*this, !RemoveConst<T>::is_const);
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
    // Whether an entity is alive: a read of its kind's liveness ("kind.@alive", or the kind).
    bool alive(Handle h) { return ctx_alive(*this, h); }
    // One kind's rows, as a view would give them: its liveness read here, each column as it
    // is taken (k.col<const T>()). For a snapshot kind read early, the rows the phase began with.
    template <class K>
    KindRows rows(K kind) { Table* t = ctx_rows(*this, kind_of(kind)); return KindRows{this, t}; }

    // A published resource (engine_publish), by name or id; T const is a read, T a write.
    // Null, with a log line, if nothing is published under that name yet.
    template <class T, class K>
    T* resource(K name) {
        typedef typename RemoveConst<T>::type U;
        return (T*)ctx_resource(*this, id_of(name), &TypeKey<U>::key, !RemoveConst<T>::is_const);
    }

    // A record for an append resource (declared in `appends`), handed to its owner at
    // the phase boundary in unit order. By name or id; false if not declared.
    template <class T, class K>
    bool append(K resource, const T& record) {
        static_assert(__is_trivially_copyable(T), "append plain data");
        return ctx_append(*this, id_of(resource), &record, (u32)sizeof(T));
    }
    template <class K>
    bool may_append(K resource) { return ctx_may_append(*this, id_of(resource)); }

    // Names to ids, for code that would rather not look a name up every call:
    //   static const u32 k_bounces = ctx.column_id("hello.ball.Bounces");
    // Ids are stable for the life of the engine and carry it: one kept across into
    // another engine is caught (dev builds) rather than reaching the wrong resource.
    u32 resource_id(const char* name) { return ctx_resource_id(*this, name, false); }
    u32 column_id(const char* name) { return ctx_resource_id(*this, name, true); } // asserts if not a column
    u32 kind_id(const char* name);

    // ---- per entity: attachments and property bags -------------------------------------
    // Under "kind.@Component" and "kind.@props" (or "*.@Component", "*.@props") in the sets;
    // a write on the whole kind covers them. Immediate, like a direct spawn: the writer owns
    // them for the phase. A kind attaches only what its KindDesc::attachable lists.
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
    AttachedRange<T> attached_all(K kind) {
        typedef typename RemoveConst<T>::type U;
        AttachedRange<T> r;
        r.table = ctx_attachments(*this, kind_of(kind), ComponentType<U>::id, !RemoveConst<T>::is_const);
        r.store = r.table ? r.table->attachment(ComponentType<U>::id) : nullptr;
        return r;
    }
    // The bag. Getters convert between int and float and answer `dflt` for a missing key or
    // another type; prop_str answers "" for none.
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

    // ---- commands: applied at the phase boundary, in issue order ---------------------
    // Spawn without holding the write: the entity exists after this phase, and a Spawned
    // event with this ticket carries its handle. Components not given stay zero.
    template <class K, class... C>
    u32 spawn_later(K kind, const C&... c) {
        u32 ticket = ctx_spawn_begin(*this, commands, kind, sizeof...(C), true);
        if (ticket) (ctx_spawn_component(commands, ComponentType<C>::id, &c, (u32)sizeof(C)), ...);
        return ticket;
    }
    // Spawn and name: the name lands with the entity (see engine_entity_name_set).
    template <class K, class... C>
    u32 spawn_named_later(K kind, const char* name, const C&... c) {
        u32 ticket = spawn_later(kind, c...);
        if (ticket) ctx_name(*this, commands, Handle{}, name);
        return ticket;
    }
    // Names an entity at the phase boundary, in unit order; empty takes its name away.
    void name_later(Handle h, const char* name) { if (!h.is_null()) ctx_name(*this, commands, h, name); }
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
        const ScheduledSystem* was = ctx_running(c.ctx->system);
        (*c.fn)(cc, begin, end);
        ctx_running(was);
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

// One entry in a module's system table. Plain data; constinit-able; named fields.
struct SystemDesc {
    const char* name   = nullptr;
    Phase       phase  = Phase::Count;
    SystemFn    fn     = nullptr;
    // Resource names, comma-separated; see HandlerDesc. A reader runs after every writer of
    // what it reads in the phase. A read marked '^' ("^.npc.Local2") is of the value the
    // phase started with: an AI that reads where things stand to write the intent a later
    // system moves them by reads their positions that way. It may be of a kind or its
    // columns, never of something the unit writes itself. A filter may leave a component
    // out ("*[Collider2-Parent].Transform2"). One resource, one mode: two spellings that
    // cover it differently are refused, as is a reader that wants one thing early and
    // another as it stands from the same writer (say which it means). Two writers of one
    // resource need an order. `appends` names append resources (ResourceDesc::apply),
    // which need no order at all.
    const char* reads  = nullptr;
    const char* writes = nullptr;
    const char* appends = nullptr;
    const char* after  = nullptr; // system names; needed between two writers, optional otherwise; "?name": only if that module is in the build
    const char* before = nullptr;
    StateUse                state  = StateUse::Write; // what it does with its module's state
    bool                    main_thread = false;      // runs on the thread driving the engine: for code that talks to the OS, the GPU, or a module's own pools
    RunWhen                 runs   = RunWhen::Unpaused; // whether a paused tick runs it
};

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
    u32                spawn_tickets = 0; // the unit's own ticket counters (see phase.cc)
    u32                timer_tickets = 0;
    Array<Table>       early;    // the snapshot kinds it reads early, as the phase found them

    const char* name() const { return full_name ? full_name : desc ? desc->name : handler->name; }
    // The unit's mode on a resource; false when it has none.
    bool        mode(u32 resource, Access* out) const;
    void        free_buffers();
};

// A snapshot kind's copy at a phase start, of what that phase reads early and writes.
// Liveness only when a unit spawns or destroys there.
struct KindSnapshot {
    struct Rows { Column* column; Array<u8> bytes; };
    Table*     table    = nullptr;
    bool       liveness = false;
    Array<u32> gens;       // the slots' generations and alive bits
    u32        len = 0, live = 0;
    Array<Rows> columns;
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];   // in run order: by level, then rank
    Array<KindSnapshot>    snapshots[(u32)Phase::Count]; // taken at each phase start
    Array<ScheduledSystem*> by_unit;                      // every unit, systems then handlers, at unit - 1

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
