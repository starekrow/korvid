#include "korvid/core/profile.h"
#include "korvid/core/sort.h"
#include "korvid/core/atomic.h"
#include "korvid/core/arena.h"
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_memory.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

bool g_prof_on = false;

namespace {

constexpr u64 kKindShift = 56;
constexpr u64 kTickMask  = ((u64)1 << kKindShift) - 1;
constexpr u32 kRingCap   = 1u << 16; // records per thread: 1.5 MB, a few seconds of a busy loop

inline ProfKind kind_of(const ProfRecord& r) { return (ProfKind)(r.ticks >> kKindShift); }
inline u64      ticks_of(const ProfRecord& r) { return r.ticks & kTickMask; }

// One per thread, claimed on first use. The thread writes; the collector reads up to the
// published write index and never stops the writer. A thread that exits with nothing
// unread frees its ring at once (most threads, when nothing is profiling: naming a thread
// claims its ring); one with records left marks it dead, and the collector drains it and
// frees the slot. `claims` tells the collector a slot changed hands.
struct Ring {
    ProfRecord* data;
    u64         write;   // published with release; slots are write % cap
    u64         read;    // the collector's position (stored with release, for an exiting thread's check)
    u64         dropped; // records the collector found overwritten
    u32         in_use;  // atomic
    u32         dead;    // atomic: the thread is gone
    u32         claims;  // times the slot was claimed; written under the lock
    u32         tid;
    char        name[32];
};

Ring g_rings[KV_PROF_MAX_RINGS];
u32  g_ring_lock = 0;
bool g_cpu_time  = false;
thread_local Ring* t_ring = nullptr;

void lock_rings()   { while (kv::atomic_exchange(&g_ring_lock, 1u, kv::MO_ACQUIRE)) {} }
void unlock_rings() { kv::atomic_store(&g_ring_lock, 0u, kv::MO_RELEASE); }

struct RingGuard {
    ~RingGuard() {
        Ring* r = t_ring;
        if (!r) return;
        t_ring = nullptr;
        lock_rings();
        if (kv::atomic_load(&r->read, kv::MO_ACQUIRE) == r->write) kv::atomic_store(&r->in_use, 0u, kv::MO_RELEASE);
        else kv::atomic_store(&r->dead, 1u, kv::MO_RELEASE);
        unlock_rings();
    }
};
thread_local RingGuard t_ring_guard;

Ring* ring() {
    if (t_ring) return t_ring;
    (void)t_ring_guard;
    lock_rings();
    Ring* r = nullptr;
    for (u32 i = 0; i < KV_PROF_MAX_RINGS; ++i) {
        if (kv::atomic_load(&g_rings[i].in_use, kv::MO_ACQUIRE)) continue;
        r = &g_rings[i];
        if (!r->data) {
            r->data = (ProfRecord*)os_reserve(kRingCap * sizeof(ProfRecord));
            if (r->data && !os_commit(r->data, os_page_align(kRingCap * sizeof(ProfRecord)))) { os_release(r->data, kRingCap * sizeof(ProfRecord)); r->data = nullptr; }
            if (!r->data) { r = nullptr; break; }
        }
        r->write = r->read = r->dropped = 0;
        r->dead = 0;
        ++r->claims;
        r->tid = thread_id();
        snprintf(r->name, sizeof r->name, "thread %u", r->tid);
        kv::atomic_store(&r->in_use, 1u, kv::MO_RELEASE);
        break;
    }
    unlock_rings();
    t_ring = r;
    return r;
}

inline void put(ProfKind kind, const char* name, u64 value) {
    Ring* r = ring();
    if (!r) return;
    ProfRecord& rec = r->data[r->write % kRingCap];
    rec.ticks = (os_ticks() & kTickMask) | ((u64)kind << kKindShift);
    rec.name  = name;
    rec.value = value;
    kv::atomic_store(&r->write, r->write + 1, kv::MO_RELEASE);
}

// ---- the collector ------------------------------------------------------------------------
struct Stat { ProfStat s; };

struct Open { u32 at; const char* name; u64 begin; u64 cpu; bool wait; };

struct Collector {
    bool            ready = false;
    Map<u64, u32>   frame_index;  // name address -> index in frame
    Array<ProfStat> frame;
    Map<u64, u32>   total_index;
    Array<ProfStat> totals;
    u32             frames_since_log = 0;
    Array<ProfThreadStat> threads;
    Array<Open>     stacks[KV_PROF_MAX_RINGS];
    bool            keep = false;
    Array<ProfRecord> kept;       // the last frame's records, with their ring in kept_ring
    Array<u8>         kept_ring;
    Array<ProfRecord> kept_next;
    Array<u8>         kept_next_ring;
    FILE*           trace = nullptr;
    bool            trace_named[KV_PROF_MAX_RINGS];
    u32             seen_claims[KV_PROF_MAX_RINGS]; // a ring's claims when last collected: changed, its state is another thread's
    u64             trace_origin = 0;
};
Collector g_c;

void collector_init() {
    if (g_c.ready) return;
    g_c.frame_index.init(mem_general());
    g_c.total_index.init(mem_general());
    g_c.ready = true;
}

ProfStat& stat_for(Map<u64, u32>& index, Array<ProfStat>& table, const char* name, bool wait) {
    bool inserted = false;
    u32* at = index.get_or_insert((u64)(usize)name, table.len, &inserted);
    if (inserted) { ProfStat s{}; s.name = name; s.wait = wait; table.push(s); }
    return table[*at];
}

f64 trace_us(u64 ticks) { return (f64)(ticks - g_c.trace_origin) * 1e6 / (f64)os_ticks_per_second(); }

void trace_record(u32 ri, const Ring& r, const ProfRecord& rec, const Open* open_for_end) {
    FILE* f = g_c.trace;
    if (!g_c.trace_named[ri]) {
        fprintf(f, "{\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"name\":\"thread_name\",\"args\":{\"name\":\"%s\"}},\n", ri + 1, r.name);
        g_c.trace_named[ri] = true;
    }
    u64 t = ticks_of(rec);
    switch (kind_of(rec)) {
    case ProfKind::ZoneBegin: fprintf(f, "{\"ph\":\"B\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"name\":\"%s\"},\n", ri + 1, trace_us(t), rec.name); break;
    case ProfKind::WaitBegin: fprintf(f, "{\"ph\":\"B\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"name\":\"%s\",\"cat\":\"wait\"},\n", ri + 1, trace_us(t), rec.name); break;
    case ProfKind::ZoneEnd:   fprintf(f, "{\"ph\":\"E\",\"pid\":1,\"tid\":%u,\"ts\":%.3f%s%s%s},\n", ri + 1, trace_us(t),
                                      open_for_end ? ",\"name\":\"" : "", open_for_end ? open_for_end->name : "", open_for_end ? "\"" : ""); break;
    case ProfKind::Mark:      fprintf(f, "{\"ph\":\"i\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"name\":\"%s\",\"s\":\"t\",\"args\":{\"value\":%llu}},\n", ri + 1, trace_us(t), rec.name, (unsigned long long)rec.value); break;
    case ProfKind::Counter:   fprintf(f, "{\"ph\":\"C\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,\"name\":\"%s\",\"args\":{\"value\":%llu}},\n", ri + 1, trace_us(t), rec.name, (unsigned long long)rec.value); break;
    case ProfKind::Frame:     fprintf(f, "{\"ph\":\"i\",\"pid\":1,\"tid\":0,\"ts\":%.3f,\"name\":\"frame %llu\",\"s\":\"g\"},\n", trace_us(t), (unsigned long long)rec.value); break;
    case ProfKind::Tick:      fprintf(f, "{\"ph\":\"i\",\"pid\":1,\"tid\":0,\"ts\":%.3f,\"name\":\"tick %llu\",\"s\":\"p\"},\n", trace_us(t), (unsigned long long)rec.value); break;
    }
}

void collect_ring(u32 ri, Ring& r) {
    u64 w = kv::atomic_load(&r.write, kv::MO_ACQUIRE);
    if (w - r.read > kRingCap) { r.dropped += w - r.read - kRingCap; r.read = w - kRingCap; }
    u64 from = r.read;
    Array<Open>& stack = g_c.stacks[ri];
    if (!stack.alloc) stack.alloc = mem_general();
    if (g_c.seen_claims[ri] != r.claims) { stack.clear(); g_c.trace_named[ri] = false; g_c.seen_claims[ri] = r.claims; }
    ProfThreadStat& ts = g_c.threads[ri];
    ts.name = r.name;
    for (u64 i = from; i < w; ++i) {
        ProfRecord rec = r.data[i % kRingCap];
        // A record the writer has lapped since we started: the rest of this batch is suspect.
        if (kv::atomic_load(&r.write, kv::MO_ACQUIRE) - i > kRingCap) { r.dropped += w - i; break; }
        ProfKind k = kind_of(rec);
        const Open* ending = nullptr;
        Open ended{};
        switch (k) {
        case ProfKind::ZoneBegin:
        case ProfKind::WaitBegin:
            stack.push(Open{(u32)stack.len, rec.name, ticks_of(rec), rec.value, k == ProfKind::WaitBegin});
            break;
        case ProfKind::ZoneEnd:
            if (stack.len) {
                ended = stack.pop();
                ending = &ended;
                u64 wall = ticks_of(rec) - ended.begin;
                u64 cpu  = rec.value && ended.cpu ? rec.value - ended.cpu : 0;
                ProfStat& s = stat_for(g_c.frame_index, g_c.frame, ended.name, ended.wait);
                s.calls += 1; s.wall += wall; s.cpu += cpu; if (wall > s.worst) s.worst = wall;
                ProfStat& t = stat_for(g_c.total_index, g_c.totals, ended.name, ended.wait);
                t.calls += 1; t.wall += wall; t.cpu += cpu; if (wall > t.worst) t.worst = wall;
                // Busy is the thread's top-level zones, so nesting does not double count;
                // waiting is every wait zone at any depth, and is part of busy when nested.
                if (stack.len == 0) ts.busy += wall;
                if (ended.wait) ts.waiting += wall;
            }
            break;
        default: break;
        }
        if (g_c.trace) trace_record(ri, r, rec, ending);
        if (g_c.keep) { g_c.kept_next.push(rec); g_c.kept_next_ring.push((u8)ri); }
    }
    kv::atomic_store(&r.read, w, kv::MO_RELEASE);
    ts.open = stack.len;
    ts.dropped = r.dropped;
}

void log_tree(const Array<ProfRecord>& recs, const Array<u8>& rings, u8 ri, u64 min_ticks) {
    // Rebuild the tree for one ring: a stack of (begin record index, depth).
    struct Frame { u32 at; u32 depth; };
    Array<Frame> stack = array_in<Frame>(mem_scratch());
    u32 depth = 0;
    for (u32 i = 0; i < recs.len; ++i) {
        if (rings[i] != ri) continue;
        const ProfRecord& rec = recs[i];
        ProfKind k = kind_of(rec);
        if (k == ProfKind::ZoneBegin || k == ProfKind::WaitBegin) { stack.push(Frame{i, depth++}); continue; }
        if (k == ProfKind::ZoneEnd) {
            if (!stack.len) continue;
            Frame f = stack.pop();
            --depth;
            const ProfRecord& b = recs[f.at];
            u64 wall = ticks_of(rec) - ticks_of(b);
            if (wall < min_ticks) continue;
            KV_INFO("prof", "  %*s%s%s %.3f ms", (int)(f.depth * 2), "", b.name, kind_of(b) == ProfKind::WaitBegin ? " (wait)" : "", prof_ms(wall));
            continue;
        }
        if (k == ProfKind::Mark && depth <= 2) KV_INFO("prof", "  %*s* %s %llu", (int)(depth * 2), "", rec.name, (unsigned long long)rec.value);
    }
    // Open zones are worth knowing about, except a worker's idle wait, which is normal.
    for (u32 i = 0; i < stack.len; ++i)
        if (kind_of(recs[stack[i].at]) != ProfKind::WaitBegin)
            KV_INFO("prof", "  %*s%s (still open)", (int)(stack[i].depth * 2), "", recs[stack[i].at].name);
}

int by_wall_desc(const void* a, const void* b) {
    u64 x = ((const ProfStat*)a)->wall, y = ((const ProfStat*)b)->wall;
    return x > y ? -1 : x < y ? 1 : 0;
}

} // namespace

