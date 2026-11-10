// The registry. Reads the module list once, in order, and turns the declarations into
// ids, a schedule and started instances. Every check here names what clashed.
#include "korvid/core/engine.h"
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
#include <stdio.h>
#include <string.h>

namespace kv {

namespace {

bool names_equal(const char* a, const char* b) { return strcmp(a, b) == 0; }


// The engine's own event types, registered after every module's so module ids are
// unaffected by what the core adds.
constinit const FieldDesc k_spawned_fields[]    = {KV_FIELD(Spawned, ticket), KV_FIELD(Spawned, kind), KV_FIELD(Spawned, entity)};
constinit const FieldDesc k_state_edge_fields[] = {KV_FIELD(StateEdge, entity), KV_FIELD(StateEdge, trigger), KV_FIELD(StateEdge, value)};
constinit const EventTypeDesc k_core_events[] = {
    KV_EVENT_EX(Spawned, k_spawned_fields, 0),
    KV_EVENT_EX(StateEdge, k_state_edge_fields, 0),
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
            e.resources.push(ResourceEntry{name, i});
            t.resource = e.resources.len;
            for (Column& c : t.columns) {
                e.resources.push(ResourceEntry{c.resource_name, i});
                c.resource = e.resources.len;
            }
            for (Attachment& a : t.attachments) {
                e.resources.push(ResourceEntry{a.resource_name, i});
                a.resource = e.resources.len;
            }
            if (k.props) {
                e.resources.push(ResourceEntry{t.props_resource_name, i});
                t.props_resource = e.resources.len;
            }
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
            entry.sink = r.sink;
            e.resources.push(entry);
        }
    }
    return true;
}

// Names to ids. "*.Component" is that column in every kind, and must exist somewhere;
// "*[Other].Component" is that column in every kind that also has Other ("*[A+B].Component":
// has both), and may match none (a module's system that moves only what opted in); ".x" is "<module>.x"; the list
// is comma-separated.
bool resolve_resource_refs(const Engine& e, const ModuleDesc& m, const char* unit_name,
                           const char* refs, const char* kind, Array<u32>& out, Array<u32>* early = nullptr) {
    char raw_buf[2 * KV_KIND_NAME_MAX + 2];
    for (const char* cursor = refs; name_list_next(cursor, raw_buf, sizeof raw_buf);) {
        // "^name": a read of the phase's earlier value (see SystemDesc::reads).
        char* raw = raw_buf;
        u32 first = out.len;
        if (raw[0] == '^') {
            if (!early) { KV_ERROR("engine", "'%s' in module '%s' %s '%s': '^' marks reads only", unit_name, m.name, kind, raw); return false; }
            ++raw;
        }
        bool is_early = raw != raw_buf;
        auto mark = [&]() { if (is_early) for (u32 k = first; k < out.len; ++k) early->push(out[k]); };
        if (raw[0] == '*' && raw[1] == '[') {
            char* close = strchr(raw, ']');
            if (!close || close[1] != '.') {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s': write *[Component].Column", unit_name, m.name, kind, raw);
                return false;
            }
            *close = 0;
            // The filter: one component, or several joined by '+', all of which a kind has.
            u32 filter[4], nf = 0;
            u32 cid = engine_component_id(e, close + 2);
            for (char* f = raw + 2; f;) {
                char* plus = strchr(f, '+');
                if (plus) *plus = 0;
                u32 id = engine_component_id(e, f);
                if (!id || nf == KV_COUNTOF(filter)) {
                    KV_ERROR("engine", "'%s' in module '%s' %s '*[...].%s': %s '%s'", unit_name, m.name, kind, close + 2,
                             id ? "more than four components in the filter at" : "unknown component", f);
                    return false;
                }
                filter[nf++] = id;
                f = plus ? plus + 1 : nullptr;
            }
            if (!cid) {
                KV_ERROR("engine", "'%s' in module '%s' %s '*[...].%s': unknown component '%s'", unit_name, m.name, kind, close + 2, close + 2);
                return false;
            }
            for (const Table& t : e.tables) {
                bool all = true;
                for (u32 k = 0; k < nf; ++k) all = all && t.has(filter[k]);
                if (all)
                    if (const Column* c = t.column(cid)) out.push(c->resource);
            }
            mark();
            continue;
        }
        if (raw[0] == '*' && raw[1] == '.' && raw[2] == '@') {
            // Every kind's attachments of a component, or every kind's bags.
            bool bags = strcmp(raw + 3, "props") == 0;
            u32 cid = bags ? 0 : engine_component_id(e, raw + 3);
            bool any = false;
            for (const Table& t : e.tables) {
                if (bags && t.props_resource) { out.push(t.props_resource); any = true; }
                if (const Attachment* a = cid ? t.attachment(cid) : nullptr) { out.push(a->resource); any = true; }
            }
            if (!any) {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s' but no kind has %s", unit_name, m.name, kind, raw, bags ? "bags" : "it attachable");
                return false;
            }
            mark();
            continue;
        }
        if (raw[0] == '*' && raw[1] == '.') {
            u32 cid = engine_component_id(e, raw + 2);
            bool any = false;
            for (const Table& t : e.tables)
                if (const Column* c = t.column(cid)) { out.push(c->resource); any = true; }
            if (!any) {
                KV_ERROR("engine", "'%s' in module '%s' %s '%s' but no kind has component '%s'",
                         unit_name, m.name, kind, raw, raw + 2);
                return false;
            }
            mark();
            continue;
        }
        const char* name = engine_qualify(m.name, raw);
        u32 id = engine_resource_id(e, name);
        if (!id) {
            KV_ERROR("engine", "'%s' in module '%s' %s unknown resource '%s'",
                     unit_name, m.name, kind, name);
            return false;
        }
        out.push(id);
        mark();
    }
    return true;
}

