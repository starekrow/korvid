// Korvid core: the update schedule. Slice 1 is a fixed, ordered list of tick phases;
// modules place systems in them. Within a phase the order is module-list order, then
// declaration order. Systems declare read/write sets by resource name; at init the names
// become ids (a "*.Component" wildcard becomes that column in every kind), and in dev
// builds every access through the TickContext is checked against them. The DAG scheduler
// that orders by those sets comes next.
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
    Handle entity;
    u32    kind;   // kind id
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
    const ScheduledSystem* system = nullptr; // whose declared sets gate the accessors; null: anything goes

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

// A system placed in the schedule, with the module it came from and its sets as ids.
struct ScheduledSystem {
    const SystemDesc* desc;
    ModuleInstance*   module;
    Array<u32>        reads;  // resource ids; wildcards expanded
    Array<u32>        writes;
};

struct PhaseList {
    Array<ScheduledSystem> systems[(u32)Phase::Count];

    u32  system_count() const;

    // Runs one tick: for each phase, deliver pending events, run its systems, apply commands.
    void run_tick(Engine& engine, EventBus& events, f64 dt, u64 tick);
    void free();
};

} // namespace kv
