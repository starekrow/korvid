// The registry. Reads the module list once, in order, and turns the declarations into
// ids, a schedule and started instances. Every check here names what clashed.
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/arena.h"
#include "korvid/core/atomic.h"
#include "korvid/core/hash.h"
#include "korvid/core/job.h"
#include "korvid/core/profile.h"
#include "korvid/core/time.h"
#include "korvid/core/thread.h"
#include "korvid/core/log.h"
#include "korvid/core/name.h"
#include "korvid/core/memory.h"
#include <string.h>

namespace kv {

namespace {

bool names_equal(const char* a, const char* b) { return strcmp(a, b) == 0; }


// The engine's own event types, registered after every module's so module ids are
// unaffected by what the core adds.
constinit const FieldDesc k_spawned_fields[]    = {KV_FIELD(Spawned, ticket), KV_FIELD(Spawned, kind), KV_FIELD(Spawned, entity)};
constinit const FieldDesc k_state_edge_fields[] = {KV_FIELD(StateEdge, entity), KV_FIELD(StateEdge, trigger), KV_FIELD(StateEdge, value)};
constinit const FieldDesc k_sim_reset_fields[]  = {KV_FIELD(SimReset, resets), KV_FIELD(SimReset, pad)};
constinit const EventTypeDesc k_core_events[] = {
    KV_EVENT_EX(Spawned, k_spawned_fields, 0),
    KV_EVENT_EX(StateEdge, k_state_edge_fields, 0),
    KV_EVENT_EX(SimReset, k_sim_reset_fields, 0),
};

bool check_module_names(Span<const ModuleDesc* const> modules) {
    if (modules.len > KV_MEM_MAX_MODULES) {
        KV_ERROR("engine", "%u modules; memory accounting supports %u", modules.len, KV_MEM_MAX_MODULES);
        return false;
    }
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc* m = modules[i];
        if (!m || !m->name || !m->name[0]) {
            KV_ERROR("engine", "module list entry %u is null or unnamed", i);
            return false;
        }
        for (u32 j = 0; j < i; ++j) {
            if (names_equal(modules[j]->name, m->name)) {
                KV_ERROR("engine", "module '%s' appears twice in the module list (entries %u and %u)",
                         m->name, j, i);
                return false;
            }
        }
    }
    return true;
}

// Event ids: module order, then declaration order, starting at 1.
bool register_events(Engine& e, Span<const ModuleDesc* const> modules) {
    u32 next = 1;
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const EventTypeDesc& t : m.events) {
            for (u32 j = 0; j < i; ++j) {
                for (const EventTypeDesc& o : modules[j]->events) {
                    if (names_equal(o.name, t.name)) {
                        KV_ERROR("engine", "event type '%s' declared by both '%s' and '%s'",
                                 t.name, modules[j]->name, m.name);
                        return false;
                    }
                }
            }
        }
        u32 n = event_types_register(m.events, next);
        if (n == 0 && m.events.len) {
            KV_ERROR("engine", "module '%s': event registration failed", m.name);
            return false;
        }
        if (n) next = n;
    }
    for (const EventTypeDesc& t : k_core_events)
        for (const ModuleDesc* m : modules)
            for (const EventTypeDesc& o : m->events)
                if (names_equal(o.name, t.name)) {
                    KV_ERROR("engine", "event type '%s' in module '%s' is the engine's", t.name, m->name);
                    return false;
                }
    u32 n = event_types_register(k_core_events, next);
    if (n == 0) { KV_ERROR("engine", "core event registration failed"); return false; }
    e.event_type_count = n - 1;
    // By id, for the bus (stale drop, next-tick) and the trace.
    e.event_types.resize_fill(e.event_type_count + 1, nullptr);
    for (const ModuleDesc* m : modules)
        for (const EventTypeDesc& t : m->events) e.event_types[*t.id] = &t;
    for (const EventTypeDesc& t : k_core_events) e.event_types[*t.id] = &t;
    return true;
}

// Component ids: module order, then declaration order, starting at 1. Every schema is
// checked for consistency here, so nothing downstream has to.
bool register_components(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const ComponentDesc& c : m.components) {
            const char* field = nullptr;
            if (const char* problem = component_check(c, &field)) {
                if (field) KV_ERROR("engine", "component '%s' in module '%s': field '%s' %s", c.name, m.name, field, problem);
                else       KV_ERROR("engine", "component '%s' in module '%s' %s", c.name ? c.name : "?", m.name, problem);
                return false;
            }
            if (u32 id = engine_component_id(e, c.name)) {
                KV_ERROR("engine", "component '%s' declared by both '%s' and '%s'", c.name,
                         modules[e.components[id - 1].module]->name, m.name);
                return false;
            }
            if (*c.id != 0) {
                KV_ERROR("engine", "component '%s' in module '%s' is the type already registered as '%s' by '%s'",
                         c.name, m.name, e.components[*c.id - 1].desc->name, modules[e.components[*c.id - 1].module]->name);
                return false;
            }
            e.components.push(ComponentEntry{&c, i});
            *c.id = e.components.len;
        }
    }
    return true;
}

void unregister_components(Engine& e) {
    for (const ComponentEntry& c : e.components) *c.desc->id = 0;
    e.components.free();
}

// Kinds: one table each, in module then declaration order, created here so their
// resources exist before any system references them. Table addresses never move after
// this: the handle registry holds them as contexts.
bool register_kinds(Engine& e, Span<const ModuleDesc* const> modules, const EngineConfig& config) {
    u32 count = 0;
    for (const ModuleDesc* m : modules) count += m->kinds.len;
    e.tables.reserve(count);
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const KindDesc& k : m.kinds) {
            const char* name = k.name ? engine_qualify(m.name, k.name) : nullptr;
            if (!name || !name[0] || name[0] == '.' || strlen(name) >= KV_KIND_NAME_MAX) {
                KV_ERROR("engine", "module '%s' has a kind with no name or a name over %u characters", m.name, KV_KIND_NAME_MAX - 1);
                return false;
            }
            if (k.capacity == 0) { KV_ERROR("engine", "kind '%s' in module '%s' has zero capacity", name, m.name); return false; }
            if (u32 id = engine_kind_id(e, name)) {
                KV_ERROR("engine", "kind '%s' declared by both '%s' and '%s'", name, modules[e.tables[id - 1].module]->name, m.name);
                return false;
            }
            if (engine_resource_id(e, name)) { KV_ERROR("engine", "kind '%s' in module '%s' collides with a resource of that name", name, m.name); return false; }
            const ComponentDesc* comps[32];
            u32 ncomps = 0;
            char cname[KV_KIND_NAME_MAX];
            for (const char* cursor = k.components; name_list_next(cursor, cname, sizeof cname);) {
                if (ncomps >= KV_COUNTOF(comps)) { KV_ERROR("engine", "kind '%s' has more than %u components", name, KV_COUNTOF(comps)); return false; }
                u32 cid = engine_component_id(e, cname);
                if (!cid) { KV_ERROR("engine", "kind '%s' in module '%s' uses unknown component '%s'", name, m.name, cname); return false; }
                for (u32 d = 0; d < ncomps; ++d)
                    if (comps[d] == e.components[cid - 1].desc) { KV_ERROR("engine", "kind '%s' lists component '%s' twice", name, cname); return false; }
                comps[ncomps++] = e.components[cid - 1].desc;
            }
            // What an entity may carry on its own: never a column of the kind too.
            const ComponentDesc* attach[16];
            u32 nattach = 0;
            for (const char* cursor = k.attachable; name_list_next(cursor, cname, sizeof cname);) {
                if (nattach >= KV_COUNTOF(attach)) { KV_ERROR("engine", "kind '%s' has more than %u attachable components", name, KV_COUNTOF(attach)); return false; }
                u32 cid = engine_component_id(e, cname);
                if (!cid) { KV_ERROR("engine", "kind '%s' in module '%s' may attach unknown component '%s'", name, m.name, cname); return false; }
                const ComponentDesc* cd = e.components[cid - 1].desc;
                for (u32 d = 0; d < ncomps; ++d)
                    if (comps[d] == cd) { KV_ERROR("engine", "kind '%s' has '%s' as a column and as an attachment", name, cname); return false; }
                for (u32 d = 0; d < nattach; ++d)
                    if (attach[d] == cd) { KV_ERROR("engine", "kind '%s' lists attachable '%s' twice", name, cname); return false; }
                attach[nattach++] = cd;
            }
            mem_set_owner((u16)(i + 1));
            MemTag tag;
            tag.module   = (u16)(i + 1);
            tag.heap     = mem_module_heap((u16)(i + 1));
            tag.category = mem_category_register("kinds");
            u32 capacity = 0;
            for (const KindCapacity& kc : config.capacities)
                if (kc.kind && !strcmp(kc.kind, name)) capacity = kc.capacity;
            if (capacity) KV_INFO("engine", "kind '%s': capacity %u (the KindDesc says %u)", name, capacity, k.capacity);
            Table& t = e.tables.push(Table{});
            bool ok = t.init(k, name, Span<const ComponentDesc* const>(comps, ncomps), e.tables.len - 1, i, tag,
                             Span<const ComponentDesc* const>(attach, nattach), capacity);
            mem_set_owner(0);
            if (!ok) { KV_ERROR("engine", "kind '%s' in module '%s': table creation failed", name, m.name); return false; }
            // The kind, then what it covers, together: its liveness and columns first, which
            // is all an early read of it covers, then attachments and bags.
            u32 kind_id = e.tables.len;
            auto add = [&](const char* rname, ResourceClass cls, bool cosmetic) {
                ResourceEntry r{rname, i};
                r.cls = cls;
                r.table = kind_id;
                r.cosmetic = cosmetic;
                e.resources.push(r);
                return e.resources.len;
            };
            t.resource = add(name, ResourceClass::Kind, false);
            {
                char alive[KV_KIND_NAME_MAX + 8];
                format(alive, sizeof alive, "%s.@alive", name);
                t.alive_resource = add(name_str(name_intern(alive)), ResourceClass::Liveness, false);
            }
            for (Column& c : t.columns) c.resource = add(c.resource_name, ResourceClass::Column, !c.desc->is_sim());
            for (Attachment& a : t.attachments) a.resource = add(a.resource_name, ResourceClass::Attachment, !a.desc->is_sim());
            if (k.props) t.props_resource = add(t.props_resource_name, ResourceClass::Bags, false);
            e.resources[t.resource - 1].children = e.resources.len - t.resource;
            e.resources[t.resource - 1].early    = 1 + t.columns.len;
            if (k.id) *k.id = e.tables.len;
        }
    }
    return true;
}

void free_tables(Engine& e) {
    for (Table& t : e.tables) {
        if (t.desc && t.desc->id) *t.desc->id = 0;
        mem_set_owner((u16)(t.module + 1));
        t.free();
        mem_set_owner(0);
    }
    e.tables.free();
}