// ---- hot path -------------------------------------------------------------------------------
void prof_enable(bool on) { g_prof_on = on; }
void prof_cpu_time(bool on) { g_cpu_time = on; }
void prof_thread_name(const char* name) {
    Ring* r = ring();
    if (r && name) { strncpy(r->name, name, sizeof r->name - 1); r->name[sizeof r->name - 1] = 0; }
}

void prof_begin(const char* name)      { put(ProfKind::ZoneBegin, name, g_cpu_time ? os_thread_cpu_ticks() : 0); }
void prof_wait_begin(const char* name) { put(ProfKind::WaitBegin, name, g_cpu_time ? os_thread_cpu_ticks() : 0); }
void prof_end()                        { put(ProfKind::ZoneEnd, nullptr, g_cpu_time ? os_thread_cpu_ticks() : 0); }
void prof_mark(const char* name, u64 value) { put(ProfKind::Mark, name, value); }
void prof_counter(const char* name, u64 value) { put(ProfKind::Counter, name, value); }
void prof_frame(u64 frame) { put(ProfKind::Frame, "frame", frame); }
void prof_tick(u64 tick)   { put(ProfKind::Tick, "tick", tick); }

// ---- cold path ------------------------------------------------------------------------------
void prof_collect() {
    collector_init();
    (void)ring(); // claimed here, never under the lock below (the lock does not nest)
    g_c.frame.clear();
    g_c.frame_index.clear();
    g_c.threads.resize_fill(KV_PROF_MAX_RINGS, ProfThreadStat{});
    for (u32 i = 0; i < KV_PROF_MAX_RINGS; ++i) { g_c.threads[i].busy = g_c.threads[i].waiting = 0; }
    if (g_c.keep) { g_c.kept_next.clear(); g_c.kept_next_ring.clear(); }
    for (u32 i = 0; i < KV_PROF_MAX_RINGS; ++i) {
        Ring& r = g_rings[i];
        if (!kv::atomic_load(&r.in_use, kv::MO_ACQUIRE)) continue;
        lock_rings(); // the claims and the death are read where an exiting or a new thread cannot change them
        collect_ring(i, r);
        if (kv::atomic_load(&r.dead, kv::MO_ACQUIRE)) {
            g_c.stacks[i].clear();
            kv::atomic_store(&r.in_use, 0u, kv::MO_RELEASE); // the slot, and its pages, serve the next thread
        }
        unlock_rings();
    }
    if (g_c.keep) {
        Array<ProfRecord> t = g_c.kept; g_c.kept = g_c.kept_next; g_c.kept_next = t;
        Array<u8> tr = g_c.kept_ring; g_c.kept_ring = g_c.kept_next_ring; g_c.kept_next_ring = tr;
    }
    if (g_c.frame.len > 1) kv::sort(g_c.frame.data, g_c.frame.len, sizeof(ProfStat), by_wall_desc);
    ++g_c.frames_since_log;
    if (g_c.trace) fflush(g_c.trace);
}

