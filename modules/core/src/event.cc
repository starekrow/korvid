#include "korvid/core/event.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
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
        const char* field = nullptr;
        if (const char* problem = schema_check(t.size, t.fields, &field)) {
            if (field) KV_ERROR("event", "event type '%s': field '%s' %s", t.name, field, problem);
            else       KV_ERROR("event", "event type '%s' %s", t.name, problem);
            return 0;
        }
        *t.id = next++;
    }
    return next;
}

void event_types_unregister(Span<const EventTypeDesc> types) {
    for (const EventTypeDesc& t : types) *t.id = 0;
}

void EventBuffer::clear() {
    bytes.clear();
#if KV_DEV
    emitters.clear();
#endif
}

void EventBuffer::free() {
    bytes.free();
#if KV_DEV
    emitters.free();
#endif
    elem_size = 0;
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

void EventWriter::emit_bytes(u32 type_id, u32 elem_size, const void* e) {
    EventBuffer& b = buffer(type_id, elem_size);
    memcpy(b.bytes.push_n(elem_size), e, elem_size);
#if KV_DEV
    if (!dev_spike_hunting()) b.emitters.push(emitter);
#endif
}

void EventWriter::append(const EventWriter& src) {
    for (u32 t = 0; t < src.by_type.len; ++t) {
        const EventBuffer& s = src.by_type[t];
        if (s.bytes.len == 0) continue;
        EventBuffer& d = buffer(t, s.elem_size);
        memcpy(d.bytes.push_n(s.bytes.len), s.bytes.data, s.bytes.len);
#if KV_DEV
        for (u32 i = 0; i < s.emitters.len; ++i) d.emitters.push(s.emitters[i]);
#endif
    }
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
    wave_start.free();
    stale_warned.free();
}

namespace {

bool handle_is_live(Handle& h, void*) { return h.is_null() || kv_handle_resolve(h.bits) != nullptr; }

} // namespace

void EventBus::deliver(bool tick_start) {
    // Where this delivery starts, per type, so handlers see only what is new.
    u32 ntypes = delivered.len;
    for (const EventWriter& w : writers) if (w.by_type.len > ntypes) ntypes = w.by_type.len;
    wave_start.resize_fill(ntypes, 0);
    for (u32 t = 0; t < ntypes; ++t) wave_start[t] = t < delivered.len ? delivered[t].count() : 0;
    dropped_last = 0;

    // Writer order, then emission order within a writer: deterministic.
    for (EventWriter& w : writers) {
        for (u32 t = 0; t < w.by_type.len; ++t) {
            EventBuffer& src = w.by_type[t];
            if (src.bytes.len == 0) continue;
            const EventTypeDesc* desc = t < types.len ? types[t] : nullptr;
            if (desc && desc->next_tick() && !tick_start) continue; // held for the next tick
            ensure_buffers(delivered, t);
            EventBuffer& dst = delivered[t];
            KV_ASSERT(dst.elem_size == 0 || dst.elem_size == src.elem_size);
            dst.elem_size = src.elem_size;
            u32 n = src.count();
            if (desc && schema_has_handles(desc->fields)) {
                // One at a time: an event naming the dead is dropped, and counted.
                for (u32 i = 0; i < n; ++i) {
                    u8* e = src.bytes.data + (usize)i * src.elem_size;
                    if (!schema_each_handle(desc->fields, e, handle_is_live, nullptr)) {
                        ++dropped_stale;
                        ++dropped_last;
#if KV_DEV
                        // Right for an OverlapEnd whose entity died; a trap for an event about a
                        // death that its own emitter caused. Said once a type, in dev builds.
                        stale_warned.resize_fill(t + 1, 0);
                        if (!stale_warned[t] && !(desc->flags & EvStaleOk)) {
                            stale_warned[t] = 1;
                            KV_WARN("events", "a %s was dropped at delivery: a handle in it was stale (said once for this type; EventBus::dropped_stale counts)",
                                    desc->name);
                        }
#endif
                        continue;
                    }
                    memcpy(dst.bytes.push_n(src.elem_size), e, src.elem_size);
#if KV_DEV
                    if (i < src.emitters.len) dst.emitters.push(src.emitters[i]);
#endif
                    ++delivered_total;
                }
            } else {
                memcpy(dst.bytes.push_n(src.bytes.len), src.bytes.data, src.bytes.len);
#if KV_DEV
                for (u32 i = 0; i < src.emitters.len; ++i) dst.emitters.push(src.emitters[i]);
#endif
                delivered_total += n;
            }
            src.clear();
        }
    }
}

void EventBus::end_tick() {
    for (EventBuffer& b : delivered) b.clear();
    for (u32& s : wave_start) s = 0;
}

u32 EventBus::new_count(u32 type_id) const {
    if (type_id == 0 || type_id >= delivered.len) return 0;
    u32 start = type_id < wave_start.len ? wave_start[type_id] : 0;
    return delivered[type_id].count() - start;
}

const void* EventBus::new_data(u32 type_id) const {
    if (type_id == 0 || type_id >= delivered.len) return nullptr;
    const EventBuffer& b = delivered[type_id];
    u32 start = type_id < wave_start.len ? wave_start[type_id] : 0;
    return b.bytes.data + (usize)start * b.elem_size;
}

u32 EventBus::pending_count() const {
    u32 n = 0;
    for (const EventWriter& w : writers) n += w.pending_count();
    return n;
}

u32 EventBus::pending_now() const {
    u32 n = 0;
    for (const EventWriter& w : writers)
        for (u32 t = 0; t < w.by_type.len; ++t) {
            const EventTypeDesc* desc = t < types.len ? types[t] : nullptr;
            if (desc && desc->next_tick()) continue;
            n += w.by_type[t].count();
        }
    return n;
}

} // namespace kv