// Every event type is a stream, named by the type, after every other resource.
void register_streams(Engine& e, Span<const ModuleDesc* const> modules) {
    e.streams.resize_fill(e.event_type_count + 1, 0);
    for (u32 t = 1; t <= e.event_type_count; ++t) {
        const EventTypeDesc* d = e.event_types[t];
        u32 module = 0;
        for (u32 i = 0; i < modules.len; ++i)
            for (const EventTypeDesc& o : modules[i]->events) if (&o == d) module = i;
        ResourceEntry r{d->name, module};
        r.cls = ResourceClass::Stream;
        r.event = t;
        e.resources.push(r);
        e.streams[t] = e.resources.len;
    }
}

bool register_resources(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        for (const ResourceDesc& r : modules[i]->resources) {
            const char* name = engine_qualify(modules[i]->name, r.name);
            if (u32 id = engine_resource_id(e, name)) {
                KV_ERROR("engine", "resource '%s' declared by both '%s' and '%s'", name,
                         modules[e.resources[id - 1].module]->name, modules[i]->name);
                return false;
            }
            ResourceEntry entry{name, i};
            entry.cls   = r.apply ? ResourceClass::Append : ResourceClass::Published;
            entry.apply = r.apply;
            e.resources.push(entry);
        }
    }
    return true;
}

// ---- declarations to access sets ----------------------------------------------------
// A unit's sets become one list of (resource, mode), expanded over the resource tree. That
// list is all the schedule and the access checks look at. See docs/design/scheduler.md.

const char* resource_name(const Engine& e, u32 id) { return id && id <= e.resources.len ? e.resources[id - 1].name : "?"; }

const char* class_name(ResourceClass c) {
    switch (c) {
    case ResourceClass::State:      return "a module's state";
    case ResourceClass::Published:  return "a published resource";
    case ResourceClass::Append:     return "an append resource";
    case ResourceClass::Stream:     return "an event type";
    case ResourceClass::Kind:       return "a kind";
    case ResourceClass::Liveness:   return "a kind's liveness";
    case ResourceClass::Column:     return "a column";
    case ResourceClass::Attachment: return "an attachment store";
    case ResourceClass::Bags:       return "property bags";
    }
    return "?";
}

// Which kinds a "*[...]" filter admits: those with every '+' component and no '-' one.
struct Filter {
    u32 has[4], lacks[4];
    u32 nhas = 0, nlacks = 0;
    bool admits(const Table& t) const {
        for (u32 k = 0; k < nhas; ++k) if (!t.has(has[k])) return false;
        for (u32 k = 0; k < nlacks; ++k) if (t.has(lacks[k])) return false;
        return true;
    }
};

// "A+B-C" into a filter; on failure, `bad` is the name it stopped at.
bool parse_filter(const Engine& e, char* text, Filter& f, const char** bad) {
    char sign = '+';
    for (char* at = text; at;) {
        char* next = strpbrk(at, "+-");
        char next_sign = next ? *next : 0;
        if (next) *next = 0;
        u32 id = engine_component_id(e, at);
        u32& n = sign == '+' ? f.nhas : f.nlacks;
        if (!id || n == 4) { *bad = at; return false; }
        (sign == '+' ? f.has : f.lacks)[n++] = id;
        sign = next_sign;
        at = next ? next + 1 : nullptr;
    }
    if (!f.nhas) { *bad = text; return false; }
    return true;
}

// One comma-separated list of names into `out` as declared entries in `mode`; '^' makes a
// read early where `early_ok`. "*.C" is that column in every kind and must exist somewhere;
// "*[F].C" is that column in every kind the filter F admits, and may match none (a module's
// system that moves only what opted in); "*.@C" and "*.@props" are every kind's
// attachments of C, or bags; ".x" is "<module>.x".
bool resolve_set(const Engine& e, const ModuleDesc& m, const char* unit, const char* list, const char* field,
                 Access mode, bool early_ok, Array<UnitAccess>& out) {
    char buf[2 * KV_KIND_NAME_MAX + 2];
    for (const char* cursor = list; name_list_next(cursor, buf, sizeof buf);) {
        char* raw = buf;
        Access am = mode;
        if (raw[0] == '^') {
            if (!early_ok) { KV_ERROR("engine", "'%s' in module '%s' %s '%s': '^' marks a system's reads only", unit, m.name, field, raw); return false; }
            am = Access::Early;
            ++raw;
        }
        u32 first = out.len;
        if (raw[0] == '*' && raw[1] == '[') {
            char* close = strchr(raw, ']');
            if (!close || close[1] != '.') {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s': write *[Component].Column", unit, m.name, field, raw);
                return false;
            }
            *close = 0;
            Filter f;
            const char* bad = nullptr;
            if (!parse_filter(e, raw + 2, f, &bad)) {
                KV_ERROR("engine", "'%s' in module '%s' %s '*[...].%s': '%s' is not a known component (four of each sign at most, one '+' at least)",
                         unit, m.name, field, close + 2, bad);
                return false;
            }
            u32 cid = engine_component_id(e, close + 2);
            if (!cid) { KV_ERROR("engine", "'%s' in module '%s' %s '*[...].%s': unknown component '%s'", unit, m.name, field, close + 2, close + 2); return false; }
            for (const Table& t : e.tables)
                if (f.admits(t))
                    if (const Column* c = t.column(cid)) out.push(UnitAccess{c->resource, am});
            continue;
        }
        if (raw[0] == '*' && raw[1] == '.' && raw[2] == '@') {
            bool bags = strcmp(raw + 3, "props") == 0;
            u32 cid = bags ? 0 : engine_component_id(e, raw + 3);
            for (const Table& t : e.tables) {
                if (bags && t.props_resource) out.push(UnitAccess{t.props_resource, am});
                if (const Attachment* a = cid ? t.attachment(cid) : nullptr) out.push(UnitAccess{a->resource, am});
            }
            if (out.len == first) {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s' but no kind has %s", unit, m.name, field, raw, bags ? "bags" : "it attachable");
                return false;
            }
            continue;
        }
        if (raw[0] == '*' && raw[1] == '.') {
            u32 cid = engine_component_id(e, raw + 2);
            for (const Table& t : e.tables)
                if (const Column* c = t.column(cid)) out.push(UnitAccess{c->resource, am});
            if (out.len == first) {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s' but no kind has component '%s'", unit, m.name, field, raw, raw + 2);
                return false;
            }
            continue;
        }
        const char* name = engine_qualify(m.name, raw);
        u32 id = engine_resource_id(e, name);
        if (!id) { KV_ERROR("engine", "'%s' in module '%s' %s unknown resource '%s'", unit, m.name, field, name); return false; }
        out.push(UnitAccess{id, am});
    }
    return true;
}

// What a declared entry may be: a module's state only through `state`; an early read only of
// a kind, its liveness or a column, the things a phase start can be copied of; a write of a
// kind's liveness only as the whole kind, since a spawn writes every column; an append only
// to an append resource, which no one but its owner writes.
bool check_declared(const Engine& e, const char* unit, u32 module, UnitAccess d) {
    const ResourceEntry& r = e.resources[d.resource - 1];
    if (r.cls == ResourceClass::State) {
        KV_ERROR("engine", "'%s' names '%s': a module's state is declared with `state`, and only its own", unit, r.name);
        return false;
    }
    if (d.mode == Access::Early && r.cls != ResourceClass::Kind && r.cls != ResourceClass::Liveness && r.cls != ResourceClass::Column &&
        r.cls != ResourceClass::Stream) {
        KV_ERROR("engine", "'%s' reads '%s' early (^), but it is %s: only a kind, its liveness, its columns or an event type are read early", unit, r.name, class_name(r.cls));
        return false;
    }
    if (r.cls == ResourceClass::Stream && d.mode == Access::Write) {
        KV_ERROR("engine", "'%s' writes '%s', an event type: declare it in `emits`", unit, r.name);
        return false;
    }
    if (d.mode == Access::Write && r.cls == ResourceClass::Liveness) {
        KV_ERROR("engine", "'%s' writes '%s': spawning and destroying write every column, so write the kind ('%s')", unit, r.name, e.tables[r.table - 1].name);
        return false;
    }
    if (d.mode == Access::Append && r.cls != ResourceClass::Append && r.cls != ResourceClass::Stream) {
        KV_ERROR("engine", "'%s' appends to '%s', which is %s, not an append resource (ResourceDesc::apply) or an event type", unit, r.name, class_name(r.cls));
        return false;
    }
    if (d.mode == Access::Write && r.cls == ResourceClass::Append && r.module != module) {
        KV_ERROR("engine", "'%s' writes '%s', an append resource of '%s': declare it under `appends`", unit, r.name, e.modules[r.module].desc->name);
        return false;
    }
    return true;
}

// The Frame and Work cycles run as often as the machine allows and are never in a replay:
// a unit in one may write cosmetic kinds, cosmetic components and its own state, never
// the simulation's kinds (docs/design/cycles.md, Determinism). Checked on the expanded
// access, so a kind named whole is caught at its first sim column.
bool check_cycle_writes(const Engine& e, const ScheduledSystem& sys, Phase phase) {
    if ((u32)phase < KV_SIM_PHASES) return true;
    for (const UnitAccess& a : sys.access) {
        if (a.mode != Access::Write) continue;
        const ResourceEntry& r = e.resources[a.resource - 1];
        if (!r.table || r.cosmetic) continue;
        const Table& t = e.tables[r.table - 1];
        if (t.desc && t.desc->cosmetic) continue;
        KV_ERROR("engine", "'%s' runs in the %s cycle and writes '%s', which is the simulation's: a Frame or Work unit writes only "
                 "cosmetic kinds and its own state; send the sim an event instead (docs/design/cycles.md)", sys.name(), phase_name(phase), r.name);
        return false;
    }
    return true;
}

// The one mode a unit has on a resource it covers twice: a write includes the read; any
// other pair is a contradiction.
bool merge_modes(Access a, Access b, Access* out) {
    if (a == b) { *out = a; return true; }
    if ((a == Access::Read && b == Access::Write) || (a == Access::Write && b == Access::Read)) { *out = Access::Write; return true; }
    return false;
}

