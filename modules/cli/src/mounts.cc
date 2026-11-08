// The engine's own mounts: what core knows, as paths. Kinds down to a field, the
// schedule, the resources, event types, triggers, the clock, the modules, memory and the
// profiler's zones. Everything here reads engine records that exist anyway; the only
// writes are a kind's fields (through the schema) and the clock.
#include "cli_internal.h"
#include "korvid/core/engine.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

const char* yes_no(bool b) { return b ? "yes" : "no"; }

// ---- /kinds -----------------------------------------------------------------------------------
// /kinds/<module>/<kind>/            the kind: live count, columns
// /kinds/<module>/<kind>/<slot>      a live row: its describe
// /kinds/<module>/<kind>/<slot>/<Component>[/<field>]   a component, a field (patchable)

// The table whose slashed name is the longest prefix of `rel`, and what follows it.
Table* kind_of(Engine& e, const char* rel, const char** rest) {
    Table* best = nullptr;
    usize best_len = 0;
    for (Table& t : e.tables) {
        char slashed[KV_KIND_NAME_MAX];
        cli_dots_to_slashes(t.name, slashed, sizeof slashed);
        usize n = strlen(slashed);
        if (strncmp(rel, slashed, n) == 0 && (rel[n] == 0 || rel[n] == '/') && n > best_len) { best = &t; best_len = n; }
    }
    if (best) *rest = rel[best_len] == '/' ? rel + best_len + 1 : rel + best_len;
    return best;
}

bool parse_slot(const char* s, u32* out) {
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end) return false;
    *out = (u32)v;
    return true;
}

// A row's describe without its handle: "kind#slot:gen Transform2{...} ..." from the first
// component on. The handle ends at the space before the first brace.
int row_summary(const Table& t, u32 slot, char* out, usize cap) {
    char full[512];
    t.describe(t.handle_at(slot), full, sizeof full);
    const char* body = full;
    if (const char* brace = strchr(full, '{')) {
        const char* sp = brace;
        while (sp > full && *sp != ' ') --sp;
        if (*sp == ' ') body = sp + 1;
    }
    return snprintf(out, cap, "%.120s", body);
}

void kinds_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    Engine& e = *ctx.engine;
    const char* rest = nullptr;
    Table* t = dir[0] ? kind_of(e, dir, &rest) : nullptr;
    if (!t) { // a module directory, or the root: the kinds as a tree
        CliTree tree{dir, emit, user};
        for (Table& tab : e.tables) {
            char slashed[KV_KIND_NAME_MAX], summary[96];
            cli_dots_to_slashes(tab.name, slashed, sizeof slashed);
            snprintf(summary, sizeof summary, "%u live of %u%s", tab.live(), tab.desc->capacity, tab.desc->cosmetic ? ", cosmetic" : "");
            if (!tree.add(slashed, true, summary)) return;
        }
        return;
    }
    // Inside a kind: rest is "" (rows), "<slot>/" (components) or "<slot>/<Component>/" (fields).
    char rbuf[128];
    strncpy(rbuf, rest, sizeof rbuf - 1); rbuf[sizeof rbuf - 1] = 0;
    usize rl = strlen(rbuf);
    if (rl && rbuf[rl - 1] == '/') rbuf[--rl] = 0;
    if (!rl) {
        for (u32 i = 0; i < t->rows(); ++i) {
            if (!t->is_alive(i)) continue;
            char name[16], summary[160];
            snprintf(name, sizeof name, "%u/", i);
            row_summary(*t, i, summary, sizeof summary);
            if (!emit(user, name, summary)) return;
        }
        return;
    }
    char* slash = strchr(rbuf, '/');
    if (slash) *slash = 0;
    u32 slot;
    if (!parse_slot(rbuf, &slot) || slot >= t->rows() || !t->is_alive(slot)) return;
    if (!slash) {
        for (const Column& c : t->columns) {
            char name[64], summary[200];
            snprintf(name, sizeof name, "%s/", c.desc->name);
            component_describe(*c.desc, c.data + (usize)slot * c.stride, summary, sizeof summary);
            if (!emit(user, name, summary)) return;
        }
        return;
    }
    const char* comp = slash + 1;
    for (const Column& c : t->columns) {
        if (strcmp(c.desc->name, comp) != 0) continue;
        const u8* row = c.data + (usize)slot * c.stride;
        for (const FieldDesc& f : c.desc->fields) {
            char summary[128];
            usize at = 0;
            for (u32 l = 0; l < f.count && at < sizeof summary; ++l) {
                f64 v = 0.0;
                if (schema_field_lane(f, row, l, &v)) at += (usize)snprintf(summary + at, sizeof summary - at, "%s%g", l ? " " : "", v);
                else { Handle h; memcpy(&h, row + f.offset, sizeof h); at += (usize)snprintf(summary + at, sizeof summary - at, "%s", h.is_null() ? "null" : kind_name(h)); break; }
            }
            if (!emit(user, f.name, summary)) return;
        }
        return;
    }
}