Span<const ProfStat> prof_frame_stats() { return g_c.frame.span(); }
Span<const ProfThreadStat> prof_thread_stats() { return g_c.threads.span(); }

void prof_log_summary(u32 top) {
    collector_init();
    u32 frames = g_c.frames_since_log ? g_c.frames_since_log : 1;
    if (g_c.totals.len > 1) kv::sort(g_c.totals.data, g_c.totals.len, sizeof(ProfStat), by_wall_desc);
    KV_INFO("prof", "over %u frames, per frame, summed over threads: %-10s %8s %8s %7s", frames, "zone", "avg ms", "worst", "calls");
    for (u32 i = 0; i < g_c.totals.len && i < top; ++i) {
        const ProfStat& s = g_c.totals[i];
        KV_INFO("prof", "%-44s %8.3f %8.3f %7.1f%s", s.name, prof_ms(s.wall) / frames, prof_ms(s.worst), (f64)s.calls / frames, s.wait ? " wait" : "");
    }
    for (u32 i = 0; i < g_c.threads.len; ++i) {
        const ProfThreadStat& t = g_c.threads[i];
        if (!t.name || (!t.busy && !t.waiting)) continue;
        if (t.dropped) KV_WARN("prof", "%s: %llu records dropped (ring overflow)", t.name, (unsigned long long)t.dropped);
    }
    g_c.totals.clear();
    g_c.total_index.clear();
    g_c.frames_since_log = 0;
}