// `sys.declared` expanded into `sys.access`, sorted by resource: a kind covers its liveness,
// columns, attachments and bags, or for an early read its liveness and columns; the module's
// state comes from `state` (a handler always has it). One mode per resource, however it is
// reached: a unit that wants some of a kind as it stands and some early names the parts.
bool build_access(const Engine& e, ScheduledSystem& sys, StateUse state) {
    const char* unit = sys.name();
    Array<u32> from = array_in<u32>(mem_scratch()); // which declared entry put each access there
    auto add = [&](u32 id, Access mode, u32 d) {
        for (u32 k = 0; k < sys.access.len; ++k) {
            if (sys.access[k].resource != id) continue;
            Access merged;
            if (merge_modes(sys.access[k].mode, mode, &merged)) { sys.access[k].mode = merged; return true; }
            // An event type consumed up to the phase start and emitted: a loop that goes round
            // once a phase. It orders as an emitter; its batch is the cursor's business.
            Access was_mode = sys.access[k].mode;
            if (e.resources[id - 1].cls == ResourceClass::Stream &&
                ((was_mode == Access::Early && mode == Access::Append) || (was_mode == Access::Append && mode == Access::Early))) {
                sys.access[k].mode = Access::Append;
                return true;
            }
            const UnitAccess& was = sys.declared[from[k]];
            const UnitAccess& now = sys.declared[d];
            bool own_write = (mode == Access::Early && sys.access[k].mode == Access::Write) || (mode == Access::Write && sys.access[k].mode == Access::Early);
            KV_ERROR("engine", "'%s' covers '%s' as %s (by %s'%s') and as %s (by %s'%s'): one mode per resource%s", unit, resource_name(e, id),
                     access_name(sys.access[k].mode), was.mode == Access::Early ? "^" : "", resource_name(e, was.resource),
                     access_name(mode), now.mode == Access::Early ? "^" : "", resource_name(e, now.resource),
                     own_write ? " (an early read of what the unit writes itself would see its own writes one way and not the other)"
                               : " (name the parts of a kind one by one, '.k.@alive, .k.Column', or leave a component out with a filter, '*[A-B].C')");
            return false;
        }
        sys.access.push(UnitAccess{id, mode});
        from.push(d);
        return true;
    };
    bool ok = true;
    for (u32 d = 0; ok && d < sys.declared.len; ++d) {
        UnitAccess a = sys.declared[d];
        if (!check_declared(e, unit, sys.module->index, a)) { ok = false; break; }
        const ResourceEntry& r = e.resources[a.resource - 1];
        u32 covers = r.cls == ResourceClass::Kind ? (a.mode == Access::Early ? r.early : r.children) : 0;
        for (u32 k = 0; ok && k <= covers; ++k) ok = add(a.resource + k, a.mode, d);
    }
    // A handler consumes its own event as it stands unless it names it.
    if (ok && sys.handler) {
        u32 own = e.streams[*sys.handler->event];
        bool named = false;
        for (const UnitAccess& d : sys.declared) named = named || (d.resource == own && d.mode != Access::Append);
        if (!named) {
            sys.declared.push(UnitAccess{own, Access::Read});
            ok = add(own, Access::Read, sys.declared.len - 1);
            if (!ok) KV_ERROR("engine", "'%s' emits the event it handles: read it early (reads = \"^%s\"), so what it emits is next phase's", unit, resource_name(e, own));
        }
    }
    // The streams it consumes, each with a cursor.
    for (u32 d = 0; ok && d < sys.declared.len; ++d) {
        const UnitAccess& a = sys.declared[d];
        const ResourceEntry& r = e.resources[a.resource - 1];
        if (r.cls != ResourceClass::Stream || (a.mode != Access::Read && a.mode != Access::Early)) continue;
        sys.cursors.push(ScheduledSystem::Cursor{r.event, a.mode == Access::Early, 0, 0, 0});
    }
    if (ok && state != StateUse::None)
        sys.access.push(UnitAccess{sys.module->state_resource, state == StateUse::Write ? Access::Write : Access::Read});
    from.free();
    if (!ok) return false;
    // By resource: lookups bisect, and two units' lists merge in one walk.
    for (u32 i = 1; i < sys.access.len; ++i) {
        UnitAccess v = sys.access[i];
        u32 j = i;
        while (j > 0 && sys.access[j - 1].resource > v.resource) { sys.access[j] = sys.access[j - 1]; --j; }
        sys.access[j] = v;
    }
    // Sim state flows to presentation, never back: a unit that reads a cosmetic column or
    // attachment may not write a sim one.
    for (const UnitAccess& rd : sys.access) {
        const ResourceEntry& r = e.resources[rd.resource - 1];
        if (!r.cosmetic || rd.mode == Access::Write) continue;
        for (const UnitAccess& wr : sys.access) {
            const ResourceEntry& w = e.resources[wr.resource - 1];
            if (wr.mode != Access::Write || w.cosmetic || (w.cls != ResourceClass::Column && w.cls != ResourceClass::Attachment)) continue;
            KV_ERROR("engine", "'%s' reads cosmetic '%s' and writes sim '%s': data flows from sim to presentation, never back", unit, r.name, w.name);
            return false;
        }
    }
    return true;
}

// ---- the schedule -----------------------------------------------------------------
// One phase's units (systems and handlers alike) become a graph of constraints,
// each with its reason, and the graph is then checked as a whole:
//   base:  a reader after every writer of what it reads; `after` and `before`;
//   early: an early reader before every writer of what it reads early, for the kinds whose
//          early reads are ordered (a snapshot kind's need no order);
//   state: two units of a module that share its state, one writing, that nothing above
//          orders, the way one topological order of base and the kinds' own early edges
//          has them: declaration order, unless the rest says otherwise.
// Base and state do not depend on what the engine forces for early reads, so writers of
// one resource keep their order whatever it does, and every unit sees the same values.
// Checked: no early reader of an ordered kind comes after its writer; no cycles; every
// pair of writers of one resource has a path between them in base and state.

enum class Why : u8 { Data, Declared, State, Early };

struct Edge {
    u32 from, to;
    u32 resource; // 0 for a declared one
    Why why;
};

struct Pair { u32 a, b, resource; };

struct Pending {
    ScheduledSystem sys;
    u32             rank; // module order then declaration order
};

const char* after_of(const Pending& p)  { return p.sys.desc ? p.sys.desc->after : p.sys.handler->after; }
const char* before_of(const Pending& p) { return p.sys.desc ? p.sys.desc->before : p.sys.handler->before; }

u32 find_pending(const Array<Pending>& pend, const char* name) {
    for (u32 i = 0; i < pend.len; ++i)
        if (names_equal(pend[i].sys.name(), name)) return i + 1;
    return 0;
}

// Who reaches whom, as bit rows. Built once per graph; every path question is a lookup.
struct Reach {
    u32        n = 0, words = 0;
    Array<u64> bits;
    void build(const Array<Edge>& edges, u32 count) {
        n = count;
        words = (n + 63) / 64;
        bits = array_in<u64>(mem_scratch());
        bits.resize_fill(n * words, 0);
        Array<u32> stack = array_in<u32>(mem_scratch());
        for (u32 s = 0; s < n; ++s) {
            u64* row = &bits[s * words];
            stack.clear();
            stack.push(s);
            while (stack.len) {
                u32 at = stack[stack.len - 1];
                --stack.len;
                for (const Edge& ed : edges) {
                    if (ed.from != at || (row[ed.to / 64] >> (ed.to % 64) & 1)) continue;
                    row[ed.to / 64] |= 1ull << (ed.to % 64);
                    stack.push(ed.to);
                }
            }
        }
        stack.free();
    }
    bool from_to(u32 a, u32 b) const { return bits[a * words + b / 64] >> (b % 64) & 1; }
    bool either(u32 a, u32 b) const { return from_to(a, b) || from_to(b, a); }
    void free() { bits.free(); }
};

const char* why_text(const Engine& e, const Edge& ed, char* buf, usize cap) {
    switch (ed.why) {
    case Why::Data:     return resource_name(e, ed.resource);
    case Why::Declared: return "declared";
    case Why::State:    format(buf, cap, "%s, in declaration order", resource_name(e, ed.resource)); return buf;
    case Why::Early:    format(buf, cap, "^%s", resource_name(e, ed.resource)); return buf;
    }
    return "?";
}

// A cycle in `edges`, if there is one: walked back from a unit inside it until a unit
// repeats, then printed forward with the reason for every step.
bool report_cycle(const Engine& e, const Array<Pending>& pend, const Array<Edge>& edges, const Reach& reach, const char* what) {
    u32 start = pend.len;
    for (u32 i = 0; i < pend.len && start == pend.len; ++i) if (reach.from_to(i, i)) start = i;
    if (start == pend.len) return false;
    // Forward from start, always to a unit that gets back to it, until start again.
    char line[1024];
    u32 at = start, n = 0;
    int k0 = format(line, sizeof line, "%s", pend[start].sys.name());
    if (k0 > 0) n = (u32)k0;
    for (u32 steps = 0; steps <= pend.len; ++steps) {
        const Edge* step = nullptr;
        for (const Edge& ed : edges)
            if (ed.from == at && (ed.to == start || reach.from_to(ed.to, start))) { step = &ed; break; }
        if (!step) break;
        char why[160];
        int k = format(line + n, sizeof line - n, " -> %s (%s)", pend[step->to].sys.name(), why_text(e, *step, why, sizeof why));
        if (k < 0 || n + (u32)k >= sizeof line) break;
        n += (u32)k;
        at = step->to;
        if (at == start) break;
    }
    KV_ERROR("engine", "a cycle among %s: %s", what, line);
    return true;
}

// Levels: one past the deepest predecessor. The graph is known to be acyclic.
void levels_of(const Array<Edge>& edges, u32 n, Array<u32>& level) {
    level.resize_fill(n, 1);
    for (bool moved = true; moved;) {
        moved = false;
        for (const Edge& ed : edges)
            if (level[ed.to] < level[ed.from] + 1) { level[ed.to] = level[ed.from] + 1; moved = true; }
    }
}

u32 count_levels(const Array<u32>& level) {
    u32 m = 0;
    for (u32 l : level) m = l > m ? l : m;
    return m;
}

// `after` or `before` names into base edges. For systems, a name in another phase is checked
// for direction; for handlers, only other handlers may be named. "?name" holds if the system
// exists, for a module ordering itself against one it does not depend on: its module may be
// missing from the build, but if the module is there the system has to be too.
bool declared_edges(const Engine& e, const Array<Pending>& pend, Phase phase, Array<Edge>& edges) {
    const char* what = phase_name(phase);
    char raw[2 * KV_KIND_NAME_MAX + 2];
    for (u32 i = 0; i < pend.len; ++i) {
        const char* me = pend[i].sys.name();
        const char* module_name = pend[i].sys.module->desc->name;
        for (u32 dir = 0; dir < 2; ++dir) {
            bool after = dir == 0;
            for (const char* cursor = after ? after_of(pend[i]) : before_of(pend[i]); name_list_next(cursor, raw, sizeof raw);) {
                const char* other = engine_qualify(module_name, raw);
                bool optional = other[0] == '?';
                if (optional) ++other;
                if (u32 j = find_pending(pend, other)) {
                    edges.push(after ? Edge{j - 1, i, 0, Why::Declared} : Edge{i, j - 1, 0, Why::Declared});
                    continue;
                }
                Phase op = engine_system_phase(e, other);
                if (op == Phase::Count) {
                    if (optional) {
                        const char* dot = strchr(other, '.');
                        usize len = dot ? (usize)(dot - other) : strlen(other);
                        bool present = false;
                        for (const ModuleInstance& inst : e.modules)
                            present = present || (strlen(inst.desc->name) == len && !strncmp(inst.desc->name, other, len));
                        if (!present) continue;
                        KV_ERROR("engine", "'%s' is %s '?%s': module '%.*s' is in the build and has no such %s", me, after ? "after" : "before", other,
                                 (int)len, other, "system or handler");
                        return false;
                    }
                    KV_ERROR("engine", "'%s' is %s unknown system or handler '%s'", me, after ? "after" : "before", other);
                    return false;
                }
                // Between cycles there is no order to declare: the loop runs them
                // (docs/design/cycles.md), so naming a unit of another cycle says nothing.
                if (op != phase && ((u32)op >= KV_SIM_PHASES || (u32)phase >= KV_SIM_PHASES)) continue;
                if (after ? (u32)op > (u32)phase : (u32)op < (u32)phase) {
                    KV_ERROR("engine", "system '%s' (phase %s) cannot run %s '%s', which is in %s phase", me, what, after ? "after" : "before", other,
                             after ? "a later" : "an earlier");
                    return false;
                }
                // another phase, the right way round: already true
            }
        }
    }
    return true;
}