// Resolves rel to table, slot, column and field (null where the path stops); false if
// any present component is not there.
bool kind_node(Engine& e, const char* rel, Table** table, i64* slot, const Column** column, const FieldDesc** field) {
    *table = nullptr; *slot = -1; *column = nullptr; *field = nullptr;
    const char* rest = nullptr;
    Table* t = kind_of(e, rel, &rest);
    if (!t) return false;
    *table = t;
    if (!rest[0]) return true;
    char buf[128];
    strncpy(buf, rest, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char* p = buf;
    char* s1 = strchr(p, '/'); if (s1) *s1++ = 0;
    u32 sl;
    if (!parse_slot(p, &sl) || sl >= t->rows() || !t->is_alive(sl)) return false;
    *slot = sl;
    if (!s1 || !*s1) return true;
    char* s2 = strchr(s1, '/'); if (s2) *s2++ = 0;
    for (const Column& c : t->columns) if (strcmp(c.desc->name, s1) == 0) *column = &c;
    if (!*column) return false;
    if (!s2 || !*s2) return true;
    if (strchr(s2, '/')) return false;
    *field = schema_field((*column)->desc->fields, s2);
    return *field != nullptr;
}

bool kinds_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    Table* t; i64 slot; const Column* col; const FieldDesc* f;
    if (!rel[0] || !kind_node(e, rel, &t, &slot, &col, &f)) return false;
    if (slot < 0) {
        char line[512];
        usize at = 0;
        for (const Column& c : t->columns)
            at += (usize)snprintf(line + at, sizeof line - at, "%s%s%s%s", at ? ", " : "", c.desc->name, c.desc->has_prev() ? " (prev)" : "", c.desc->is_sim() ? "" : " (cosmetic)");
        ctx.print("%s: %u live of %u, module %s%s", t->name, t->live(), t->desc->capacity, e.modules[t->module].desc->name, t->desc->cosmetic ? ", cosmetic kind" : "");
        ctx.print("columns: %s", line);
        return true;
    }
    if (!col) {
        char text[1024];
        t->describe(t->handle_at((u32)slot), text, sizeof text);
        ctx.print("%s", text);
        return true;
    }
    const u8* row = col->data + (usize)slot * col->stride;
    if (!f) {
        char text[512];
        component_describe(*col->desc, row, text, sizeof text);
        ctx.print("%s", text);
        if (col->prev) { component_describe(*col->desc, col->prev + (usize)slot * col->stride, text, sizeof text); ctx.print("prev %s", text); }
        return true;
    }
    char line[256];
    usize at = 0;
    for (u32 l = 0; l < f->count; ++l) {
        f64 v = 0.0;
        if (schema_field_lane(*f, row, l, &v)) at += (usize)snprintf(line + at, sizeof line - at, "%s%g", l ? " " : "", v);
        else { Handle h; memcpy(&h, row + f->offset, sizeof h); char hd[160]; kv_handle_describe(h.bits, hd, sizeof hd); at += (usize)snprintf(line + at, sizeof line - at, "%s", hd); break; }
    }
    ctx.print("%s", line);
    return true;
}