// ---- the schedule -----------------------------------------------------------------
// Within a phase the systems form a DAG: a reader runs after every writer of what it
// reads, `after` and `before` add edges by hand, and two writers of one resource with no
// path between them is an error, as is a cycle. Ties break by module order then
// declaration order, so the order is deterministic and the log shows why.

struct Edge {
    u32 from, to;
    u32 resource; // 0: declared with after/before
};

struct Pending {
    ScheduledSystem sys;
    Array<u32>      reads_eff;  // reads plus, for a kind, every column of it
    Array<u32>      writes_eff;
    Array<u32>      early_eff;  // the reads marked '^': before this phase's writers, not after
    u32             rank;       // module order then declaration order
};

const char* after_of(const Pending& p)  { return p.sys.desc ? p.sys.desc->after : p.sys.handler->after; }
const char* before_of(const Pending& p) { return p.sys.desc ? p.sys.desc->before : p.sys.handler->before; }

const char* resource_name(const Engine& e, u32 id) { return id && id <= e.resources.len ? e.resources[id - 1].name : "?"; }

bool contains(const Array<u32>& ids, u32 id) {
    for (u32 x : ids) if (x == id) return true;
    return false;
}

// A kind resource stands for the kind and all of its columns.
void expand(const Engine& e, const Array<u32>& ids, Array<u32>& out) {
    for (u32 id : ids) {
        if (!contains(out, id)) out.push(id);
        for (const Table& t : e.tables)
            if (t.resource == id)
                for (const Column& c : t.columns)
                    if (!contains(out, c.resource)) out.push(c.resource);
    }
}

// The first resource both touch that is a real one, not a module's implicit state.
u32 first_conflict(const Engine& e, const Array<u32>& a, const Array<u32>& b) {
    for (u32 x : a) if (contains(b, x) && !e.resources[x - 1].implicit) return x;
    return 0;
}

const Column* column_of(const Engine& e, u32 resource) {
    for (const Table& t : e.tables)
        for (const Column& c : t.columns)
            if (c.resource == resource) return &c;
    return nullptr;
}