// One phase's constraints, as gathered from the access lists and the declarations.
struct Constraints {
    Array<Edge> base;    // readers after writers, `after` and `before`; then module state's
    Array<Edge> early;   // an early reader before a writer, for every kind
    Array<Pair> writers; // two writers of one resource: need a path
    Array<Pair> sharers; // two units sharing a module's state, one writing
};

// Which early edges a graph takes: as the kinds say, as the engine runs, or all of them.
enum class EarlyEdges : u8 { AsDeclared, AsRun, All };

void graph_with(const Engine& e, const Constraints& c, EarlyEdges which, Array<Edge>& out) {
    for (const Edge& ed : c.base) out.push(ed);
    for (const Edge& ed : c.early) {
        const Table& t = e.tables[e.resources[ed.resource - 1].table - 1];
        bool ordered = which == EarlyEdges::All ||
                       (which == EarlyEdges::AsDeclared ? t.desc->early_reads == EarlyReads::Order : engine_early_reads(e, ed.resource) == EarlyReads::Order);
        if (ordered) out.push(ed);
    }
}

// Every pair of units, every resource they share, in one walk over two sorted lists; then
// the declared edges.
bool gather(const Engine& e, const Array<Pending>& pend, Phase phase, Constraints& c) {
    for (u32 i = 0; i < pend.len; ++i)
        for (u32 j = i + 1; j < pend.len; ++j) {
            const Array<UnitAccess>& a = pend[i].sys.access;
            const Array<UnitAccess>& b = pend[j].sys.access;
            for (u32 x = 0, y = 0; x < a.len && y < b.len;) {
                if (a[x].resource < b[y].resource) { ++x; continue; }
                if (b[y].resource < a[x].resource) { ++y; continue; }
                u32 r = a[x].resource;
                Access ma = a[x].mode, mb = b[y].mode;
                ++x, ++y;
                if (e.resources[r - 1].cls == ResourceClass::Stream) { // an emitter before a consumer as it stands
                    if (ma == Access::Append && mb == Access::Read) c.base.push(Edge{i, j, r, Why::Data});
                    if (mb == Access::Append && ma == Access::Read) c.base.push(Edge{j, i, r, Why::Data});
                    continue;
                }
                bool wa = ma == Access::Write, wb = mb == Access::Write;
                if (!wa && !wb) continue; // reads and appends never order
                if (e.resources[r - 1].cls == ResourceClass::State) { c.sharers.push(Pair{i, j, r}); continue; }
                if (wa && wb) { c.writers.push(Pair{i, j, r}); continue; }
                u32 w = wa ? i : j, o = wa ? j : i;
                Access mo = wa ? mb : ma;
                if (mo == Access::Read)  c.base.push(Edge{w, o, r, Why::Data});
                if (mo == Access::Early) c.early.push(Edge{o, w, r, Why::Early});
            }
        }
    return declared_edges(e, pend, phase, c.base);
}

// An early reader of an ordered kind must come before its writer; if base puts it after,
// the reader asks for two things at once. (From a snapshot it can come after.)
bool check_early_readers(const Engine& e, const Array<Pending>& pend, const char* what, const Constraints& c) {
    Reach r;
    r.build(c.base, pend.len);
    for (const Edge& ed : c.early) {
        u32 reader = ed.from, writer = ed.to;
        if (engine_early_reads(e, ed.resource) == EarlyReads::Snapshot || !r.from_to(writer, reader)) continue;
        const char* kind = e.tables[e.resources[ed.resource - 1].table - 1].name;
        const Edge* direct = nullptr;
        for (const Edge& b : c.base) if (b.from == writer && b.to == reader && b.why == Why::Data) { direct = &b; break; }
        if (direct)
            KV_ERROR("engine", "'%s' reads '%s' early (^) and '%s' as it stands, both written by '%s' (%s): the first wants it before '%s', the second after. "
                     "Read both early, or both as they stand, or make kind '%s' a snapshot (KindDesc::early_reads)",
                     pend[reader].sys.name(), resource_name(e, ed.resource), resource_name(e, direct->resource), pend[writer].sys.name(), what,
                     pend[writer].sys.name(), kind);
        else
            KV_ERROR("engine", "'%s' reads '%s' early (^), written by '%s' (%s), but the rest of the schedule puts it after '%s': "
                     "read it as it stands, reorder, or make kind '%s' a snapshot (KindDesc::early_reads)",
                     pend[reader].sys.name(), resource_name(e, ed.resource), pend[writer].sys.name(), what, pend[writer].sys.name(), kind);
        return false;
    }
    return true;
}

// Module state: two of a module's units that share it, one writing, and that base does not
// order, go the way one topological order of the rest has them (base and the kinds' own
// early edges; lowest rank first among the ready). Every such edge follows that order, so
// none makes a cycle, and none depends on which pair was looked at first.
bool order_state(const Engine& e, const Array<Pending>& pend, const char* what, Constraints& c) {
    u32 n = pend.len;
    Array<Edge> declared = array_in<Edge>(mem_scratch());
    graph_with(e, c, EarlyEdges::AsDeclared, declared);
    Reach r;
    r.build(declared, n);
    if (report_cycle(e, pend, declared, r, what)) return false;
    if (!c.sharers.len) return true;
    Array<u32> place = array_in<u32>(mem_scratch());
    place.resize_fill(n, 0);
    Array<u8> placed = array_in<u8>(mem_scratch());
    placed.resize_fill(n, 0);
    for (u32 at = 0; at < n; ++at) {
        u32 pick = n;
        for (u32 i = 0; i < n; ++i) {
            if (placed[i]) continue;
            bool ready = true;
            for (const Edge& ed : declared) if (ed.to == i && !placed[ed.from]) { ready = false; break; }
            if (ready && (pick == n || pend[i].rank < pend[pick].rank)) pick = i;
        }
        placed[pick] = 1;
        place[pick] = at;
    }
    // Skipped only where base already orders the pair: an order from an ordered kind's
    // early read alone would not hold with every kind a snapshot. Going along `place`, the
    // edge never contradicts one.
    Reach rb;
    rb.build(c.base, n);
    for (const Pair& p : c.sharers) {
        if (rb.either(p.a, p.b)) continue;
        bool a_first = place[p.a] < place[p.b];
        c.base.push(Edge{a_first ? p.a : p.b, a_first ? p.b : p.a, p.resource, Why::State});
    }
    return true;
}

// Two writers of one resource: a path between them in base, either way round, so they
// keep their order whatever the early reads do.
bool check_writers(const Engine& e, const Array<Pending>& pend, const char* what, const Constraints& c) {
    Reach r;
    r.build(c.base, pend.len);
    for (const Pair& p : c.writers) {
        if (r.either(p.a, p.b)) continue;
        KV_ERROR("engine", "'%s' and '%s' both write '%s' (%s) with no order between them: add `after` or `before`",
                 pend[p.a].sys.name(), pend[p.b].sys.name(), resource_name(e, p.resource), what);
        return false;
    }
    return true;
}

// The levels three ways, for the log: as run, every kind ordered, every kind a snapshot.
void log_levels(const Engine& e, u32 n, const Constraints& c, const Array<u32>& level) {
    Array<u32> lv = array_in<u32>(mem_scratch());
    levels_of(c.base, n, lv);
    u32 snapshot_levels = count_levels(lv);
    Array<Edge> all = array_in<Edge>(mem_scratch());
    graph_with(e, c, EarlyEdges::All, all);
    Reach r;
    r.build(all, n);
    bool acyclic = true;
    for (u32 i = 0; i < n; ++i) acyclic = acyclic && !r.from_to(i, i);
    if (acyclic) {
        levels_of(all, n, lv);
        KV_INFO("engine", "  %u units in %u levels; %u with every early read ordered, %u with every one a snapshot", n, count_levels(level), count_levels(lv), snapshot_levels);
    } else {
        KV_INFO("engine", "  %u units in %u levels; every early read ordered makes a cycle, %u with every one a snapshot", n, count_levels(level), snapshot_levels);
    }
}

// Why each unit sits where it does: one line for each unit before it, with the first
// reason and how many more.
void log_edges(const Engine& e, const Array<Pending>& pend, const Array<Edge>& used) {
    for (u32 i = 0; i < pend.len; ++i)
        for (u32 k = 0; k < used.len; ++k) {
            const Edge& ed = used[k];
            if (ed.to != i) continue;
            bool seen = false;
            for (u32 q = 0; q < k && !seen; ++q) seen = used[q].to == i && used[q].from == ed.from;
            if (seen) continue;
            u32 more = 0;
            for (u32 q = k + 1; q < used.len; ++q) more += used[q].to == i && used[q].from == ed.from;
            char why[160];
            if (more) KV_INFO("engine", "  %s after %s: %s and %u more", pend[i].sys.name(), pend[ed.from].sys.name(), why_text(e, ed, why, sizeof why), more);
            else      KV_INFO("engine", "  %s after %s: %s", pend[i].sys.name(), pend[ed.from].sys.name(), why_text(e, ed, why, sizeof why));
        }
}

// The units into `out` by (level, rank): insertion sort, n is small. Every edge climbs a
// level, so this is a topological order. The units' arrays move with them.
void place_units(Array<Pending>& pend, const Array<u32>& level, Array<ScheduledSystem>& out) {
    u32 first = out.len;
    for (u32 i = 0; i < pend.len; ++i) {
        pend[i].sys.level = level[i];
        pend[i].sys.rank  = pend[i].rank;
        out.push(pend[i].sys);
        pend[i].sys = ScheduledSystem{};
    }
    for (u32 i = first + 1; i < out.len; ++i) {
        ScheduledSystem s = out[i];
        u32 j = i;
        while (j > first && (out[j - 1].level > s.level || (out[j - 1].level == s.level && out[j - 1].rank > s.rank))) { out[j] = out[j - 1]; --j; }
        out[j] = s;
    }
}