bool kinds_patch(CliContext& ctx, const char* rel, Span<const char* const> values) {
    Engine& e = *ctx.engine;
    Table* t; i64 slot; const Column* col; const FieldDesc* f;
    if (!rel[0] || !kind_node(e, rel, &t, &slot, &col, &f)) { ctx.fail("no such path under /kinds: %s", rel); return false; }
    if (!f) { ctx.fail("patch a field: /kinds/<module>/<kind>/<slot>/<Component>/<field> value..."); return false; }
    if (f->type == FieldType::Handle) { ctx.fail("a handle field cannot be patched"); return false; }
    if (values.len != f->count) { ctx.fail("field '%s' takes %u value%s", f->name, f->count, f->count == 1 ? "" : "s"); return false; }
    u8* row = const_cast<Column*>(col)->data + (usize)slot * col->stride;
    for (u32 l = 0; l < f->count; ++l) {
        f32 v;
        if (!cli_arg_f32(ctx, values[l], f->name, &v)) return false;
        schema_field_set(*f, row, l, (f64)v);
    }
    return kinds_describe(ctx, rel);
}

// ---- /systems and /handlers -----------------------------------------------------------------------

void sets_line(Engine& e, const Array<u32>& ids, char* out, usize cap) {
    usize at = 0;
    out[0] = 0;
    for (u32 id : ids) {
        if (id > e.resources.len) continue;
        at += (usize)snprintf(out + at, cap - at, "%s%s", at ? ", " : "", e.resources[id - 1].name);
        if (at >= cap) break;
    }
}

template <class F>
void each_system(Engine& e, F fn) {
    for (u32 p = 0; p < (u32)Phase::Count; ++p)
        for (ScheduledSystem& s : e.phases.systems[p]) fn(s, (Phase)p);
}

void systems_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    CliTree tree{dir, emit, user};
    each_system(*ctx.engine, [&](ScheduledSystem& s, Phase p) {
        char slashed[128], summary[96];
        cli_dots_to_slashes(s.name(), slashed, sizeof slashed);
        snprintf(summary, sizeof summary, "%s, level %u%s%s", phase_name(p), s.level, s.desc->main_thread ? ", main thread" : "", s.desc->runs == RunWhen::Always ? ", always" : "");
        tree.add(slashed, false, summary);
    });
}

bool systems_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    char dotted[128];
    cli_slashes_to_dots(rel, dotted, sizeof dotted);
    bool found = false;
    each_system(e, [&](ScheduledSystem& s, Phase p) {
        if (found || strcmp(s.name(), dotted) != 0) return;
        found = true;
        char line[1024];
        ctx.print("%s: phase %s, level %u, unit %u, module %s", s.name(), phase_name(p), s.level, s.unit, s.module->desc->name);
        sets_line(e, s.reads, line, sizeof line);  ctx.print("reads:  %s", line[0] ? line : "(nothing)");
        sets_line(e, s.writes, line, sizeof line); ctx.print("writes: %s", line[0] ? line : "(nothing)");
        ctx.print("after: %s; before: %s; state %s; main thread %s; runs %s", s.desc->after ? s.desc->after : "-", s.desc->before ? s.desc->before : "-",
                  s.desc->state == StateUse::Write ? "write" : s.desc->state == StateUse::Read ? "read" : "none",
                  yes_no(s.desc->main_thread), s.desc->runs == RunWhen::Always ? "always" : "unpaused");
    });
    return found;
}

void handlers_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    Engine& e = *ctx.engine;
    CliTree tree{dir, emit, user};
    for (ScheduledSystem& h : e.handlers) {
        char slashed[128], summary[96];
        cli_dots_to_slashes(h.name(), slashed, sizeof slashed);
        snprintf(summary, sizeof summary, "on %s, level %u", h.handler->event_name ? h.handler->event_name : "?", h.level);
        if (!tree.add(slashed, false, summary)) return;
    }
}

bool handlers_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    char dotted[128];
    cli_slashes_to_dots(rel, dotted, sizeof dotted);
    for (ScheduledSystem& h : e.handlers) {
        if (strcmp(h.name(), dotted) != 0) continue;
        char line[1024];
        ctx.print("%s: on %s, level %u, unit %u, module %s", h.name(), h.handler->event_name ? h.handler->event_name : "?", h.level, h.unit, h.module->desc->name);
        sets_line(e, h.reads, line, sizeof line);  ctx.print("reads:  %s", line[0] ? line : "(nothing)");
        sets_line(e, h.writes, line, sizeof line); ctx.print("writes: %s", line[0] ? line : "(nothing)");
        ctx.print("after: %s; before: %s", h.handler->after ? h.handler->after : "-", h.handler->before ? h.handler->before : "-");
        return true;
    }
    return false;
}

// ---- /resources ----------------------------------------------------------------------------------