u32 prof_top(Span<ProfStat> out, u32* frames) {
    collector_init();
    if (frames) *frames = g_c.frames_since_log ? g_c.frames_since_log : 1;
    if (g_c.totals.len > 1) kv::sort(g_c.totals.data, g_c.totals.len, sizeof(ProfStat), by_wall_desc);
    u32 n = 0;
    for (; n < g_c.totals.len && n < out.len; ++n) out[n] = g_c.totals[n];
    return n;
}

void prof_keep_frame(bool on) {
    collector_init();
    g_c.keep = on;
    if (on) {
        g_c.kept.alloc = g_c.kept_next.alloc = g_c.kept_ring.alloc = g_c.kept_next_ring.alloc = mem_general();
    }
}

void prof_log_frame(u64 min_ticks) {
    collector_init();
    if (!g_c.keep) { KV_WARN("prof", "no frame kept: call prof_keep_frame(true) first"); return; }
    MemScope scope;
    for (u32 ri = 0; ri < KV_PROF_MAX_RINGS; ++ri) {
        bool any = false;
        for (u32 i = 0; i < g_c.kept_ring.len && !any; ++i) any = g_c.kept_ring[i] == ri;
        if (!any) continue;
        // A thread with nothing but an open idle wait has nothing to say.
        bool worth_it = false;
        for (u32 i = 0; i < g_c.kept_ring.len && !worth_it; ++i)
            worth_it = g_c.kept_ring[i] == ri && kind_of(g_c.kept[i]) != ProfKind::WaitBegin && kind_of(g_c.kept[i]) != ProfKind::ZoneEnd;
        if (!worth_it) continue;
        KV_INFO("prof", "%s:", g_rings[ri].name);
        log_tree(g_c.kept, g_c.kept_ring, (u8)ri, min_ticks);
    }
}