// Orders one phase's units into `out`: gather the
// constraints, check them, complete them with module state, and place the units by level.
// Takes ownership of the pending units' arrays.
bool order_units(Engine& e, Array<Pending>& pend, Phase phase, Array<ScheduledSystem>& out) {
    MemScope scope;
    const char* what = phase_name(phase);
    Constraints c;
    c.base = array_in<Edge>(mem_scratch());
    c.early = array_in<Edge>(mem_scratch());
    c.writers = array_in<Pair>(mem_scratch());
    c.sharers = array_in<Pair>(mem_scratch());
    Array<Edge> used = array_in<Edge>(mem_scratch());
    bool ok = gather(e, pend, phase, c) && check_early_readers(e, pend, what, c) && order_state(e, pend, what, c) &&
              check_writers(e, pend, what, c);
    if (ok) {
        graph_with(e, c, EarlyEdges::AsRun, used);
        Reach r;
        r.build(used, pend.len);
        ok = !report_cycle(e, pend, used, r, what); // every kind forced ordered can still make one
    }
    if (!ok) {
        for (Pending& p : pend) p.sys.free_buffers();
        pend.clear();
        return false;
    }
    Array<u32> level = array_in<u32>(mem_scratch());
    levels_of(used, pend.len, level);
    if (c.early.len) log_levels(e, pend.len, c, level);
    log_edges(e, pend, used);
    place_units(pend, level, out);
    pend.clear();
    return true;
}

// A unit from its table entry: its name, its sets, its access list.
bool make_unit(Engine& e, u32 module, const char* name, const char* reads, const char* writes, const char* appends,
               const char* emits, StateUse state, Pending& p) {
    const ModuleDesc& m = *e.modules[module].desc;
    p.sys.module = &e.modules[module];
    p.sys.full_name = name;
    if (!resolve_set(e, m, name, reads, "reads", Access::Read, true, p.sys.declared) ||
        !resolve_set(e, m, name, writes, "writes", Access::Write, false, p.sys.declared) ||
        !resolve_set(e, m, name, appends, "appends", Access::Append, false, p.sys.declared) ||
        !resolve_set(e, m, name, emits, "emits", Access::Append, false, p.sys.declared) ||
        !build_access(e, p.sys, state)) {
        p.sys.free_buffers();
        return false;
    }
    return true;
}

// The snapshot kinds of one phase: what to copy at its start (a column some unit reads early
// and another writes; a kind's liveness when one reads it early and another spawns or
// destroys there), and for every unit reading early from such a kind, its own view of it.
void plan_snapshots(Engine& e, Array<ScheduledSystem>& units, Array<KindSnapshot>& out, const char* what) {
    auto any = [&](u32 resource, Access mode) {
        for (const ScheduledSystem& s : units) {
            Access m;
            if (s.mode(resource, &m) && m == mode) return true;
        }
        return false;
    };
    for (Table& t : e.tables) {
        if (engine_early_reads(e, t.resource) != EarlyReads::Snapshot) continue;
        KindSnapshot ks;
        ks.table = &t;
        ks.liveness = any(t.alive_resource, Access::Early) && any(t.alive_resource, Access::Write);
        for (Column& c : t.columns)
            if (any(c.resource, Access::Early) && any(c.resource, Access::Write)) {
                if (!ks.columns.len) ks.columns = array_in<KindSnapshot::Rows>(mem_general());
                ks.columns.push(KindSnapshot::Rows{&c, array_in<u8>(mem_general())});
            }
        if (!ks.liveness && !ks.columns.len) continue;
        ks.gens = array_in<u32>(mem_general());
        KV_INFO("engine", "  %s: snapshot of %s:%s%s", what, t.name, ks.liveness ? " liveness" : "", ks.columns.len ? "" : " (no columns)");
        for (const KindSnapshot::Rows& r : ks.columns) KV_INFO("engine", "    %s", r.column->resource_name);
        out.push(ks);
        for (ScheduledSystem& s : units) {
            bool reads_early = false;
            for (u32 k = 0; k <= e.resources[t.resource - 1].early; ++k) {
                Access m;
                reads_early = reads_early || (s.mode(t.resource + k, &m) && m == Access::Early);
            }
            if (!reads_early) continue;
            if (!s.early.len) s.early = array_in<Table>(mem_general());
            Table& view = s.early.push(Table{});
            view.columns = array_in<Column>(mem_general());
            view.columns.resize_fill(t.columns.len, Column{});
            view.index = t.index;
        }
    }
}

// Collects every system and handler with its sets as ids and orders each phase. A
// module's handlers rank after its systems, in declaration order.
bool build_schedule(Engine& e, Span<const ModuleDesc* const> modules) {
    Array<Pending> pend[(u32)Phase::Count];
    bool ok = true;
    u32 rank = 0;
    auto unique = [&](const char* name, const ModuleDesc& m) {
        for (u32 p = 0; p < (u32)Phase::Count; ++p)
            if (u32 j = find_pending(pend[p], name)) {
                KV_ERROR("engine", "'%s' declared by both '%s' and '%s'", name, pend[p][j - 1].sys.module->desc->name, m.name);
                return false;
            }
        return true;
    };
    for (u32 i = 0; ok && i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const SystemDesc& s : m.systems) {
            if (!s.name || !s.fn || (u32)s.phase >= (u32)Phase::Count) {
                KV_ERROR("engine", "module '%s' has a system with no name, no function or a bad phase", m.name);
                ok = false;
                break;
            }
            const char* name = engine_qualify(m.name, s.name);
            if (!(ok = unique(name, m))) break;
            Pending p{};
            p.sys.desc = &s;
            p.rank = rank++;
            if (!make_unit(e, i, name, s.reads, s.writes, s.appends, s.emits, s.state, p)) { ok = false; break; }
            if (!check_cycle_writes(e, p.sys, s.phase)) { p.sys.free_buffers(); ok = false; break; }
            pend[(u32)s.phase].push(p);
        }
        for (const HandlerDesc& h : m.handlers) {
            if (!ok) break;
            if (!h.name || !h.fn || !h.event) { KV_ERROR("engine", "module '%s' has a handler with no name, no function or no event", m.name); ok = false; break; }
            const char* name = engine_qualify(m.name, h.name);
            if (*h.event == 0) { KV_ERROR("engine", "handler '%s' in module '%s' is on event type '%s', which no module declares", name, m.name, h.event_name ? h.event_name : "?"); ok = false; break; }
            if ((u32)h.phase >= (u32)Phase::Count) { KV_ERROR("engine", "handler '%s' in module '%s' has no phase: say which one it runs in", name, m.name); ok = false; break; }
            if (!(ok = unique(name, m))) break;
            Pending p{};
            p.sys.handler = &h;
            p.rank = rank++;
            if (!make_unit(e, i, name, h.reads, h.writes, h.appends, h.emits, StateUse::Write, p)) { ok = false; break; }
            if (!check_cycle_writes(e, p.sys, h.phase)) { p.sys.free_buffers(); ok = false; break; }
            pend[(u32)h.phase].push(p);
        }
    }
    for (u32 p = 0; ok && p < (u32)Phase::Count; ++p) {
        KV_INFO("engine", "phase %s:", phase_name((Phase)p));
        ok = order_units(e, pend[p], (Phase)p, e.phases.systems[p]);
        if (ok) plan_snapshots(e, e.phases.systems[p], e.phases.snapshots[p], phase_name((Phase)p));
    }
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        for (Pending& q : pend[p]) q.sys.free_buffers();
        pend[p].free();
    }
    return ok;
}

// Units by rank, phase by phase, each phase's a run [first, last]. Commands, appends,
// events and tickets go by this number, so where the schedule put a unit changes none of
// them.
void number_units(Engine& e) {
    u32 unit = 0;
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        Array<ScheduledSystem>& list = e.phases.systems[p];
        u32 first = unit;
        for (u32 k = 0; k < list.len; ++k) {
            u32 below = 0; // units of this phase with a lower rank
            for (const ScheduledSystem& o : list) below += o.rank < list[k].rank;
            list[k].unit = first + below + 1;
        }
        unit += list.len;
        e.phases.first_unit[p] = first + 1;
        e.phases.last_unit[p]  = unit;
    }
    e.units = unit;
    e.phases.by_unit.resize_fill(unit, nullptr);
    for (u32 p = 0; p < (u32)Phase::Count; ++p)
        for (ScheduledSystem& s : e.phases.systems[p]) e.phases.by_unit[s.unit - 1] = &s;
}

// Triggers: "kind.Component.field" resolved to a column that keeps a previous copy and a
// scalar field, so the edge is a compare of two rows.
bool build_triggers(Engine& e, Span<const ModuleDesc* const> modules) {
    for (u32 i = 0; i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const TriggerDesc& t : m.triggers) {
            if (!t.name || !t.field) { KV_ERROR("engine", "module '%s' has a trigger with no name or field", m.name); return false; }
            const char* name  = engine_qualify(m.name, t.name);
            const char* field = engine_qualify(m.name, t.field);
            if (engine_trigger_id(e, name)) { KV_ERROR("engine", "trigger '%s' declared twice", name); return false; }
            const char* dot = strrchr(field, '.');
            if (!dot || dot == field || !dot[1]) { KV_ERROR("engine", "trigger '%s': field '%s' is not kind.Component.field", name, field); return false; }
            char column_name[2 * KV_KIND_NAME_MAX + 2];
            usize len = (usize)(dot - field);
            if (len >= sizeof column_name) { KV_ERROR("engine", "trigger '%s': column name too long", name); return false; }
            memcpy(column_name, field, len);
            column_name[len] = 0;
            u32 rid = engine_resource_id(e, column_name);
            Trigger tr{&t, nullptr, nullptr, nullptr, i};
            tr.name = name;
            tr.field_name = field;
            for (Table& tab : e.tables)
                for (const Column& c : tab.columns)
                    if (c.resource == rid) { tr.table = &tab; tr.column = &c; }
            if (!tr.column) { KV_ERROR("engine", "trigger '%s': unknown column '%s'", name, column_name); return false; }
            if (!tr.column->prev) { KV_ERROR("engine", "trigger '%s': component '%s' keeps no previous copy (declare it with CompPrev)", name, tr.column->desc->name); return false; }
            tr.field = schema_field(tr.column->desc->fields, dot + 1);
            if (!tr.field) { KV_ERROR("engine", "trigger '%s': component '%s' has no field '%s'", name, tr.column->desc->name, dot + 1); return false; }
            if (tr.field->count != 1) { KV_ERROR("engine", "trigger '%s': field '%s' is not a scalar", name, dot + 1); return false; }
            if (t.edge != TriggerEdge::Changed && tr.field->type == FieldType::Handle) { KV_ERROR("engine", "trigger '%s': a handle field can only be watched for Changed", name); return false; }
            e.triggers.push(tr);
            if (t.id) *t.id = e.triggers.len;
            KV_INFO("engine", "trigger %-2u %s on %s", e.triggers.len, name, field);
        }
    }
    return true;
}

void log_components(const Engine& e) {
    for (u32 i = 0; i < e.components.len; ++i) {
        const ComponentDesc& c = *e.components[i].desc;
        KV_INFO("engine", "component %-2u %-16s %3u bytes, %u fields%s%s", i + 1, c.name, c.size, c.fields.len,
                c.is_sim() ? "" : ", cosmetic", c.has_prev() ? ", prev" : "");
    }
}