void resources_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    Engine& e = *ctx.engine;
    CliTree tree{dir, emit, user};
    for (u32 i = 0; i < e.resources.len; ++i) {
        const ResourceEntry& r = e.resources[i];
        if (r.implicit) continue;
        char slashed[128], summary[96];
        cli_dots_to_slashes(r.name, slashed, sizeof slashed);
        snprintf(summary, sizeof summary, "%s%s%s", e.modules[r.module].desc->name, r.ptr ? ", published" : "", r.sink ? ", sink" : "");
        if (!tree.add(slashed, false, summary)) return;
    }
}

bool resources_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    char dotted[128];
    cli_slashes_to_dots(rel, dotted, sizeof dotted);
    u32 id = engine_resource_id(e, dotted);
    if (!id) return false;
    const ResourceEntry& r = e.resources[id - 1];
    ctx.print("%s: id %u, module %s, %s%s", r.name, id, e.modules[r.module].desc->name,
              r.ptr ? "published (a column or a struct; see /kinds for columns)" : "declared, nothing published", r.sink ? ", sink" : "");
    return true;
}

// ---- /events and /triggers ----------------------------------------------------------------------

void fields_line(Span<const FieldDesc> fields, char* out, usize cap) {
    usize at = 0;
    out[0] = 0;
    for (const FieldDesc& f : fields) {
        at += (usize)snprintf(out + at, cap - at, "%s%s %s%s", at ? ", " : "", field_type_name(f.type), f.name, f.count > 1 ? "[]" : "");
        if (at >= cap) break;
    }
}

void events_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    Engine& e = *ctx.engine;
    for (u32 i = 1; i < e.event_types.len; ++i) {
        const EventTypeDesc* d = e.event_types[i];
        if (!d) continue;
        char summary[96];
        snprintf(summary, sizeof summary, "%u bytes, %u fields%s", d->size, d->fields.len, d->next_tick() ? ", next tick" : "");
        if (!emit(user, d->name, summary)) return;
    }
}

bool events_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    for (u32 i = 1; i < e.event_types.len; ++i) {
        const EventTypeDesc* d = e.event_types[i];
        if (!d || strcmp(d->name, rel) != 0) continue;
        char line[512];
        fields_line(d->fields, line, sizeof line);
        ctx.print("%s: id %u, %u bytes, align %u%s", d->name, i, d->size, d->align, d->next_tick() ? ", delivered next tick" : "");
        ctx.print("fields: %s", line[0] ? line : "(no schema)");
        return true;
    }
    return false;
}

const char* edge_name(TriggerEdge e) { return e == TriggerEdge::Changed ? "changed" : e == TriggerEdge::CrossedBelow ? "crossed below" : "crossed above"; }

void triggers_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    Engine& e = *ctx.engine;
    CliTree tree{dir, emit, user};
    for (const Trigger& t : e.triggers) {
        char slashed[128], summary[128];
        cli_dots_to_slashes(t.name, slashed, sizeof slashed);
        snprintf(summary, sizeof summary, "%s %s %g", t.field_name, edge_name(t.desc->edge), t.desc->value);
        if (!tree.add(slashed, false, summary)) return;
    }
}

bool triggers_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    char dotted[128];
    cli_slashes_to_dots(rel, dotted, sizeof dotted);
    for (u32 i = 0; i < e.triggers.len; ++i) {
        const Trigger& t = e.triggers[i];
        if (strcmp(t.name, dotted) != 0) continue;
        ctx.print("%s: id %u, %s %s %g, module %s", t.name, i + 1, t.field_name, edge_name(t.desc->edge), t.desc->value, e.modules[t.module].desc->name);
        return true;
    }
    return false;
}

// ---- /clock -------------------------------------------------------------------------------------

struct ClockLeaf { const char* name; const char* what; };
const ClockLeaf k_clock_leaves[] = {
    {"tick", "the sim tick"}, {"tick_total", "every tick, paused ones included"}, {"paused", "1 while the sim holds; patchable"},
    {"rate", "time scale while unpaused; patchable"}, {"step", "sim ticks still to let through a pause; patchable"},
    {"dt", "seconds per tick"}, {"alpha", "the render interpolation point"}, {"dropped", "ticks' worth of time dropped at the cap"},
    {"frame", "frames so far"},
};

