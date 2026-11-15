// Korvid core: events are plain-data facts in typed streams. Emitting is an append into
// a writer: each unit (system or handler) has its own, and writer 0 is the engine's, for
// what happens outside the schedule. Writers merge into one stream per type in segments:
// the tick start (held next-tick events, then the engine's writer), each phase (its units
// in unit order, which is rank order, whatever ran where), and the boundary after it (the
// engine's writer again: the commands' Spawned, the pumps' results). A phase's units of
// one type merge as soon as its consumers there need them, which the schedule makes
// after its last emitter there; the rest at the phase's end. A consumer (a unit reading
// the type) has a cursor: its batch is what was merged after its last run and before
// this one, or for an early read, before its phase began. So every consumer sees every
// event once, in the same order, however the levels fell. docs/design/events.md.
//
// An event type may carry a field table (KV_EVENT_EX). Then the bus knows which fields
// are handles and drops, when a batch is made, any event whose handle no longer
// resolves, counted. The table is also what the trace prints. A type declared
// `next_tick` is held to the next tick's start.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/component.h"

namespace kv {

enum EventFlags : u8 {
    EvNextTick = 1, // delivered at the first boundary of the next tick, never the same tick
    EvStaleOk  = 2, // dropped for a stale handle as a matter of course (an overlap ending with a death): no dev warning
};

// A plain-data event type. The id slot is EventType<E>::id, filled at init.
struct EventTypeDesc {
    const char*           name;
    u32                   size;
    u32                   align;
    u32*                  id;
    Span<const FieldDesc> fields = {}; // optional schema: stale-handle drop, trace, describe
    u8                    flags  = 0;

    bool next_tick() const { return (flags & EvNextTick) != 0; }
};

// Per-type id slot. Zero-initialized (constant initialization), never set by user code;
// a module's table entry points at it.
template <class E>
struct EventType {
    static_assert(__is_trivially_copyable(E), "events are plain data");
    static inline u32 id = 0;
};

// Assigns ids first_id, first_id + 1, ... to the table in order. Returns the next free
// id. Fails (returns 0) on a duplicate name or a type registered twice. engine_init
// calls this; tests and tools may call it directly.
u32  event_types_register(Span<const EventTypeDesc> types, u32 first_id);
void event_types_unregister(Span<const EventTypeDesc> types);

struct EventBuffer {
    u32       elem_size = 0;
    Array<u8> bytes;
#if KV_DEV
    Array<const char*> emitters; // by event: who emitted it, for the trace
#endif

    u32  count() const { return elem_size ? bytes.len / elem_size : 0; }
    void clear();
    void free();
};

struct EventWriter {
    Array<EventBuffer> by_type; // indexed by event id; slot 0 is never used
    const char*        emitter = ""; // stamped on each emit in dev builds

    template <class E>
    void emit(const E& e) {
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0); // not registered: the module list does not declare this type
        if (id == 0) return;
        emit_bytes(id, sizeof(E), &e);
    }
    void emit_bytes(u32 type_id, u32 elem_size, const void* e);
    // Everything pending in `src`, after what is pending here. The chunk merge.
    void append(const EventWriter& src);

    EventBuffer& buffer(u32 type_id, u32 elem_size);
    u32  pending_count() const;
    void clear();
    void free();
};

// One type's stream: every event merged and not yet passed by every consumer, oldest
// first, numbered from the start of the run.
struct EventStream {
    EventBuffer events;          // events.bytes holds numbers [base, end())
    u64         base        = 0;
    u64         tick_start  = 0; // the first merged this tick: what read() returns
    u64         phase_start = 0; // the first merged this phase: where early batches end
    u64         traced      = 0; // the dev trace has recorded up to here
    bool        merged      = false; // this phase's units are in

    u64 end() const { return base + events.count(); }
};

struct EventBus {
    Array<EventWriter> writers;   // 0: the engine's; unit u: writers[u]
    Array<EventStream> streams;   // by type id
    EventWriter        held;      // next-tick events, for the next tick's start
    Span<const EventTypeDesc* const> types; // by id, set by the engine; empty means no schemas
    u64                delivered_total = 0; // merged, ever
    u64                dropped_stale   = 0; // dropped from a batch for a stale handle, ever
    Array<u8>          stale_warned;        // by type: dev builds warn the first time a type is dropped
    Array<u8>              held_warned;  // dev: by type, a consumer that does not run was named for holding it
    u32                main_thread     = 0; // the thread that may write the engine's writer during a tick

    void init(u32 writer_count = 1);
    void free();

    EventWriter& writer(u32 i) { return writers[i]; }
    void set_emitter(const char* name) { writers[0].emitter = name; }

    // Into the engine's writer: outside the schedule, or from a unit on the main thread
    // for a fact that is not the unit's to order (a library reporting as it goes). Lands
    // in the next segment, where no consumer of the same phase sees it.
    template <class E>
    void emit(const E& e) { check_outside(); writers[0].emit(e); }
    void check_outside() const;
    void check_read() const; // dev builds: not from inside a unit, which has its batch

    // A phase begins: early batches end at what is merged now.
    void begin_phase();
    // Units [first, last] (one phase's, in unit order) into the streams: one type, or
    // every type. Next-tick types go to `held`.
    void merge(u32 type_id, u32 first, u32 last);
    void merge_all(u32 first, u32 last);
    // The engine's writer; at a tick's start the held next-tick events go first.
    void merge_outside(bool tick_start);
    // Numbers [from, to) of one type, less any naming the dead: in place, or copied into
    // the caller's scratch when something was dropped. `count` is what is left.
    const void* batch(u32 type_id, u64 from, u64 to, u32* count);
    // End of tick: drop what every consumer has passed (`keep_from` by type: the lowest
    // cursor, or the stream's end for a type nobody reads). read() starts over.
    void end_tick(Span<const u64> keep_from);
    // After a cycle outside the tick: drops what every consumer has passed, but nothing
    // the tick's own window (tick_start, what read() returns) still holds.
    void trim(Span<const u64> keep_from);
    // Drop everything merged and pending: a fresh sim. Cursors behind the new base read
    // from it.
    void drop_all();

    // This tick's events of a type, for code outside the schedule (tools, tests); a unit
    // reads its batch through the context.
    template <class E>
    Span<const E> read() const {
        check_read();
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0);
        if (id == 0 || id >= streams.len) return Span<const E>();
        const EventStream& s = streams[id];
        u64 skip = s.tick_start > s.base ? s.tick_start - s.base : 0;
        return Span<const E>((const E*)s.events.bytes.data + skip, (u32)(s.events.count() - skip));
    }

    u32 pending_count() const; // in the writers and held
};

} // namespace kv

// Table entries for event types: KV_EVENT(WindowResized), or with a schema and flags:
// KV_EVENT_EX(RockHit, k_rock_hit_fields, 0), KV_EVENT_EX(ScoreChanged, k_fields, EvNextTick).
#define KV_EVENT(E) {#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id}
#define KV_EVENT_EX(E, fields, flags) \
    {#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id, fields, (::kv::u8)(flags)}