// Sim state flows to presentation, never back: a system that reads a cosmetic column may
// not write a sim one.
bool check_flow(const Engine& e, const Pending& p) {
    for (u32 r : p.reads_eff) {
        const Column* rc = column_of(e, r);
        if (!rc || rc->desc->is_sim()) continue;
        for (u32 w : p.writes_eff) {
            const Column* wc = column_of(e, w);
            if (wc && wc->desc->is_sim()) {
                KV_ERROR("engine", "'%s' reads cosmetic '%s' and writes sim '%s': data flows from sim to presentation, never back",
                         p.sys.name(), rc->resource_name, wc->resource_name);
                return false;
            }
        }
    }
    return true;
}

bool reaches(const Array<Edge>& edges, u32 from, u32 to, Array<u8>& seen) {
    if (from == to) return true;
    if (seen[from]) return false;
    seen[from] = 1;
    for (const Edge& ed : edges)
        if (ed.from == from && reaches(edges, ed.to, to, seen)) return true;
    return false;
}

bool reaches(const Array<Edge>& edges, u32 n, u32 from, u32 to) {
    Array<u8> seen = array_in<u8>(mem_scratch());
    seen.resize_fill(n, 0);
    bool r = reaches(edges, from, to, seen);
    seen.free();
    return r;
}

u32 find_pending(const Array<Pending>& pend, const char* name) {
    for (u32 i = 0; i < pend.len; ++i)
        if (names_equal(pend[i].sys.name(), name)) return i + 1;
    return 0;
}

// Walks edges from `start` among the unplaced systems until a system repeats, and prints
// the loop so the message names the path.
void report_cycle(const Array<Pending>& pend, const Array<Edge>& edges, const Array<u8>& placed, u32 start) {
    Array<u32> path = array_in<u32>(mem_scratch());
    u32 at = start;
    for (;;) {
        for (u32 x : path) if (x == at) goto found;
        path.push(at);
        bool stepped = false;
        for (const Edge& ed : edges)
            if (ed.to == at && !placed[ed.from]) { at = ed.from; stepped = true; break; }
        if (!stepped) break;
    }
found:
    // The walk went backwards along edges from `start` and stopped at a repeat, `at`; the
    // loop forward is `at`, then the path from its end back to where `at` sits in it.
    u32 from = 0;
    for (u32 i = 0; i < path.len; ++i) if (path[i] == at) from = i;
    char line[1024];
    u32 n = 0;
    int k0 = snprintf(line, sizeof line, "%s", pend[at].sys.name());
    if (k0 > 0) n = (u32)k0;
    for (u32 i = path.len; i-- > from;) {
        int k = snprintf(line + n, sizeof line - n, " -> %s", pend[path[i]].sys.name());
        if (k < 0 || n + (u32)k >= sizeof line) break;
        n += (u32)k;
    }
    KV_ERROR("engine", "a cycle among %s: %s", pend[start].sys.desc ? "systems in one phase" : "handlers", line);
    path.free();
}

void free_pending(Array<Pending>& pend) {
    for (Pending& p : pend) { p.sys.reads.free(); p.sys.writes.free(); p.reads_eff.free(); p.writes_eff.free(); p.early_eff.free(); }
    pend.free();
}