int clock_value(Engine& e, const char* name, char* out, usize cap) {
    const TickClock& c = e.clock;
    if (!strcmp(name, "tick"))       return snprintf(out, cap, "%llu", (unsigned long long)c.tick_count);
    if (!strcmp(name, "tick_total")) return snprintf(out, cap, "%llu", (unsigned long long)c.tick_total);
    if (!strcmp(name, "paused"))     return snprintf(out, cap, "%d", c.paused ? 1 : 0);
    if (!strcmp(name, "rate"))       return snprintf(out, cap, "%g", c.rate);
    if (!strcmp(name, "step"))       return snprintf(out, cap, "%u", c.step_ticks);
    if (!strcmp(name, "dt"))         return snprintf(out, cap, "%g", c.tick_dt);
    if (!strcmp(name, "alpha"))      return snprintf(out, cap, "%.3f", (f64)c.alpha());
    if (!strcmp(name, "dropped"))    return snprintf(out, cap, "%llu", (unsigned long long)c.dropped_time_ticks);
    if (!strcmp(name, "frame"))      return snprintf(out, cap, "%llu", (unsigned long long)e.frame_count);
    return -1;
}

void clock_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    for (const ClockLeaf& l : k_clock_leaves) {
        char v[64], summary[128];
        clock_value(*ctx.engine, l.name, v, sizeof v);
        snprintf(summary, sizeof summary, "%-8s %s", v, l.what);
        if (!emit(user, l.name, summary)) return;
    }
}

bool clock_describe(CliContext& ctx, const char* rel) {
    char v[64];
    if (!rel[0]) { for (const ClockLeaf& l : k_clock_leaves) { clock_value(*ctx.engine, l.name, v, sizeof v); ctx.print("%-10s %s", l.name, v); } return true; }
    if (clock_value(*ctx.engine, rel, v, sizeof v) < 0) return false;
    ctx.print("%s", v);
    return true;
}

bool clock_patch(CliContext& ctx, const char* rel, Span<const char* const> values) {
    Engine& e = *ctx.engine;
    if (values.len != 1) { ctx.fail("one value"); return false; }
    f32 v;
    if (!cli_arg_f32(ctx, values[0], rel, &v)) return false;
    if (!strcmp(rel, "paused")) { engine_pause(e, v != 0.0f); if (v == 0.0f) e.clock.step_ticks = 0; }
    else if (!strcmp(rel, "rate")) { if (!engine_rate(e, (f64)v)) { ctx.fail("rate must be positive; patch paused 1 instead"); return false; } }
    else if (!strcmp(rel, "step")) { engine_step(e, (u32)v); }
    else { ctx.fail("%s is read-only", rel[0] ? rel : "/clock"); return false; }
    return clock_describe(ctx, rel);
}

// ---- /modules -----------------------------------------------------------------------------------

void modules_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    Engine& e = *ctx.engine;
    for (const ModuleInstance& m : e.modules) {
        char summary[128];
        snprintf(summary, sizeof summary, "%s, arena %llu KB used, %llu KB peak", m.started ? "up" : "down",
                 (unsigned long long)(m.arena.used / 1024), (unsigned long long)(m.arena.peak / 1024));
        if (!emit(user, m.desc->name, summary)) return;
    }
}

bool modules_describe(CliContext& ctx, const char* rel) {
    Engine& e = *ctx.engine;
    for (const ModuleInstance& m : e.modules) {
        if (strcmp(m.desc->name, rel) != 0) continue;
        const ModuleDesc& d = *m.desc;
        ctx.print("%s: %s, index %u; %u events, %u components, %u kinds, %u resources, %u systems, %u handlers, %u triggers, %u commands, %u mounts",
                  d.name, m.started ? "up" : "down", m.index, d.events.len, d.components.len, d.kinds.len, d.resources.len, d.systems.len, d.handlers.len, d.triggers.len, d.commands.len, d.mounts.len);
        ctx.print("arena: %llu bytes used, %llu peak, %llu committed", (unsigned long long)m.arena.used, (unsigned long long)m.arena.peak, (unsigned long long)m.arena.committed);
        return true;
    }
    return false;
}

// ---- /mem ---------------------------------------------------------------------------------------

