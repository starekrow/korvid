// Korvid core: events are plain-data facts in typed per-tick buffers.
// Emitting is an append into a writer's pending buffer. At each phase boundary
// deliver() merges every writer into the delivered buffers in writer order, so
// the merge is deterministic. Consumers read whole batches. end_tick() clears.
// Slice 1 is single-threaded; one writer per thread comes later without API change.
//
// Event types are registered by modules (see module.h): EventType<E>::id is 0 until
// engine_init assigns it in module-list order, and 0 again after shutdown.
#pragma once
#include "korvid/core/array.h"

namespace kv {

struct EventTypeDesc;

// Per-type id slot. Zero-initialized (constant initialization), never set by user code;
// module.h's KV_EVENT points a module's table entry at it.
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

    u32  count() const { return elem_size ? bytes.len / elem_size : 0; }
    void clear() { bytes.clear(); }
    void free()  { bytes.free(); elem_size = 0; }
};

struct EventWriter {
    Array<EventBuffer> by_type; // indexed by event id; slot 0 is never used

    template <class E>
    void emit(const E& e) {
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0); // not registered: the module list does not declare this type
        if (id == 0) return;
        EventBuffer& b = buffer(id, sizeof(E));
        u8* dst = b.bytes.push_n(sizeof(E));
        __builtin_memcpy(dst, &e, sizeof(E));
    }

    EventBuffer& buffer(u32 type_id, u32 elem_size);
    u32  pending_count() const;
    void clear();
    void free();
};

struct EventBus {
    Array<EventWriter> writers;
    Array<EventBuffer> delivered;
    u64                delivered_total = 0;

    void init(u32 writer_count = 1);
    void free();

    EventWriter& writer(u32 i) { return writers[i]; }

    template <class E>
    void emit(const E& e) { writers[0].emit(e); }

    // Phase boundary: move everything pending into the delivered buffers.
    void deliver();
    // End of tick: drop the delivered buffers.
    void end_tick();

    template <class E>
    Span<const E> read() const {
        u32 id = EventType<E>::id;
        KV_ASSERT(id != 0);
        if (id == 0 || id >= delivered.len) return Span<const E>();
        const EventBuffer& b = delivered[id];
        return Span<const E>((const E*)b.bytes.data, b.count());
    }

    u32 pending_count() const;
};

} // namespace kv