// Orders one phase's systems (or, with phase == Count, the handlers) into `out`. Takes
// ownership of the pending units' arrays.
bool order_units(Engine& e, Array<Pending>& pend, Phase phase, Array<ScheduledSystem>& out) {
    Array<Edge> edges = array_in<Edge>(mem_scratch());
    u32 n = pend.len;
    const char* what = phase == Phase::Count ? "handlers" : phase_name(phase);

    // Readers after writers; a pair writing the same thing is remembered for the check below.
    // A module's state is not a conflict: its systems keep declaration order among themselves.
    // A read marked '^' turns its edge around: that reader goes before the writers.
    for (u32 i = 0; i < n; ++i)
        for (u32 j = 0; j < n; ++j) {
            if (i == j) continue;
            for (u32 x : pend[i].writes_eff) {
                if (!contains(pend[j].reads_eff, x) || e.resources[x - 1].implicit) continue;
                if (contains(pend[j].early_eff, x)) edges.push(Edge{j, i, x});
                else edges.push(Edge{i, j, x});
                break;
            }
        }

    // Declared order. For systems, a name in another phase is checked for direction; for
    // handlers, only other handlers may be named. A name starting with '?' is an order
    // that holds if the system exists: for a module ordering itself against one it does
    // not depend on.
    char raw[2 * KV_KIND_NAME_MAX + 2];
    for (u32 i = 0; i < n; ++i) {
        const char* me = pend[i].sys.name();
        const char* module_name = pend[i].sys.module->desc->name;
        for (const char* cursor = after_of(pend[i]); name_list_next(cursor, raw, sizeof raw);) {
            const char* other = engine_qualify(module_name, raw);
            bool optional = other[0] == '?';
            if (optional) ++other;
            u32 j = find_pending(pend, other);
            if (!j) {
                Phase op = phase == Phase::Count ? Phase::Count : engine_system_phase(e, other);
                if (op == Phase::Count) {
                    if (optional) continue;
                    KV_ERROR("engine", "'%s' is after unknown %s '%s'", me, phase == Phase::Count ? "handler" : "system", other); edges.free(); return false;
                }
                if ((u32)op > (u32)phase) { KV_ERROR("engine", "system '%s' (phase %s) cannot run after '%s', which is in a later phase", me, what, other); edges.free(); return false; }
                continue; // an earlier phase: already true
            }
            edges.push(Edge{j - 1, i, 0});
        }
        for (const char* cursor = before_of(pend[i]); name_list_next(cursor, raw, sizeof raw);) {
            const char* other = engine_qualify(module_name, raw);
            bool optional = other[0] == '?';
            if (optional) ++other;
            u32 j = find_pending(pend, other);
            if (!j) {
                Phase op = phase == Phase::Count ? Phase::Count : engine_system_phase(e, other);
                if (op == Phase::Count) {
                    if (optional) continue;
                    KV_ERROR("engine", "'%s' is before unknown %s '%s'", me, phase == Phase::Count ? "handler" : "system", other); edges.free(); return false;
                }
                if ((u32)op < (u32)phase) { KV_ERROR("engine", "system '%s' (phase %s) cannot run before '%s', which is in an earlier phase", me, what, other); edges.free(); return false; }
                continue;
            }
            edges.push(Edge{i, j - 1, 0});
        }
    }
    // Two systems of one module that both touch its state, one of them writing, and have
    // no path between them yet keep declaration order. Handlers run one at a time: no need.
    if (phase != Phase::Count)
        for (u32 i = 0; i < n; ++i)
            for (u32 j = i + 1; j < n; ++j) {
                if (pend[i].sys.module != pend[j].sys.module) continue;
                u32 st = pend[i].sys.module->state_resource;
                bool wi = contains(pend[i].writes_eff, st), wj = contains(pend[j].writes_eff, st);
                bool ti = wi || contains(pend[i].reads_eff, st), tj = wj || contains(pend[j].reads_eff, st);
                if (!(ti && tj && (wi || wj))) continue;
                if (reaches(edges, n, i, j) || reaches(edges, n, j, i)) continue;
                edges.push(Edge{i, j, st});
            }
    // Two writers of one resource need a path between them, either way round. A sink (a
    // command queue) is the exception: appends from anywhere land in unit order.
    for (u32 i = 0; i < n; ++i)
        for (u32 j = i + 1; j < n; ++j) {
            u32 r = first_conflict(e, pend[i].writes_eff, pend[j].writes_eff);
            while (r && e.resources[r - 1].sink) {
                // Skip sinks and look for a real conflict beyond them.
                u32 next = 0;
                for (u32 x : pend[i].writes_eff)
                    if (x > r && contains(pend[j].writes_eff, x) && !e.resources[x - 1].implicit && !e.resources[x - 1].sink) { next = x; break; }
                r = next;
            }
            if (!r) continue;
            if (reaches(edges, n, i, j) || reaches(edges, n, j, i)) continue;
            KV_ERROR("engine", "'%s' and '%s' both write '%s' (%s) with no order between them: add `after` or `before`",
                     pend[i].sys.name(), pend[j].sys.name(), resource_name(e, r), what);
            edges.free();
            return false;
        }

    // Topological order, lowest rank first among the ready. A unit's level is one past
    // its deepest predecessor; the output is then sorted by level, rank, which is still a
    // topological order since every edge climbs a level.
    Array<u8> placed = array_in<u8>(mem_scratch());
    placed.resize_fill(n, 0);
    Array<u32> level = array_in<u32>(mem_scratch());
    level.resize_fill(n, 0);
    u32 first_out = out.len;
    for (u32 done = 0; done < n; ++done) {
        u32 pick = n;
        for (u32 i = 0; i < n; ++i) {
            if (placed[i]) continue;
            bool ready = true;
            for (const Edge& ed : edges) if (ed.to == i && !placed[ed.from]) { ready = false; break; }
            if (ready && (pick == n || pend[i].rank < pend[pick].rank)) pick = i;
        }
        if (pick == n) {
            for (u32 i = 0; i < n; ++i) if (!placed[i]) { report_cycle(pend, edges, placed, i); break; }
            placed.free(); edges.free();
            return false;
        }
        placed[pick] = 1;
        u32 lv = 1;
        for (const Edge& ed : edges)
            if (ed.to == pick && level[ed.from] + 1 > lv) lv = level[ed.from] + 1;
        level[pick] = lv;
        pend[pick].sys.level = lv;
        pend[pick].sys.rank  = pend[pick].rank;
        out.push(pend[pick].sys);
        pend[pick].sys.reads = pend[pick].sys.writes = Array<u32>{}; // now owned by the schedule
        // Log why it sits where it does.
        for (const Edge& ed : edges)
            if (ed.to == pick)
                KV_INFO("engine", "  %s after %s: %s", pend[pick].sys.name(), pend[ed.from].sys.name(),
                        ed.resource ? resource_name(e, ed.resource) : "declared");
    }
    // Sort the new entries by (level, rank): insertion sort, n is small.
    for (u32 i = first_out + 1; i < out.len; ++i) {
        ScheduledSystem s = out[i];
        u32 j = i;
        while (j > first_out && (out[j - 1].level > s.level || (out[j - 1].level == s.level && out[j - 1].rank > s.rank))) { out[j] = out[j - 1]; --j; }
        out[j] = s;
    }
    level.free();
    placed.free();
    edges.free();
    return true;
}

