#include "korvid/core/event.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/arena.h"
#include "korvid/core/phase.h"
#include "korvid/core/thread.h"
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
    main_thread = thread_id();
}

void EventBus::free() {
    for (EventWriter& w : writers) w.free();
    writers.free();
    for (EventStream& st : streams) st.events.free();
    streams.free();
    held.free();
    stale_warned.free();
}

void EventBus::check_outside() const {
#if KV_DEV
    if (ctx_running() && thread_id() != main_thread) {
        KV_ERROR("events", "'%s' emits into the engine's writer from a worker thread: emit through the context (ctx.emit, declared in `emits`)",
                 ctx_running()->name());
        KV_VERIFY(!"engine writer from a worker: see the log");
    }
#endif
}

void EventBus::check_read() const {
#if KV_DEV
    if (const ScheduledSystem* u = ctx_running()) {
        KV_ERROR("events", "'%s' reads the tick's events from the bus: declare the type in `reads` and take ctx.received<E>(), its batch", u->name());
        KV_VERIFY(!"bus read from inside a unit: see the log");
    }
#endif
}

namespace {

void ensure_streams(Array<EventStream>& arr, u32 type_id) {
    if (type_id >= arr.len) arr.resize_fill(type_id + 1, EventStream{});
}

// One writer's events of one type onto the end of a stream (or held, for a next-tick type).
void take(EventBus& bus, EventWriter& w, u32 t, bool to_held) {
    if (t >= w.by_type.len) return;
    EventBuffer& src = w.by_type[t];
    if (src.bytes.len == 0) return;
    EventBuffer* dst;
    if (to_held) dst = &bus.held.buffer(t, src.elem_size);
    else {
        ensure_streams(bus.streams, t);
        dst = &bus.streams[t].events;
        KV_ASSERT(dst->elem_size == 0 || dst->elem_size == src.elem_size);
        dst->elem_size = src.elem_size;
        bus.delivered_total += src.count();
    }
    memcpy(dst->bytes.push_n(src.bytes.len), src.bytes.data, src.bytes.len);
#if KV_DEV
    for (u32 i = 0; i < src.emitters.len; ++i) dst->emitters.push(src.emitters[i]);
#endif
    src.clear();
}

bool held_type(const EventBus& bus, u32 t) { return t < bus.types.len && bus.types[t] && bus.types[t]->next_tick(); }

u32 type_count(const EventBus& bus, const EventWriter& w) { return w.by_type.len > bus.streams.len ? w.by_type.len : bus.streams.len; }

bool handle_is_live(Handle& h, void*) { return h.is_null() || kv_handle_resolve(h.bits) != nullptr; }

} // namespace

void EventBus::begin_phase() {
    for (EventStream& st : streams) {
        st.phase_start = st.end();
        st.merged = false;
    }
}

void EventBus::merge(u32 type_id, u32 first, u32 last) {
    ensure_streams(streams, type_id);
    bool to_held = held_type(*this, type_id);
    for (u32 u = first; u <= last && u < writers.len; ++u) take(*this, writers[u], type_id, to_held);
    streams[type_id].merged = true;
}

void EventBus::merge_all(u32 first, u32 last) {
    for (u32 u = first; u <= last && u < writers.len; ++u)
        for (u32 t = 1; t < writers[u].by_type.len; ++t) take(*this, writers[u], t, held_type(*this, t));
}

void EventBus::merge_outside(bool tick_start) {
    if (tick_start)
        for (u32 t = 1; t < held.by_type.len; ++t) take(*this, held, t, false);
    EventWriter& w = writers[0];
    for (u32 t = 1; t < type_count(*this, w); ++t) take(*this, w, t, !tick_start && held_type(*this, t));
}

const void* EventBus::batch(u32 type_id, u64 from, u64 to, u32* count) {
    *count = 0;
    if (type_id >= streams.len) return nullptr;
    EventStream& st = streams[type_id];
    if (from < st.base) from = st.base; // a fresh sim dropped them
    if (to > st.end()) to = st.end();
    if (from >= to) return nullptr;
    u32 elem = st.events.elem_size;
    u8* first = st.events.bytes.data + (usize)(from - st.base) * elem;
    u32 n = (u32)(to - from);
    const EventTypeDesc* desc = type_id < types.len ? types[type_id] : nullptr;
    if (!desc || !schema_has_handles(desc->fields)) { *count = n; return first; }
    // One at a time: an event naming the dead is dropped from this batch, and counted.
    u8* out = nullptr;
    u32 kept = 0;
    for (u32 i = 0; i < n; ++i) {
        u8* e = first + (usize)i * elem;
        bool live = schema_each_handle(desc->fields, e, handle_is_live, nullptr);
        if (!live) {
            ++dropped_stale;
#if KV_DEV
            stale_warned.resize_fill(type_id + 1, 0);
            if (!stale_warned[type_id] && !(desc->flags & EvStaleOk)) {
                stale_warned[type_id] = 1;
                KV_WARN("events", "a %s was dropped from a batch: a handle in it was stale (said once for this type; EventBus::dropped_stale counts)", desc->name);
            }
#endif
            if (!out) { // the first dead one: copy what came before it, and go on copying
                out = (u8*)alloc_in(mem_scratch(), (usize)n * elem);
                memcpy(out, first, (usize)i * elem);
                kept = i;
            }
            continue;
        }
        if (out) memcpy(out + (usize)kept++ * elem, e, elem);
    }
    if (!out) { *count = n; return first; }
    *count = kept;
    return out;
}

void EventBus::end_tick(Span<const u64> keep_from) {
    for (u32 t = 1; t < streams.len; ++t) {
        EventStream& st = streams[t];
        u64 keep = t < keep_from.len ? keep_from[t] : st.end();
        if (keep > st.end()) keep = st.end();
        if (keep > st.base) {
            u32 drop = (u32)(keep - st.base);
            u32 elem = st.events.elem_size;
            u32 left = st.events.count() - drop;
            if (left) memmove(st.events.bytes.data, st.events.bytes.data + (usize)drop * elem, (usize)left * elem);
            st.events.bytes.len = left * elem;
#if KV_DEV
            if (st.events.emitters.len >= drop) {
                for (u32 i = 0; i + drop < st.events.emitters.len; ++i) st.events.emitters[i] = st.events.emitters[i + drop];
                st.events.emitters.len -= drop;
            }
#endif
            st.base = keep;
        }
        st.tick_start = st.end();
        if (st.traced < st.base) st.traced = st.base;
    }
}

void EventBus::drop_all() {
    for (EventWriter& w : writers) w.clear();
    held.clear();
    for (EventStream& st : streams) {
        st.base = st.end();
        st.events.clear();
        st.tick_start = st.phase_start = st.traced = st.base;
    }
}

u32 EventBus::pending_count() const {
    u32 n = held.pending_count();
    for (const EventWriter& w : writers) n += w.pending_count();
    return n;
}

} // namespace kv
