// The engine's events in Lua, by their field tables (KR18), and a resource read the same
// way. The events object:
//
//   ev:emit("PlayerPlay", { track = 3 })   -- the fields left out at the struct's defaults
//   ev:emit("Tick")                        -- a type with no field table: its defaults
//   for _, p in ipairs(ev:received("PlayerPosition")) do ... end
//   ev:fields("PlayerPlay")                -- the field names, in the table's order
//   ev:has("PlayerPlay")                   -- whether the engine knows the type
//
// Inside a unit (script_events_set_context) emit is the unit's, checked against its
// `emits`, and received is its batch of a type in its `reads`: either undeclared is a Lua
// error naming the type, not the assertion C++ gets. Outside the schedule emit goes to the
// engine's writer, landing at the next boundary, and received is what the running tick
// merged so far (EventBus::read). The mapping and its errors are fields.h's, a row's.
#include "korvid/script/script.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "fields.h"
#include <string.h>

namespace kv {

namespace {

constexpr const char* kEventsMeta = "kv.events";

struct ScriptEvents { Engine* engine; TickContext* ctx; };

ScriptEvents* check_events(lua_State* L, int i) { return (ScriptEvents*)luaL_checkudata(L, i, kEventsMeta); }

// The type named by the string at `i`: its id and table. Raises for a name nobody declared.
const EventTypeDesc* check_type(lua_State* L, ScriptEvents* ev, int i, u32* id) {
    const char* name = luaL_checkstring(L, i);
    const Engine& e = *ev->engine;
    for (u32 t = 1; t < e.event_types.len; ++t)
        if (e.event_types[t] && strcmp(e.event_types[t]->name, name) == 0) { *id = t; return e.event_types[t]; }
    luaL_error(L, "no event type '%s'", name);
    return nullptr;
}

int events_emit(lua_State* L) {
    ScriptEvents* ev = check_events(L, 1);
    u32 id = 0;
    const EventTypeDesc* d = check_type(L, ev, 2, &id);
    bool given = !lua_isnoneornil(L, 3);
    if (given && !d->fields.len) {
        lua_pushnil(L);
        bool any = lua_next(L, 3) != 0;
        if (any) { lua_pop(L, 2); luaL_error(L, "%s has no field table: it is emitted with its defaults only", d->name); }
    }
    Engine& e = *ev->engine;
    TickContext* ctx = ev->ctx;
    // A type registered while the engine runs (engine_add_event_type) has no stream
    // resource: nothing in the schedule names it, and it goes by the engine's writer.
    bool runtime = id < e.streams.len && e.streams[id] == 0;
    if (runtime) ctx = nullptr;
    if (ctx && ctx->system) {
        Access m;
        u32 r = id < e.streams.len ? e.streams[id] : 0;
        if (!r || !ctx->system->mode(r, &m) || m != Access::Append)
            luaL_error(L, "'%s' emits %s without declaring it in `emits`", ctx->system->name(), d->name);
    }
    // The event built in place: its defaults, then the table's fields over them. A full
    // userdata, so that an error part way leaves nothing to free.
    u8* bytes = (u8*)lua_newuserdatauv(L, d->size ? d->size : 1, 0);
    if (d->defaults) memcpy(bytes, d->defaults, d->size);
    else memset(bytes, 0, d->size);
    if (given && d->fields.len) script_set_fields(L, d->fields, bytes, 3, d->name);
    EventWriter& w = ctx && ctx->writer ? *ctx->writer : e.events.writers[0];
    if (!(ctx && ctx->writer)) e.events.check_outside();
    w.emit_bytes(id, d->size, bytes);
    lua_pop(L, 1);
    return 0;
}

int events_received(lua_State* L) {
    ScriptEvents* ev = check_events(L, 1);
    u32 id = 0;
    const EventTypeDesc* d = check_type(L, ev, 2, &id);
    Engine& e = *ev->engine;
    TickContext* ctx = ev->ctx;
    const u8* data = nullptr;
    u32 count = 0;
    if (id < e.streams.len && e.streams[id] == 0 && ctx && ctx->system)
        luaL_error(L, "%s was registered while the engine runs: it is read by listening (engine_listen), not received", d->name);
    if (ctx && ctx->system) {
        bool reads = false;
        for (const ScheduledSystem::Cursor& c : ctx->system->cursors) if (c.type == id) reads = true;
        if (!reads) luaL_error(L, "'%s' reads %s without declaring it in `reads`", ctx->system->name(), d->name);
        data = (const u8*)ctx_received(*ctx, id, &count);
    } else if (id < e.events.streams.len) {
        e.events.check_read();
        const EventStream& s = e.events.streams[id];
        u64 skip = s.tick_start > s.base ? s.tick_start - s.base : 0;
        count = (u32)(s.events.count() - skip);
        data  = s.events.bytes.data + (usize)skip * s.events.elem_size;
    }
    lua_createtable(L, (int)count, 0);
    usize stride = d->size;
    for (u32 i = 0; i < count; ++i) {
        script_push_fields(L, d->fields, data + (usize)i * stride);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int events_fields(lua_State* L) {
    ScriptEvents* ev = check_events(L, 1);
    u32 id = 0;
    const EventTypeDesc* d = check_type(L, ev, 2, &id);
    lua_createtable(L, (int)d->fields.len, 0);
    for (u32 i = 0; i < d->fields.len; ++i) {
        lua_pushstring(L, d->fields[i].name);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

int events_has(lua_State* L) {
    ScriptEvents* ev = check_events(L, 1);
    const char* name = luaL_checkstring(L, 2);
    bool found = false;
    for (u32 t = 1; t < ev->engine->event_types.len && !found; ++t)
        found = ev->engine->event_types[t] && strcmp(ev->engine->event_types[t]->name, name) == 0;
    lua_pushboolean(L, found);
    return 1;
}

constexpr luaL_Reg k_events_methods[] = {
    {"emit", events_emit}, {"received", events_received}, {"fields", events_fields}, {"has", events_has}, {nullptr, nullptr},
};

} // namespace

void script_push_events(lua_State* L, Engine& e, TickContext* ctx) {
    ScriptEvents* ev = (ScriptEvents*)lua_newuserdatauv(L, sizeof(ScriptEvents), 0);
    ev->engine = &e;
    ev->ctx    = ctx;
    if (luaL_newmetatable(L, kEventsMeta)) {
        lua_newtable(L);
        luaL_setfuncs(L, k_events_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
}

void script_events_set_context(lua_State* L, int index, TickContext* ctx) {
    check_events(L, index)->ctx = ctx;
}

bool script_push_resource(lua_State* L, Engine& e, const char* name) {
    u32 id = engine_resource_id(e, name);
    const ResourceEntry* r = id ? &e.resources[id - 1] : nullptr;
    if (!r || !r->fields.len || !r->ptr) { lua_pushnil(L); return false; }
    script_push_fields(L, r->fields, (const u8*)r->ptr);
    return true;
}

} // namespace kv