void log_order(const Engine& e) {
    for (u32 p = 0; p < (u32)Phase::Count; ++p) {
        char line[512];
        u32 at = 0;
        u32 last_level = 0;
        for (const ScheduledSystem& s : e.phases.systems[p]) {
            const char* sep = at == 0 ? "" : s.level != last_level ? " | " : ", ";
            int n = format(line + at, sizeof line - at, "%s%s", sep, s.name());
            if (n < 0 || at + (u32)n >= sizeof line) break;
            at += (u32)n;
            last_level = s.level;
        }
        KV_INFO("engine", "order %-7s %s", phase_name((Phase)p), at ? line : "(empty)"); // '|' separates levels
    }
}

void log_kinds(const Engine& e) {
    for (const Table& t : e.tables) {
        char line[512];
        u32 at = 0;
        for (const Column& c : t.columns) {
            int n = format(line + at, sizeof line - at, "%s%s", at ? ", " : "", c.desc->name);
            if (n < 0 || at + (u32)n >= sizeof line) break;
            at += (u32)n;
        }
        KV_INFO("engine", "kind %-2u %-16s %u slots: %s", t.index + 1, t.name, t.desc->capacity, at ? line : "(no components)");
    }
}

} // namespace

// Heaps from the config, modules into them; every allocator a module gets carries its heap.
bool assign_heaps(Engine& e, Span<const ModuleDesc* const> modules, const EngineConfig& config) {
    for (const HeapDesc& h : config.heaps) {
        if (!h.name || !h.name[0]) { KV_ERROR("engine", "a heap in the config has no name"); return false; }
        if (h.soft && h.hard && h.soft > h.hard) { KV_ERROR("engine", "heap '%s': soft limit above the hard limit", h.name); return false; }
        if (!mem_heap_register(h.name, h.soft, h.hard)) { KV_ERROR("engine", "heap '%s' could not be registered (%u heaps at most)", h.name, KV_MEM_MAX_HEAPS - 1); return false; }
    }
    for (const ModuleHeapDesc& a : config.module_heaps) {
        ModuleInstance* inst = nullptr;
        for (ModuleInstance& m : e.modules) if (names_equal(m.desc->name, a.module)) inst = &m;
        if (!inst) { KV_ERROR("engine", "config assigns unknown module '%s' to heap '%s'", a.module, a.heap); return false; }
        u8 heap = 0;
        bool known = false;
        for (const HeapDesc& h : config.heaps) if (names_equal(h.name, a.heap)) { heap = mem_heap_register(h.name, 0, 0); known = true; }
        if (!known) { KV_ERROR("engine", "config assigns module '%s' to unknown heap '%s'", a.module, a.heap); return false; }
        mem_set_module_heap((u16)(inst->index + 1), heap);
        KV_INFO("engine", "module '%s' lives in heap '%s'", a.module, a.heap);
    }
    KV_UNUSED(modules);
    return true;
}

bool engine_init(Engine& e, Span<const ModuleDesc* const> modules, const EngineConfig& config) {
    KV_ASSERT(e.modules.len == 0);
    if (!check_module_names(modules)) return false;
    // 1..255, so an id the context hands out says which engine it came from.
    static u32 s_serials = 0;
    e.serial = atomic_add_fetch(&s_serials, 1u, MO_RELAXED) % 255 + 1;
    // Names for the memory messages, from the start.
    {
        const char* names[KV_MEM_MAX_MODULES];
        for (u32 i = 0; i < modules.len; ++i) names[i] = modules[i]->name;
        mem_set_module_names(names, modules.len);
    }

    for (u32 i = 0; i < modules.len; ++i) {
        ModuleInstance inst;
        inst.desc  = modules[i];
        inst.index = i;
        format(inst.state_name, sizeof inst.state_name, "%s.state", modules[i]->name);
        e.modules.push(inst);
    }
    // Every module's state is a resource its systems share; registered first so kinds
    // and declared resources cannot shadow it.
    for (ModuleInstance& inst : e.modules) {
        ResourceEntry r{inst.state_name, inst.index};
        r.cls = ResourceClass::State;
        e.resources.push(r);
        inst.state_resource = e.resources.len;
    }
    for (ModuleInstance& inst : e.modules) mem_set_module_heap((u16)(inst.index + 1), 0);
    if (!assign_heaps(e, modules, config)) { engine_shutdown(e); return false; }

    bool ok = register_events(e, modules) && register_components(e, modules) && register_kinds(e, modules, config) &&
              register_resources(e, modules) && (register_streams(e, modules), true) && build_schedule(e, modules) &&
              build_triggers(e, modules);
    if (ok) {
        // Units by rank, each with an event writer.
        number_units(e);
        e.events.init(1 + e.units);
        for (ScheduledSystem* u : e.phases.by_unit) e.events.writer(u->unit).emitter = u->name();
        e.events.types = Span<const EventTypeDesc* const>(e.event_types.data, e.event_types.len);
        e.events.streams.resize_fill(e.event_type_count + 1, EventStream{});
        e.events.stale_warned.resize_fill(e.event_type_count + 1, 0); // sized now: batches are made on any thread
        e.timers.init();
        // Threads: the main thread plus workers. Per-system hashing needs one at a time.
        u32 threads = e.threads ? e.threads : os_cpu_count();
        if (e.hash_systems && threads != 1) { KV_INFO("engine", "--hash-systems: running on one thread"); threads = 1; }
        if (threads > 64) threads = 64;
        if (!jobs_init(threads - 1)) { KV_ERROR("engine", "the job pool did not start"); ok = false; }
        KV_INFO("engine", "early reads: %s", early_reads_name(e.early_reads));
    }
    if (ok) {
#if KV_DEV
        e.trace.ring.resize_fill(1024, TraceRecord{});
#endif
        log_components(e);
        log_kinds(e);
        log_order(e);
        for (ModuleInstance& inst : e.modules) {
            const ModuleDesc& m = *inst.desc;
            // Allocations made from here on are the module's: its arena, and anything it
            // takes from the general allocator during startup.
            mem_set_owner((u16)(inst.index + 1));
            MemTag tag;
            tag.module = (u16)(inst.index + 1);
            tag.heap   = mem_module_heap((u16)(inst.index + 1));
            mem_general_tagged(inst.general, tag, m.name);
            if (m.memory.reserve && !arena_create(inst.arena, m.memory.reserve, m.name, tag)) {
                ok = false;
                break;
            }
            if (m.startup && !m.startup(e, inst)) {
                KV_ERROR("engine", "module '%s' failed to start", m.name);
                ok = false;
                break;
            }
            inst.started = true;
            KV_MARK(m.name);
            KV_INFO("engine", "module '%s' up", m.name);
        }
        mem_set_owner(0);
    }
    if (!ok) engine_shutdown(e);
    return ok;
}

void engine_shutdown(Engine& e) {
    for (u32 i = e.modules.len; i-- > 0;) {
        ModuleInstance& inst = e.modules[i];
        mem_set_owner((u16)(inst.index + 1));
        if (inst.started && inst.desc->shutdown) inst.desc->shutdown(e, inst);
        if (inst.started) KV_INFO("engine", "module '%s' down", inst.desc->name);
        if (inst.arena.base)
            KV_INFO("engine", "module '%s' arena: %llu bytes used, %llu peak, %llu committed of %llu",
                    inst.desc->name, (unsigned long long)inst.arena.used, (unsigned long long)inst.arena.peak,
                    (unsigned long long)inst.arena.committed, (unsigned long long)inst.arena.reserved);
        inst.started    = false;
        inst.state      = nullptr;
        inst.state_type = nullptr;
        arena_destroy(inst.arena); // the module's state goes with it
        mem_set_owner(0);
    }
    jobs_shutdown();
    e.commands.bytes.free();
    e.commands.count = 0;
    e.timers.free();
    e.trace.ring.free();
    e.trace.head = 0;
    e.streams.free();
    e.units = 0;
    for (const Trigger& t : e.triggers) if (t.desc->id) *t.desc->id = 0;
    e.triggers.free();
    free_tables(e); // after every module is down: nothing holds an entity any more
    e.entity_names.free();
    e.events.types = {};
    for (const ModuleInstance& inst : e.modules) event_types_unregister(inst.desc->events);
    event_types_unregister(k_core_events);
    e.event_types.free();
    unregister_components(e);
    e.phases.free();
    e.events.free();
    e.resources.free();
    // The leak report runs in mem_shutdown, after tables that outlive the engine are
    // gone; it wants module names, which are constinit and stay valid.
    const char* names[KV_MEM_MAX_MODULES];
    u32 count = e.modules.len < KV_MEM_MAX_MODULES ? e.modules.len : KV_MEM_MAX_MODULES;
    for (u32 i = 0; i < count; ++i) names[i] = e.modules[i].desc->name;
    mem_set_module_names(names, count);
    e.modules.free();
    e.event_type_count = 0;
    KV_INFO("engine", "shutdown after %llu frames, %llu ticks",
            (unsigned long long)e.frame_count, (unsigned long long)e.clock.tick_count);
}

// ---- the cycles ------------------------------------------------------------------------

u32 engine_frame(Engine& e, f64 frame_dt) {
    KV_ZONE("ticks");
    mem_scratch_reset(); // frame lifetime: everything the last frame put in scratch is gone
    mem_sample(e.frame_count); // last frame's memory, for budgets and anyone reading pressure
    if (g_prof_on) {
        const MemSample& m = mem_last_sample();
        prof_counter("mem live KB", m.live_bytes / 1024);
        prof_counter("mem committed KB", m.committed_bytes / 1024);
    }
    ++e.frame_count;
    u32 ticks = e.clock.advance(frame_dt);
    for (u32 i = 0; i < ticks; ++i) {
        // A quit asked for in a tick ends the frame's ticks there, so a run that quits on an
        // event ends on the same tick however many ticks its last frame held.
        if (e.quit_requested) { ticks = i; break; }
        u64  tick   = e.clock.tick_count; // the sim tick this is, or would be
        bool paused = e.clock.begin_tick();
        e.phases.run_tick(e, e.events, e.clock.tick_dt, tick, paused);
    }
    return ticks;
}

void engine_frame_cycle(Engine& e, f64 dt) {
    KV_ZONE("frame cycle");
    ++e.frame_cycles;
    e.phases.run_cycle(e, e.events, Phase::Frame, dt);
}

f64 engine_work(Engine& e, f64 budget) {
    f64 start = time_now();
    e.work_deadline = start + (budget > 0.0 ? budget : 0.0);
    bool any = e.phases.systems[(u32)Phase::Work].len > 0;
    e.work_pending = false;
    if (any) {
        KV_ZONE("work");
        // At least one cycle, then again while a unit said more and the budget lasts.
        for (;;) {
            e.work_more = false;
            e.phases.run_cycle(e, e.events, Phase::Work, 0.0);
            ++e.work_cycles;
            e.work_pending = e.work_more;
            if (!e.work_more || time_now() >= e.work_deadline) break;
        }
    }
    e.work_deadline = 0.0;
    return time_now() - start;
}