bool prof_trace_open(const char* path) {
    collector_init();
    prof_trace_close();
    g_c.trace = fopen(path, "w");
    if (!g_c.trace) { KV_ERROR("prof", "cannot write trace '%s'", path); return false; }
    fprintf(g_c.trace, "[\n");
    g_c.trace_origin = os_ticks();
    memset(g_c.trace_named, 0, sizeof g_c.trace_named);
    KV_INFO("prof", "tracing to '%s'", path);
    return true;
}

void prof_trace_close() {
    if (!g_c.trace) return;
    fprintf(g_c.trace, "{\"ph\":\"M\",\"pid\":1,\"tid\":0,\"name\":\"thread_name\",\"args\":{\"name\":\"frames and ticks\"}}\n]\n");
    fclose(g_c.trace);
    g_c.trace = nullptr;
}

f64 prof_ms(u64 ticks) { return (f64)ticks * 1e3 / (f64)os_ticks_per_second(); }
u64 prof_ticks_from_ms(f64 ms) { return (u64)(ms * 1e-3 * (f64)os_ticks_per_second()); }

void prof_shutdown() {
    prof_trace_close();
    if (g_c.ready) {
        g_c.frame.free(); g_c.frame_index.free(); g_c.totals.free(); g_c.total_index.free(); g_c.threads.free();
        for (Array<Open>& s : g_c.stacks) s.free();
        g_c.kept.free(); g_c.kept_next.free(); g_c.kept_ring.free(); g_c.kept_next_ring.free();
        g_c.ready = false;
        g_c.keep = false;
    }
    for (Ring& r : g_rings) {
        if (r.data) os_release(r.data, kRingCap * sizeof(ProfRecord));
        r = Ring{};
    }
    g_prof_on = false;
}

} // namespace kv