// Collects every system with its sets as ids, checks the flow rule, and orders each phase.
bool build_schedule(Engine& e, Span<const ModuleDesc* const> modules) {
    Array<Pending> pend[(u32)Phase::Count];
    bool ok = true;
    u32 rank = 0;
    for (u32 i = 0; ok && i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const SystemDesc& s : m.systems) {
            if (!s.name || !s.fn || (u32)s.phase >= (u32)Phase::Count) {
                KV_ERROR("engine", "module '%s' has a system with no name, no function or a bad phase", m.name);
                ok = false;
                break;
            }
            const char* name = engine_qualify(m.name, s.name);
            for (u32 p = 0; p < (u32)Phase::Count; ++p)
                if (u32 j = find_pending(pend[p], name)) {
                    KV_ERROR("engine", "system '%s' declared by both '%s' and '%s'", name, pend[p][j - 1].sys.module->desc->name, m.name);
                    ok = false;
                }
            if (!ok) break;
            Pending p{};
            p.sys.desc = &s; p.sys.module = &e.modules[i]; p.sys.full_name = name; p.rank = rank++;
            Array<u32> early;
            if (!resolve_resource_refs(e, m, name, s.reads, "reads", p.sys.reads, &early) ||
                !resolve_resource_refs(e, m, name, s.writes, "writes", p.sys.writes)) {
                p.sys.reads.free(); p.sys.writes.free(); early.free();
                ok = false;
                break;
            }
            expand(e, p.sys.reads, p.reads_eff);
            expand(e, p.sys.writes, p.writes_eff);
            expand(e, early, p.early_eff);
            early.free();
            if (s.state == StateUse::Write) p.writes_eff.push(e.modules[i].state_resource);
            else if (s.state == StateUse::Read) p.reads_eff.push(e.modules[i].state_resource);
            pend[(u32)s.phase].push(p);
            if (!check_flow(e, pend[(u32)s.phase].last())) { ok = false; break; }
        }
    }
    for (u32 p = 0; ok && p < (u32)Phase::Count; ++p) {
        KV_INFO("engine", "phase %s:", phase_name((Phase)p));
        ok = order_units(e, pend[p], (Phase)p, e.phases.systems[p]);
    }
    for (u32 p = 0; p < (u32)Phase::Count; ++p) free_pending(pend[p]);
    return ok;
}