void engine_sim_reset(Engine& e) {
    KV_ASSERT(e.clock.paused); // a held tick: no sim system is between its reads and its writes
    u32 emptied = 0;
    for (Table& t : e.tables) {
        if (t.desc && t.desc->cosmetic) continue;
        emptied += t.live();
        t.restore_slots(Span<const u32>(), Span<const u32>()); // no slots: as at startup
    }
    u64 was = e.clock.tick_count;
    e.clock.tick_count = 0;
    e.timers.free(); // every scheduled event and its ticket
    e.timers.next_ticket = 1;
    e.timers.init();
    e.events.drop_all(); // emitted, merged or not: about the old run
    auto drop_commands = [](CommandBuffer& cb) { cb.bytes.clear(); cb.count = 0; };
    drop_commands(e.commands);
    e.commands.next_ticket = 1;
    for (Array<ScheduledSystem>& phase : e.phases.systems)
        for (ScheduledSystem& s : phase) { drop_commands(s.commands); s.spawn_tickets = s.timer_tickets = 0; }
    e.last_hash = 0;
    ++e.sim_resets;
    e.events.emit(SimReset{e.sim_resets, 0});
    KV_INFO("engine", "sim reset %u at tick %llu: %u entities gone, the sim tick back to 0", e.sim_resets, (unsigned long long)was, emptied);
}

void engine_hold(Engine& e, u32 reason, bool on) {
    u32 before = e.clock.holds;
    e.clock.holds = on ? before | reason : before & ~reason;
    if (e.clock.holds == before) return;
    bool was = e.clock.paused;
    e.clock.paused = e.clock.holds != 0;
    if (was != e.clock.paused)
        KV_INFO("engine", "%s at tick %llu (holds %#x)", e.clock.paused ? "paused" : "resumed", (unsigned long long)e.clock.tick_count, e.clock.holds);
}

u32 engine_holds(const Engine& e) { return e.clock.holds; }

void engine_pause(Engine& e, bool paused) { engine_hold(e, HoldUser, paused); }

void engine_step(Engine& e, u32 n) {
    if (!e.clock.paused) engine_pause(e, true);
    e.clock.step_ticks += n;
}

bool engine_rate(Engine& e, f64 rate) {
    if (!(rate > 0.0)) { KV_WARN("engine", "rate must be positive; pause instead"); return false; }
    e.clock.rate = rate;
    return true;
}

bool engine_time_scale(Engine& e, f32 scale) {
    if (!(scale >= 0.0f)) { KV_WARN("engine", "time scale must be zero or more"); return false; }
    e.clock.scale = (f64)scale;
    return true;
}

f32 engine_time_scale_get(const Engine& e) { return (f32)e.clock.scale; }

// ---- commands ------------------------------------------------------------------------
// Records in a byte stream: a header, then for a spawn one entry per component.
namespace {

// The entity name map's lock: names are set from boundaries and from code outside the
// schedule, looked up from anywhere.
void names_lock(Engine& e)   { while (atomic_exchange(&e.entity_names_lock, 1u, MO_ACQUIRE)) {} }
void names_unlock(Engine& e) { atomic_store(&e.entity_names_lock, 0u, MO_RELEASE); }

enum CommandOp : u8 { CMD_SPAWN, CMD_DESTROY, CMD_DESTROY_ALL, CMD_NAME, CMD_APPEND };

struct CommandHeader {
    u8     op;
    u32    kind;       // kind id (spawn); resource id (append)
    u32    ticket;     // spawn
    u32    components; // spawn: entries that follow; append: the record's bytes, padded to 8 after
    Handle entity;     // destroy
};

struct ComponentEntryHeader {
    u32 component; // component id
    u32 size;      // bytes that follow, padded to 8
};

template <class T>
void put(Array<u8>& bytes, const T& v) { memcpy(bytes.push_n(sizeof(T)), &v, sizeof(T)); }
template <class T>
const T* take(const u8*& p) { const T* v = (const T*)p; p += sizeof(T); return v; }

} // namespace

u32 command_spawn_begin(CommandBuffer& cb, u32 kind_id, u32 component_count, u32 ticket) {
    if (ticket == 0 && cb.next_ticket) ticket = cb.next_ticket++;
    CommandHeader h{CMD_SPAWN, kind_id, ticket, component_count, Handle{}};
    put(cb.bytes, h);
    ++cb.count;
    return h.ticket;
}

void command_spawn_component(CommandBuffer& cb, u32 component_id, const void* data, u32 size) {
    ComponentEntryHeader h{component_id, size};
    put(cb.bytes, h);
    u32 padded = (size + 7) & ~7u;
    u8* dst = cb.bytes.push_n(padded);
    memcpy(dst, data, size);
    if (padded > size) memset(dst + size, 0, padded - size);
}

void command_destroy(CommandBuffer& cb, Handle h) {
    put(cb.bytes, CommandHeader{CMD_DESTROY, 0, 0, 0, h});
    ++cb.count;
}

void command_destroy_all(CommandBuffer& cb, u32 kind_id) {
    put(cb.bytes, CommandHeader{CMD_DESTROY_ALL, kind_id, 0, 0, Handle{}});
    ++cb.count;
}

void command_name(CommandBuffer& cb, Handle h, u64 name_hash) {
    put(cb.bytes, CommandHeader{CMD_NAME, 0, 0, 0, h});
    put(cb.bytes, name_hash);
    ++cb.count;
}

void command_append(CommandBuffer& cb, u32 resource_id, const void* record, u32 size) {
    put(cb.bytes, CommandHeader{CMD_APPEND, resource_id, 0, size, Handle{}});
    u32 padded = (size + 7) & ~7u;
    u8* dst = cb.bytes.push_n(padded);
    memcpy(dst, record, size);
    if (padded > size) memset(dst + size, 0, padded - size);
    ++cb.count;
}

namespace {

void apply_buffer(Engine& e, CommandBuffer& cb) {
    if (cb.count == 0) return;
    const u8* p   = cb.bytes.data;
    const u8* end = p + cb.bytes.len;
    Handle last; // the buffer's last spawn, for a name that follows it
    while (p < end) {
        const CommandHeader* h = take<CommandHeader>(p);
        if (h->op == CMD_NAME) {
            u64 name = *take<u64>(p);
            Handle target = h->entity.is_null() ? last : h->entity;
            if (!target.is_null()) engine_entity_name_set(e, target, name ? name_str(Name{name}) : nullptr);
            continue;
        }
        if (h->op == CMD_APPEND) {
            const u8* data = p;
            p += (h->components + 7) & ~7u;
            const ResourceEntry& r = e.resources[h->kind - 1];
            u16 was = mem_owner();
            mem_set_owner((u16)(r.module + 1));
            r.apply(e, data, h->components);
            mem_set_owner(was);
            continue;
        }
        if (h->op == CMD_SPAWN) {
            Table* t = engine_table_now(e, h->kind);
            Handle entity = t ? t->spawn() : Handle{};
            last = entity;
            for (u32 i = 0; i < h->components; ++i) {
                const ComponentEntryHeader* ch = take<ComponentEntryHeader>(p);
                const u8* data = p;
                p += (ch->size + 7) & ~7u;
                if (entity.is_null()) continue;
                Column* c = t->column(ch->component);
                if (!c || c->desc->size != ch->size) {
                    KV_ERROR("engine", "spawn_later of '%s': component '%s' is not one of its", t->name,
                             engine_component(e, ch->component) ? engine_component(e, ch->component)->name : "?");
                    continue;
                }
                memcpy(c->data + (usize)entity.index() * c->stride, data, ch->size);
                if (c->prev) memcpy(c->prev + (usize)entity.index() * c->stride, data, ch->size);
            }
            e.events.emit(Spawned{h->ticket, h->kind, entity}); // null entity: the kind was full or unknown
        } else if (h->op == CMD_DESTROY_ALL) {
            if (Table* t = engine_table_now(e, h->kind)) t->destroy_all();
        } else {
            for (Table& t : e.tables)
                if (t.kind == h->entity.kind()) { t.destroy(h->entity); break; } // stale: silently nothing
        }
    }
    cb.bytes.clear();
    cb.count = 0;
}

} // namespace

void engine_apply_commands(Engine& e) {
    apply_buffer(e, e.commands);
    for (ScheduledSystem* u : e.phases.by_unit) apply_buffer(e, u->commands);
    // Names of the dead leave the reverse map, which nothing else reads while this runs.
    for (Table& t : e.tables) {
        if (!t.sparse_dirty) continue;
        MemScope scope;
        Array<Name> gone = array_in<Name>(mem_scratch());
        names_lock(e);
        t.sweep_sparse(&gone);
        for (Name n : gone) {
            Handle* at = e.entity_names.find(n.hash);
            if (at && engine_table_of_now(e, *at) == &t && t.name_of(*at) != n) e.entity_names.remove(n.hash);
        }
        names_unlock(e);
    }
}

// ---- entity names ---------------------------------------------------------------------------

namespace {

// Whether `h` is alive and has the name `n` now.
bool name_holds(Engine& e, Handle h, Name n) {
    Table* t = engine_table_of_now(e, h);
    return t && t->name_of(h) == n;
}

} // namespace

bool engine_entity_name_set(Engine& e, Handle entity, const char* name) {
    Table* t = engine_table_of_now(e, entity);
    if (!t || !t->valid(entity)) { KV_WARN("engine", "naming a stale entity '%s'", name ? name : ""); return false; }
    Name n = name && name[0] ? name_intern(name) : Name{};
    names_lock(e);
    Name old = t->name_of(entity);
    if (!n.is_null()) {
        Handle* at = e.entity_names.find(n.hash);
        if (at && *at != entity && name_holds(e, *at, n)) {
            Handle other = *at;
            names_unlock(e);
            char who[96];
            kv_handle_describe(other.bits, who, sizeof who);
            KV_WARN("engine", "name '%s' is taken by %.40s", name, who);
            return false;
        }
    }
    if (old == n) { names_unlock(e); return true; }
    if (!old.is_null()) {
        Handle* at = e.entity_names.find(old.hash);
        if (at && *at == entity) e.entity_names.remove(old.hash);
    }
    if (n.is_null()) {
        u32 i = t->names.find(entity.index());
        if (i != KV_POOL_NONE) t->names.erase(i);
    } else {
        *(Name*)t->names.insert(entity.index(), entity.gen()) = n;
        e.entity_names.set(n.hash, entity);
    }
    names_unlock(e);
    return true;
}

Handle engine_entity_find(Engine& e, Name name) {
    if (name.is_null()) return Handle{};
    names_lock(e);
    Handle* at = e.entity_names.find(name.hash);
    Handle h = at && name_holds(e, *at, name) ? *at : Handle{};
    names_unlock(e);
    return h;
}

Handle engine_entity_find(Engine& e, const char* name) {
    if (!name || !name[0]) return Handle{};
    return engine_entity_find(e, name_lookup(name));
}

const char* engine_entity_name(Engine& e, Handle entity) {
    Table* t = engine_table_of_now(e, entity);
    if (!t) return nullptr;
    names_lock(e);
    Name n = t->name_of(entity);
    names_unlock(e);
    return n.is_null() ? nullptr : name_str(n);
}

