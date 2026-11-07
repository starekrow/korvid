// Korvid core: events are plain-data facts in typed per-tick buffers.
// Emitting is an append into a writer's pending buffer. At each phase boundary
// deliver() merges every writer into the delivered buffers in writer order, so the
// merge is deterministic. Consumers read whole batches; the batch accumulates through
// the tick and end_tick() clears it. Pending events survive the end of a tick: they are
// delivered at the next boundary. A type declared `next_tick` is held until the first
// boundary of the following tick, never delivered within the tick it was emitted.
//
// An event type may carry a field table (KV_EVENT_EX). Then the bus knows which fields
// are handles and drops, at delivery, any event whose handle no longer resolves: stale
// handles in events are dropped silently and counted. The table is also what the trace
// prints. Slice 1 is single-threaded; one writer per thread comes later without API change.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/component.h"

namespace kv {

enum EventFlags : u8 {
    EvNextTick = 1, // delivered at the first boundary of the next tick, never the same tick
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

    EventBuffer& buffer(u32 type_id, u32 elem_size);
    u32  pending_count() const;
    void clear();
    void free();
};

struct EventBus {
    Array<EventWriter> writers;
    Array<EventBuffer> delivered;
    Array<u32>         wave_start;        // by type: delivered count before the last deliver()
    Span<const EventTypeDesc* const> types; // by id, set by the engine; empty means no schemas
    u64                delivered_total = 0;
    u64                dropped_stale   = 0; // events dropped for a stale handle, ever
    u32                dropped_last    = 0; // at the last deliver()

    void init(u32 writer_count = 1);
    void free();

    EventWriter& writer(u32 i) { return writers[i]; }
    void set_emitter(const char* name) { for (EventWriter& w : writers) w.emitter = name; }

    template <class E>
    void emit(const E& e) { writers[0].emit(e); }

    // Phase boundary: move everything pending into the delivered buffers. At the first
    // boundary of a tick (tick_start) next-tick types move too; otherwise they wait.
    void deliver(bool tick_start = true);
    // End of tick: drop the delivered buffers. Pending stays pending.
    void end_tick();

    template <class E>
    Span<const E> read() const {
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0);
        if (id == 0 || id >= delivered.len) return Span<const E>();
        const EventBuffer& b = delivered[id];
        return Span<const E>((const E*)b.bytes.data, b.count());
    }
    // What the last deliver() added, for handlers: nothing is handled twice.
    u32         new_count(u32 type_id) const;
    const void* new_data(u32 type_id) const;
    template <class E>
    Span<const E> read_new() const { return Span<const E>((const E*)new_data(EventType<E>::id), new_count(EventType<E>::id)); }

    u32 pending_count() const;
    // Pending events a mid-tick deliver(false) would move: everything but next-tick types.
    u32 pending_now() const;
};

} // namespace kv

// Table entries for event types: KV_EVENT(WindowResized), or with a schema and flags:
// KV_EVENT_EX(RockHit, k_rock_hit_fields, 0), KV_EVENT_EX(ScoreChanged, k_fields, EvNextTick).
#define KV_EVENT(E) ::kv::EventTypeDesc{#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id}
#define KV_EVENT_EX(E, fields, flags) \
    ::kv::EventTypeDesc{#E, (::kv::u32)sizeof(E), (::kv::u32)alignof(E), &::kv::EventType<E>::id, fields, (::kv::u8)(flags)}