// Handlers: one ordered list for every boundary, built like a phase. A handler's event
// must be a declared type.
bool build_handlers(Engine& e, Span<const ModuleDesc* const> modules) {
    Array<Pending> pend;
    bool ok = true;
    u32 rank = 0;
    for (u32 i = 0; ok && i < modules.len; ++i) {
        const ModuleDesc& m = *modules[i];
        for (const HandlerDesc& h : m.handlers) {
            if (!h.name || !h.fn || !h.event) { KV_ERROR("engine", "module '%s' has a handler with no name, no function or no event", m.name); ok = false; break; }
            const char* name = engine_qualify(m.name, h.name);
            if (*h.event == 0) { KV_ERROR("engine", "handler '%s' in module '%s' is on event type '%s', which no module declares", name, m.name, h.event_name ? h.event_name : "?"); ok = false; break; }
            if (find_pending(pend, name)) { KV_ERROR("engine", "handler '%s' declared twice", name); ok = false; break; }
            Pending p{};
            p.sys.module = &e.modules[i]; p.sys.handler = &h; p.sys.full_name = name; p.rank = rank++;
            if (!resolve_resource_refs(e, m, name, h.reads, "reads", p.sys.reads) ||
                !resolve_resource_refs(e, m, name, h.writes, "writes", p.sys.writes)) {
                p.sys.reads.free(); p.sys.writes.free();
                ok = false;
                break;
            }
            expand(e, p.sys.reads, p.reads_eff);
            expand(e, p.sys.writes, p.writes_eff);
            pend.push(p);
            if (!check_flow(e, pend.last())) { ok = false; break; }
        }
    }
    if (ok && pend.len) {
        KV_INFO("engine", "handlers:");
        ok = order_units(e, pend, Phase::Count, e.handlers);
    }
    free_pending(pend);
    return ok;
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
            int n = snprintf(line + at, sizeof line - at, "%s%s", sep, s.name());
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
            int n = snprintf(line + at, sizeof line - at, "%s%s", at ? ", " : "", c.desc->name);
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
        snprintf(inst.state_name, sizeof inst.state_name, "%s.state", modules[i]->name);
        e.modules.push(inst);
    }
    // Every module's state is a resource its systems share; registered first so kinds
    // and declared resources cannot shadow it.
    for (ModuleInstance& inst : e.modules) {
        e.resources.push(ResourceEntry{inst.state_name, inst.index, nullptr, nullptr, true});
        inst.state_resource = e.resources.len;
    }
    for (ModuleInstance& inst : e.modules) mem_set_module_heap((u16)(inst.index + 1), 0);
    if (!assign_heaps(e, modules, config)) { engine_shutdown(e); return false; }

    bool ok = register_events(e, modules) && register_components(e, modules) && register_kinds(e, modules, config) &&
              register_resources(e, modules) && build_schedule(e, modules) && build_handlers(e, modules) &&
              build_triggers(e, modules);
    if (ok) {
        // Units: systems in phase order, then handlers. Each owns an event writer.
        u32 unit = 0;
        for (u32 p = 0; p < (u32)Phase::Count; ++p)
            for (ScheduledSystem& s : e.phases.systems[p]) s.unit = ++unit;
        for (ScheduledSystem& h : e.handlers) h.unit = ++unit;
        e.units = unit;
        e.events.init(1 + unit);
        for (u32 p = 0; p < (u32)Phase::Count; ++p)
            for (ScheduledSystem& s : e.phases.systems[p]) e.events.writer(s.unit).emitter = s.name();
        for (ScheduledSystem& h : e.handlers) e.events.writer(h.unit).emitter = h.name();
        e.events.types = Span<const EventTypeDesc* const>(e.event_types.data, e.event_types.len);
        e.timers.init();
        // Threads: the main thread plus workers. Per-system hashing needs one at a time.
        u32 threads = e.threads ? e.threads : os_cpu_count();
        if (e.hash_systems && threads != 1) { KV_INFO("engine", "--hash-systems: running on one thread"); threads = 1; }
        if (threads > 64) threads = 64;
        if (!jobs_init(threads - 1)) ok = false;
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
    e.idle.free();
    e.commands.bytes.free();
    e.commands.count = 0;
    e.timers.free();
    e.trace.ring.free();
    e.trace.head = 0;
    for (ScheduledSystem& h : e.handlers) h.free_buffers();
    e.handlers.free();
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

// ---- idle tasks ------------------------------------------------------------------------

u32 engine_idle_add(Engine& e, const char* name, IdleFn fn, void* user) {
    if (!fn) return 0;
    IdleTask t{name ? name : "idle", fn, user, e.idle_next_id++, mem_owner(), 0, 0.0};
    e.idle.push(t);
    return t.id;
}

bool engine_idle_remove(Engine& e, u32 id) {
    for (u32 i = 0; i < e.idle.len; ++i) {
        if (e.idle[i].id != id) continue;
        for (u32 j = i + 1; j < e.idle.len; ++j) e.idle[j - 1] = e.idle[j];
        --e.idle.len;
        if (e.idle_cursor > i) --e.idle_cursor;
        return true;
    }
    return false;
}

// Round-robin from the cursor: a round visits every task once; rounds continue while a
// task answered More and time remains. A task that answers Done leaves the list and the
// cursor stays on its successor.
f64 engine_idle_run(Engine& e, f64 budget) {
    if (e.idle.len == 0 || budget <= 0.0) return 0.0;
    KV_ZONE("idle");
    f64 start = time_now();
    f64 used  = 0.0;
    ++e.idle_stats.runs;
    bool more = true;
    while (more && e.idle.len) {
        more = false;
        for (u32 n = 0; n < e.idle.len && used < budget;) {
            u32 i = e.idle_cursor % e.idle.len;
            IdleTask& t = e.idle[i];
            u16 was = mem_owner();
            mem_set_owner(t.owner);
            f64 t0 = time_now();
            IdleResult r = t.fn(e, budget - used, t.user);
            f64 dt = time_now() - t0;
            mem_set_owner(was);
            t.calls++;
            t.seconds += dt;
            ++e.idle_stats.calls;
            used = time_now() - start;
            if (r == IdleResult::Done) { engine_idle_remove(e, t.id); continue; } // n stays: one fewer task in the round
            e.idle_cursor = (i + 1) % e.idle.len;
            ++n;
            if (r == IdleResult::More) more = true;
        }
    }
    e.idle_stats.seconds += used;
    e.idle_stats.last     = used;
    return used;
}

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

enum CommandOp : u8 { CMD_SPAWN, CMD_DESTROY, CMD_DESTROY_ALL, CMD_NAME };

struct CommandHeader {
    u8     op;
    u32    kind;       // kind id (spawn)
    u32    ticket;     // spawn
    u32    components; // spawn: entries that follow
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
        if (h->op == CMD_SPAWN) {
            Table* t = engine_table(e, h->kind);
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
            if (Table* t = engine_table(e, h->kind)) t->destroy_all();
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
    for (u32 p = 0; p < (u32)Phase::Count; ++p)
        for (ScheduledSystem& s : e.phases.systems[p]) apply_buffer(e, s.commands);
    for (ScheduledSystem& h : e.handlers) apply_buffer(e, h.commands);
    // Names of the dead leave the reverse map, which nothing else reads while this runs.
    for (Table& t : e.tables) {
        if (!t.sparse_dirty) continue;
        MemScope scope;
        Array<Name> gone = array_in<Name>(mem_scratch());
        names_lock(e);
        t.sweep_sparse(&gone);
        for (Name n : gone) {
            Handle* at = e.entity_names.find(n.hash);
            if (at && engine_table_of(e, *at) == &t && t.name_of(*at) != n) e.entity_names.remove(n.hash);
        }
        names_unlock(e);
    }
}

// ---- entity names ---------------------------------------------------------------------------

namespace {

// Whether `h` is alive and has the name `n` now.
bool name_holds(Engine& e, Handle h, Name n) {
    Table* t = engine_table_of(e, h);
    return t && t->name_of(h) == n;
}

} // namespace

bool engine_entity_name_set(Engine& e, Handle entity, const char* name) {
    Table* t = engine_table_of(e, entity);
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
    Table* t = engine_table_of(e, entity);
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
    if (!d) return snprintf(out, cap, "event#%u", type_id);
    if (d->fields.len == 0) return snprintf(out, cap, "%s{..}", d->name);
    return schema_describe(d->name, d->fields, event, out, cap);
}

void engine_trace_delivery(Engine& e, u64 tick, u32 wave) {
#if KV_DEV
    if (e.trace.ring.len == 0) return;
    EventBus& bus = e.events;
    if (bus.dropped_last && e.trace.print)
        KV_INFO("trace", "tick %llu wave %u: %u event%s dropped for stale handles", (unsigned long long)tick, wave,
                bus.dropped_last, bus.dropped_last == 1 ? "" : "s");
    for (u32 t = 1; t < bus.delivered.len; ++t) {
        u32 n = bus.new_count(t);
        if (!n) continue;
        const EventBuffer& b = bus.delivered[t];
        u32 start = b.count() - n;
        for (u32 i = 0; i < n; ++i) {
            const u8* ev = b.bytes.data + (usize)(start + i) * b.elem_size;
            TraceRecord& r = e.trace.ring[e.trace.head];
            r.tick    = tick;
            r.emitter = b.emitters[start + i];
            r.type    = t;
            r.wave    = (u16)wave;
            r.size    = (u8)(b.elem_size < sizeof r.data ? b.elem_size : sizeof r.data);
            memcpy(r.data, ev, r.size);
            e.trace.head = (e.trace.head + 1) % e.trace.ring.len;
            ++e.trace.total;
            if (e.trace.print) {
                char text[256];
                engine_describe_event(e, t, ev, text, sizeof text);
                KV_INFO("trace", "tick %llu wave %u: %s <- %s", (unsigned long long)tick, wave, text, r.emitter[0] ? r.emitter : "(outside the schedule)");
            }
        }
    }
#else
    KV_UNUSED(e); KV_UNUSED(tick); KV_UNUSED(wave);
#endif
}

u32 engine_trigger_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.triggers.len; ++i)
        if (names_equal(e.triggers[i].name, name)) return i + 1;
    return 0;
}