void engine_entity_names_each(Engine& e, bool (*fn)(Handle entity, Name name, void* user), void* user) {
    // Copied out first, so the callback may name and look up.
    MemScope scope;
    Array<Handle> hs = array_in<Handle>(mem_scratch());
    Array<Name> ns = array_in<Name>(mem_scratch());
    names_lock(e);
    for (u32 i = 0; i < e.entity_names.len(); ++i) {
        Handle h = e.entity_names.values[i];
        Name n{e.entity_names.keys[i]};
        if (name_holds(e, h, n)) { hs.push(h); ns.push(n); }
    }
    names_unlock(e);
    for (u32 i = 0; i < hs.len; ++i)
        if (!fn(hs[i], ns[i], user)) return;
}

void engine_fire_triggers(Engine& e, u64 tick) {
    KV_UNUSED(tick);
    for (u32 ti = 0; ti < e.triggers.len; ++ti) {
        const Trigger& t = e.triggers[ti];
        const Table& tab = *t.table;
        u32 n = tab.rows();
        e.events.set_emitter(t.name);
        for (u32 i = 0; i < n; ++i) {
            if (!tab.is_alive(i)) continue;
            const u8* cur  = t.column->data + (usize)i * t.column->stride;
            const u8* prev = t.column->prev + (usize)i * t.column->stride;
            bool fired = false;
            f64 value = 0.0;
            if (t.desc->edge == TriggerEdge::Changed) {
                usize bytes = field_type_size(t.field->type);
                fired = memcmp(cur + t.field->offset, prev + t.field->offset, bytes) != 0;
                if (fired) schema_field_as_f64(*t.field, cur, &value);
            } else {
                f64 before = 0.0;
                schema_field_as_f64(*t.field, cur, &value);
                schema_field_as_f64(*t.field, prev, &before);
                f64 v = t.desc->value;
                fired = t.desc->edge == TriggerEdge::CrossedBelow ? (before >= v && value < v) : (before <= v && value > v);
            }
            if (!fired) continue;
            e.events.emit(StateEdge{tab.handle_at(i), ti + 1, 0, value});
            ++e.edges_fired;
        }
    }
    e.events.set_emitter("");
}

int engine_describe_event(const Engine& e, u32 type_id, const void* event, char* out, usize cap) {
    const EventTypeDesc* d = type_id < e.event_types.len ? e.event_types[type_id] : nullptr;
    if (!d) return format(out, cap, "event#%u", type_id);
    if (d->fields.len == 0) return format(out, cap, "%s{..}", d->name);
    return schema_describe(d->name, d->fields, event, out, cap);
}

void engine_trace_delivery(Engine& e, u64 tick, u32 phase) {
#if KV_DEV
    if (e.trace.ring.len == 0) return;
    EventBus& bus = e.events;
    const char* where = phase < (u32)Phase::Count ? phase_name((Phase)phase) : "boundary";
    for (u32 t = 1; t < bus.streams.len; ++t) {
        EventStream& st = bus.streams[t];
        if (st.traced < st.base) st.traced = st.base;
        const EventBuffer& b = st.events;
        for (u64 at = st.traced; at < st.end(); ++at) {
            u32 i = (u32)(at - st.base);
            const u8* ev = b.bytes.data + (usize)i * b.elem_size;
            TraceRecord& r = e.trace.ring[e.trace.head];
            r.tick    = tick;
            r.emitter = i < b.emitters.len ? b.emitters[i] : "(not noted: --spike-hunt)";
            r.type    = t;
            r.phase   = (u16)phase;
            r.size    = (u8)(b.elem_size < sizeof r.data ? b.elem_size : sizeof r.data);
            memcpy(r.data, ev, r.size);
            e.trace.head = (e.trace.head + 1) % e.trace.ring.len;
            ++e.trace.total;
            if (e.trace.print) {
                char text[256];
                engine_describe_event(e, t, ev, text, sizeof text);
                KV_INFO("trace", "tick %llu %s: %s <- %s", (unsigned long long)tick, where, text, r.emitter[0] ? r.emitter : "(outside the schedule)");
            }
        }
        st.traced = st.end();
    }
#else
    KV_UNUSED(e); KV_UNUSED(tick); KV_UNUSED(phase);
#endif
}

u32 engine_trigger_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.triggers.len; ++i)
        if (names_equal(e.triggers[i].name, name)) return i + 1;
    return 0;
}

void engine_request_quit(Engine& e) { e.quit_requested = true; }

Table* engine_table_of_now(Engine& e, Handle h) {
    if (h.is_null()) return nullptr;
    const kv_handle_kind_desc* d = kv_handle_kind_desc_get(h.kind());
    if (!d || !d->ctx) return nullptr;
    Table* t = (Table*)d->ctx;
    if (t < e.tables.data || t >= e.tables.data + e.tables.len || t->kind != h.kind()) return nullptr;
    return t;
}

namespace {

// Dev builds: a table reached around the context by a unit that reads that kind early from
// a snapshot. Its view is the kind as the phase found it; the table is the kind now.
Table* checked_direct(Table* t) {
#if KV_DEV
    const ScheduledSystem* u = ctx_running();
    if (t && u)
        for (const Table& v : u->early)
            if (v.index == t->index) {
                KV_ERROR("engine", "'%s' reads kind '%s' early from a snapshot and reaches the table directly (engine_table): "
                         "that is the kind as it is now, not as the phase found it. Reach it through the context (ctx.view, ctx.read, ctx.read_kind)",
                         u->name(), t->name);
                KV_VERIFY(!"a snapshot kind reached around the context: see the log");
            }
#endif
    return t;
}

} // namespace

Table* engine_table_of(Engine& e, Handle h) { return checked_direct(engine_table_of_now(e, h)); }

// Every sim column of every live row, in slot order, with the entity's handle so identity
// counts too. Field by field, so padding never matters. Each table's hash starts from its
// name and the tables are summed, so the order kinds registered in (which moves when a
// module is added) changes nothing; handles in fields hash their kind by name for the same
// reason. An empty table is no state: a module that adds a kind changes no hash until
// something is in it.
u64 engine_hash_state(const Engine& e) {
    u64 h = 0x6b6f72766964ull; // "korvid"
    for (const Table& t : e.tables)
        if (!t.desc->cosmetic && t.live()) h += hash_mix(table_hash(t));
    return hash_mix(h);
}

ModuleInstance* engine_module(Engine& e, const char* name) {
    for (ModuleInstance& inst : e.modules)
        if (inst.started && names_equal(inst.desc->name, name)) return &inst;
    return nullptr;
}

void* engine_module_state(Engine& e, const char* name) {
    ModuleInstance* inst = engine_module(e, name);
    return inst ? inst->state : nullptr;
}

EarlyReads engine_early_reads(const Engine& e, u32 resource_id) {
    if (e.early_reads != EarlyReadsChoice::PerKind) return e.early_reads == EarlyReadsChoice::Snapshot ? EarlyReads::Snapshot : EarlyReads::Order;
    u32 table = resource_id && resource_id <= e.resources.len ? e.resources[resource_id - 1].table : 0;
    return table ? e.tables[table - 1].desc->early_reads : EarlyReads::Order;
}

u32 engine_resource_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.resources.len; ++i)
        if (names_equal(e.resources[i].name, name)) return i + 1;
    return 0;
}

bool engine_publish(Engine& e, const ModuleInstance& self, const char* name, void* ptr, const void* type) {
    name = engine_qualify(self.desc->name, name);
    u32 id = engine_resource_id(e, name);
    if (!id) { KV_ERROR("engine", "module '%s' publishes unknown resource '%s'", self.desc->name, name); return false; }
    ResourceEntry& r = e.resources[id - 1];
    if (r.module != self.index) {
        KV_ERROR("engine", "module '%s' publishes resource '%s', which belongs to '%s'", self.desc->name, name, e.modules[r.module].desc->name);
        return false;
    }
    r.ptr  = ptr;
    r.type = type;
    return true;
}

u32 engine_component_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.components.len; ++i)
        if (names_equal(e.components[i].desc->name, name)) return i + 1;
    return 0;
}

const ComponentDesc* engine_component(const Engine& e, u32 id) {
    return id && id <= e.components.len ? e.components[id - 1].desc : nullptr;
}

Phase engine_system_phase(const Engine& e, const char* name) {
    for (const ModuleInstance& inst : e.modules) {
        for (const SystemDesc& s : inst.desc->systems)
            if (names_equal(engine_qualify(inst.desc->name, s.name), name)) return s.phase;
        for (const HandlerDesc& h : inst.desc->handlers)
            if (h.name && names_equal(engine_qualify(inst.desc->name, h.name), name)) return h.phase;
    }
    return Phase::Count;
}

u32 engine_kind_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.tables.len; ++i)
        if (names_equal(e.tables[i].name, name)) return i + 1;
    return 0;
}

Table* engine_table_now(Engine& e, u32 kind_id) {
    return kind_id && kind_id <= e.tables.len ? &e.tables[kind_id - 1] : nullptr;
}

Table* engine_table(Engine& e, u32 kind_id) { return checked_direct(engine_table_now(e, kind_id)); }

Table* engine_table(Engine& e, const char* name) { return engine_table(e, engine_kind_id(e, name)); }

const char* engine_qualify(const char* module, const char* name) {
    if (!name) return "";
    bool optional = name[0] == '?';
    const char* body = optional ? name + 1 : name;
    if (body[0] != '.' || !module) return name;
    char full[2 * KV_KIND_NAME_MAX + 64];
    int n = format(full, sizeof full, "%s%s%s", optional ? "?" : "", module, body);
    if (n <= 0 || (usize)n >= sizeof full) { KV_ERROR("engine", "name '%s' in module '%s' is too long", name, module); return name; }
    return name_str(name_intern(full));
}

bool name_list_next(const char*& cursor, char* out, usize cap) {
    if (!cursor) return false;
    for (;;) {
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t' || *cursor == '\n') ++cursor;
        if (!*cursor) return false;
        const char* start = cursor;
        while (*cursor && *cursor != ',') ++cursor;
        const char* end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) --end;
        usize len = (usize)(end - start);
        if (len == 0) continue;
        if (len >= cap) len = cap - 1;
        memcpy(out, start, len);
        out[len] = 0;
        return true;
    }
}

bool engine_add_waker(Engine& e, bool (*fn)(Engine&, void*), void* user) {
    if (!fn || e.waker_count >= KV_COUNTOF(e.wakers)) return false;
    e.wakers[e.waker_count++] = Engine::Waker{fn, user};
    return true;
}

void engine_remove_waker(Engine& e, bool (*fn)(Engine&, void*), void* user) {
    for (u32 i = 0; i < e.waker_count; ++i)
        if (e.wakers[i].fn == fn && e.wakers[i].user == user) { e.wakers[i] = e.wakers[--e.waker_count]; return; }
}

bool engine_wake(Engine& e) {
    bool any = false;
    for (u32 i = 0; i < e.waker_count; ++i) any |= e.wakers[i].fn(e, e.wakers[i].user);
    return any;
}

} // namespace kv