void mem_list(CliContext&, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    const MemSample& s = mem_last_sample();
    char v[64];
    snprintf(v, sizeof v, "%llu KB", (unsigned long long)(s.live_bytes / 1024));      if (!emit(user, "live", v)) return;
    snprintf(v, sizeof v, "%llu KB", (unsigned long long)(s.committed_bytes / 1024)); if (!emit(user, "committed", v)) return;
    snprintf(v, sizeof v, "%llu KB", (unsigned long long)(s.peak_live_bytes / 1024)); if (!emit(user, "peak", v)) return;
    snprintf(v, sizeof v, "%llu", (unsigned long long)s.live_allocs);                 if (!emit(user, "allocs", v)) return;
    snprintf(v, sizeof v, "%llu", (unsigned long long)s.frame);                       emit(user, "sampled_at_frame", v);
}

bool mem_describe(CliContext& ctx, const char* rel) {
    const MemSample& s = mem_last_sample();
    if (!rel[0]) { ctx.print("live %llu KB, committed %llu KB, peak %llu KB, %llu allocations, sampled at frame %llu", (unsigned long long)(s.live_bytes / 1024), (unsigned long long)(s.committed_bytes / 1024), (unsigned long long)(s.peak_live_bytes / 1024), (unsigned long long)s.live_allocs, (unsigned long long)s.frame); return true; }
    if (!strcmp(rel, "live")) { ctx.print("%llu", (unsigned long long)s.live_bytes); return true; }
    if (!strcmp(rel, "committed")) { ctx.print("%llu", (unsigned long long)s.committed_bytes); return true; }
    if (!strcmp(rel, "peak")) { ctx.print("%llu", (unsigned long long)s.peak_live_bytes); return true; }
    if (!strcmp(rel, "allocs")) { ctx.print("%llu", (unsigned long long)s.live_allocs); return true; }
    if (!strcmp(rel, "sampled_at_frame")) { ctx.print("%llu", (unsigned long long)s.frame); return true; }
    return false;
}

// ---- /perf --------------------------------------------------------------------------------------
// The profiler's zones since the last report, as leaves: average milliseconds per frame.

void perf_list(CliContext&, const char* dir, CliEmitFn emit, void* user) {
    if (dir[0]) return;
    if (!prof_enabled()) { emit(user, "(profiling off: --profile or --prof-report)", ""); return; }
    ProfStat top[64];
    u32 frames = 1;
    u32 n = prof_top(top, &frames);
    for (u32 i = 0; i < n; ++i) {
        char summary[96];
        snprintf(summary, sizeof summary, "%.3f ms avg, %.3f ms worst, %.1f calls/frame%s", prof_ms(top[i].wall) / frames, prof_ms(top[i].worst), (f64)top[i].calls / frames, top[i].wait ? ", wait" : "");
        if (!emit(user, top[i].name, summary)) return;
    }
}

bool perf_describe(CliContext& ctx, const char* rel) {
    if (!prof_enabled()) { ctx.print("profiling off: run with --profile FILE or --prof-report"); return true; }
    ProfStat top[64];
    u32 frames = 1;
    u32 n = prof_top(top, &frames);
    if (!rel[0]) { ctx.print("%u zones over %u frames", n, frames); return true; }
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(top[i].name, rel) != 0) continue;
        ctx.print("%s: %.3f ms avg per frame, %.3f ms worst call, %.1f calls per frame%s, over %u frames", top[i].name, prof_ms(top[i].wall) / frames, prof_ms(top[i].worst), (f64)top[i].calls / frames, top[i].wait ? ", a wait" : "", frames);
        return true;
    }
    return false;
}

} // namespace

const CliMountDesc k_engine_mounts[10] = {
    {.path = "/kinds",     .list = kinds_list,     .describe = kinds_describe, .patch = kinds_patch},
    {.path = "/systems",   .list = systems_list,   .describe = systems_describe},
    {.path = "/handlers",  .list = handlers_list,  .describe = handlers_describe},
    {.path = "/resources", .list = resources_list, .describe = resources_describe},
    {.path = "/events",    .list = events_list,    .describe = events_describe},
    {.path = "/triggers",  .list = triggers_list,  .describe = triggers_describe},
    {.path = "/clock",     .list = clock_list,     .describe = clock_describe, .patch = clock_patch},
    {.path = "/modules",   .list = modules_list,   .describe = modules_describe},
    {.path = "/mem",       .list = mem_list,       .describe = mem_describe},
    {.path = "/perf",      .list = perf_list,      .describe = perf_describe},
};

} // namespace kv
