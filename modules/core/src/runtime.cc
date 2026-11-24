// Event types registered while the engine runs (KR19), and the listeners that read any
// type outside the schedule. A runtime type takes the next id, and its stream is made by
// the bus as anything is emitted into it (ensure_streams); since no unit is scheduled
// against it, it is read by listening, which engine_trace_delivery serves at each boundary.
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include <new>
#include <string.h>

namespace kv {

struct Engine::RuntimeEventType {
    EventTypeDesc desc{};
    u32           id = 0;
    u32           module = 0;
    bool          removed = false;
};

namespace {

Engine::RuntimeEventType* runtime_of(Engine& e, u32 type) {
    for (Engine::RuntimeEventType* r : e.runtime_events)
        if (r->id == type && !r->removed) return r;
    return nullptr;
}

} // namespace

u32 engine_event_type_id(const Engine& e, const char* name) {
    if (!name) return 0;
    for (u32 t = 1; t < e.event_types.len; ++t)
        if (e.event_types[t] && !strcmp(e.event_types[t]->name, name)) return t;
    return 0;
}

u32 engine_add_event_type(Engine& e, const char* module, const EventTypeDesc& d) {
    if (!d.name || !d.name[0] || !d.size || !d.align) { KV_ERROR("engine", "a runtime event type needs a name, a size and an alignment"); return 0; }
    if (engine_event_type_id(e, d.name)) { KV_ERROR("engine", "event type '%s' is registered already", d.name); return 0; }
    for (const Engine::RuntimeEventType* r : e.runtime_events)
        if (!strcmp(r->desc.name, d.name)) { KV_ERROR("engine", "event type '%s' was registered in this run and taken away: its name is not given out again", d.name); return 0; }
    const char* field = nullptr;
    if (const char* problem = schema_check(d.size, d.fields, &field)) {
        if (field) KV_ERROR("engine", "event type '%s': field '%s' %s", d.name, field, problem);
        else       KV_ERROR("engine", "event type '%s' %s", d.name, problem);
        return 0;
    }
    u32 owner = 0;
    if (module) {
        bool found = false;
        for (u32 i = 0; i < e.modules.len; ++i) if (!strcmp(e.modules[i].desc->name, module)) { owner = i; found = true; }
        if (!found) { KV_ERROR("engine", "event type '%s': no module '%s'", d.name, module); return 0; }
    }
    e.events.check_outside();
    Engine::RuntimeEventType* r = new (mem_alloc(sizeof(Engine::RuntimeEventType))) Engine::RuntimeEventType{};
    r->desc       = d;
    r->desc.name  = name_str(name_intern(d.name)); // the engine's copy, for the run
    r->desc.id    = &r->id;
    r->desc.flags = 0; // never next-tick, never held
    r->id         = e.event_types.len;
    r->module     = owner;
    e.runtime_events.push(r);
    e.event_types.push(&r->desc);
    e.event_type_count = e.event_types.len - 1;
    e.streams.push(0); // no resource: nothing in the schedule names it
    e.events.types = Span<const EventTypeDesc* const>(e.event_types.data, e.event_types.len);
    e.events.stale_warned.resize_fill(e.event_types.len, 0);
    KV_INFO("engine", "event type '%s' (%u bytes, %u fields) added as %u by '%s'", r->desc.name, d.size, d.fields.len, r->id,
            module ? module : "the program");
    return r->id;
}

bool engine_remove_event_type(Engine& e, u32 type) {
    Engine::RuntimeEventType* r = runtime_of(e, type);
    if (!r) return false;
    e.events.check_outside();
    // What is pending or merged of it, gone; its listeners with it.
    for (EventWriter& w : e.events.writers) if (type < w.by_type.len) w.by_type[type].clear();
    if (type < e.events.streams.len) {
        EventStream& st = e.events.streams[type];
        st.base = st.end();
        st.events.clear();
        st.traced = st.base;
    }
    u32 w = 0;
    for (u32 i = 0; i < e.listeners.len; ++i) if (e.listeners[i].type != type) e.listeners[w++] = e.listeners[i];
    e.listeners.len = w;
    e.event_types[type] = nullptr;
    e.events.types = Span<const EventTypeDesc* const>(e.event_types.data, e.event_types.len);
    r->removed = true;
    KV_INFO("engine", "event type '%s' (%u) taken away", r->desc.name, type);
    return true;
}

bool engine_emit(Engine& e, u32 type, const void* event) {
    const EventTypeDesc* d = type && type < e.event_types.len ? e.event_types[type] : nullptr;
    if (!d || !event) return false;
    e.events.check_outside();
    e.events.writers[0].emit_bytes(type, d->size, event);
    return true;
}

bool engine_listen(Engine& e, u32 type, EventListenFn fn, void* user) {
    if (!fn || !type || type >= e.event_types.len || !e.event_types[type]) return false;
    // From what is merged now on: what is delivered from here.
    u64 seen = type < e.events.streams.len ? e.events.streams[type].end() : 0;
    e.listeners.push(Engine::Listener{type, fn, user, seen});
    return true;
}

void engine_unlisten(Engine& e, u32 type, EventListenFn fn, void* user) {
    // Marked, and dropped after the delivery in progress (if any) is done with the list.
    for (Engine::Listener& l : e.listeners)
        if (l.type == type && l.fn == fn && l.user == user) l.fn = nullptr;
}

void engine_deliver_listened(Engine& e) {
    u32 n = e.listeners.len; // a listener added by a listener starts at the next boundary
    for (u32 i = 0; i < n; ++i) {
        Engine::Listener l = e.listeners[i];
        if (!l.fn || l.type >= e.events.streams.len) continue;
        const EventStream& st = e.events.streams[l.type];
        u64 from = l.seen < st.base ? st.base : l.seen;
        u32 elem = st.events.elem_size;
        for (u64 at = from; at < st.end() && elem; ++at) {
            if (!e.listeners[i].fn) break; // it stopped listening part way
            l.fn(l.user, l.type, st.events.bytes.data + (usize)(at - st.base) * elem);
        }
        e.listeners[i].seen = st.end();
    }
    u32 w = 0;
    for (u32 i = 0; i < e.listeners.len; ++i) if (e.listeners[i].fn) e.listeners[w++] = e.listeners[i];
    e.listeners.len = w;
}

void engine_free_runtime(Engine& e) {
    for (Engine::RuntimeEventType* r : e.runtime_events) { r->~RuntimeEventType(); mem_free(r); }
    e.runtime_events.free();
    e.listeners.free();
}

} // namespace kv
