// Korvid core: events are plain-data facts in typed per-tick buffers.
// Emitting is an append into a writer's pending buffer. At each phase boundary
// deliver() merges every writer into the delivered buffers in writer order, so
// the merge is deterministic. Consumers read whole batches. end_tick() clears.
// Slice 1 is single-threaded; one writer per thread comes later without API change.
#pragma once
#include "korvid/core/array.h"

namespace kv {

namespace detail {
u32 register_event_type(usize elem_size);
}

// Assigned on first use, in use order. Module registration will hand these out
// as data later; the call site stays the same.
template <class E>
u32 event_type_id() {
    static_assert(__is_trivially_copyable(E), "events are plain data");
    static const u32 id = detail::register_event_type(sizeof(E));
    return id;
}

u32 event_type_count();

struct EventBuffer {
    u32       elem_size = 0;
    Array<u8> bytes;

    u32  count() const { return elem_size ? bytes.len / elem_size : 0; }
    void clear() { bytes.clear(); }
    void free()  { bytes.free(); elem_size = 0; }
};

struct EventWriter {
    Array<EventBuffer> by_type;

    template <class E>
    void emit(const E& e) {
        EventBuffer& b = buffer(event_type_id<E>(), sizeof(E));
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
        u32 id = event_type_id<E>();
        if (id >= delivered.len) return Span<const E>();
        const EventBuffer& b = delivered[id];
        return Span<const E>((const E*)b.bytes.data, b.count());
    }

    u32 pending_count() const;
};

} // namespace kv
