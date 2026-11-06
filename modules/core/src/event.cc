#include "korvid/core/event.h"
#include "korvid/core/log.h"
#include "korvid/core/module.h"
#include <string.h>

namespace kv {

u32 event_types_register(Span<const EventTypeDesc> types, u32 first_id) {
    KV_ASSERT(first_id != 0);
    u32 next = first_id;
    for (u32 i = 0; i < types.len; ++i) {
        const EventTypeDesc& t = types[i];
        if (*t.id != 0) {
            KV_ERROR("event", "event type '%s' registered twice", t.name);
            return 0;
        }
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(types[j].name, t.name) == 0) {
                KV_ERROR("event", "duplicate event type name '%s'", t.name);
                return 0;
            }
        }
        *t.id = next++;
    }
    return next;
}

void event_types_unregister(Span<const EventTypeDesc> types) {
    for (const EventTypeDesc& t : types) *t.id = 0;
}

static void ensure_buffers(Array<EventBuffer>& arr, u32 type_id) {
    if (type_id < arr.len) return;
    arr.resize_fill(type_id + 1, EventBuffer{});
}

EventBuffer& EventWriter::buffer(u32 type_id, u32 elem_size) {
    ensure_buffers(by_type, type_id);
    EventBuffer& b = by_type[type_id];
    KV_ASSERT(b.elem_size == 0 || b.elem_size == elem_size);
    b.elem_size = elem_size;
    return b;
}

u32 EventWriter::pending_count() const {
    u32 n = 0;
    for (const EventBuffer& b : by_type) n += b.count();
    return n;
}

void EventWriter::clear() {
    for (EventBuffer& b : by_type) b.clear();
}

void EventWriter::free() {
    for (EventBuffer& b : by_type) b.free();
    by_type.free();
}

void EventBus::init(u32 writer_count) {
    KV_ASSERT(writer_count >= 1);
    for (u32 i = 0; i < writer_count; ++i) writers.push(EventWriter{});
}

void EventBus::free() {
    for (EventWriter& w : writers) w.free();
    writers.free();
    for (EventBuffer& b : delivered) b.free();
    delivered.free();
}

void EventBus::deliver() {
    // Writer order, then emission order within a writer: deterministic.
    for (EventWriter& w : writers) {
        for (u32 t = 0; t < w.by_type.len; ++t) {
            EventBuffer& src = w.by_type[t];
            if (src.bytes.len == 0) continue;
            ensure_buffers(delivered, t);
            EventBuffer& dst = delivered[t];
            KV_ASSERT(dst.elem_size == 0 || dst.elem_size == src.elem_size);
            dst.elem_size = src.elem_size;
            u8* p = dst.bytes.push_n(src.bytes.len);
            __builtin_memcpy(p, src.bytes.data, src.bytes.len);
            delivered_total += src.count();
            src.clear();
        }
    }
}

void EventBus::end_tick() {
    for (EventBuffer& b : delivered) b.clear();
}

u32 EventBus::pending_count() const {
    u32 n = 0;
    for (const EventWriter& w : writers) n += w.pending_count();
    return n;
}

} // namespace kv