void engine_request_quit(Engine& e) { e.quit_requested = true; }

Table* engine_table_of(Engine& e, Handle h) {
    if (h.is_null()) return nullptr;
    const kv_handle_kind_desc* d = kv_handle_kind_desc_get(h.kind());
    if (!d || !d->ctx) return nullptr;
    Table* t = (Table*)d->ctx;
    if (t < e.tables.data || t >= e.tables.data + e.tables.len || t->kind != h.kind()) return nullptr;
    return t;
}

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
    for (const ModuleInstance& inst : e.modules)
        for (const SystemDesc& s : inst.desc->systems)
            if (names_equal(engine_qualify(inst.desc->name, s.name), name)) return s.phase;
    return Phase::Count;
}

u32 engine_kind_id(const Engine& e, const char* name) {
    for (u32 i = 0; i < e.tables.len; ++i)
        if (names_equal(e.tables[i].name, name)) return i + 1;
    return 0;
}

Table* engine_table(Engine& e, u32 kind_id) {
    return kind_id && kind_id <= e.tables.len ? &e.tables[kind_id - 1] : nullptr;
}

Table* engine_table(Engine& e, const char* name) { return engine_table(e, engine_kind_id(e, name)); }

const char* engine_qualify(const char* module, const char* name) {
    if (!name) return "";
    bool optional = name[0] == '?';
    const char* body = optional ? name + 1 : name;
    if (body[0] != '.' || !module) return name;
    char full[2 * KV_KIND_NAME_MAX + 64];
    int n = snprintf(full, sizeof full, "%s%s%s", optional ? "?" : "", module, body);
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

} // namespace kv
