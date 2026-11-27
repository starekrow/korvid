// cpukiller: a stress tool. No window. It measures the job pool's dispatch
// overhead, wake latency and contention, nested batches, the memory system under
// cross-thread load, and the engine's tick rate on one thread against many, with the state
// hash as the referee. Numbers go to stdout as a table; every scenario also checks that
// the memory accounting comes back to where it started.
//
//   cpukiller [--threads N] [--bodies N] [--ticks N] [--fighters N] [--reps N] [--spin US] [--only SCENARIO]
//             [--quick] [--sweep] [--md] [--window] [--profile FILE]
//
// --spin sets the pool's idle spin in microseconds (jobs_set_idle_spin; 0 for none), to see
// what it buys. --only runs one scenario: dispatch, wake, contention, nested, memory, engine,
// brawl.
//
// A number here is a median of repeats, never one run: each case is run --reps times with
// its variants alternated (serial, pool, reference; serial, pool, reference), so a clock
// that moves or a core that warms moves all three, and the table carries the spread
// between the fastest and slowest repeat beside the median. Latencies are given as p50,
// p99 and the worst seen.
//
// Every case has a reference beside it (floor.h): for the pool, the same batches on a
// pool that only spins and claims, the least a pool can do; for the allocator, the C
// runtime's malloc on the same threads; for the engine, the same four systems over bare
// arrays. "serial" is a plain loop on the calling thread, no pool at all.
//
// --sweep runs the cases that scale at 1, 2, 4... threads up to --threads instead, for
// the curve. --md prints the tables as Markdown (tools/concurrency-numbers.sh wraps a run
// in the machine it ran on).
//
// Every scenario is a zone and every case a mark, so --profile FILE gives a trace of the
// whole run; the engine scenario's breakdown (what the ticks spent where, and how much of
// the workers' time was waiting) comes from the profiler's collector, read after every
// frame so that no ring overflows, not from stopwatches.
//
// With --window the usual stack comes up first (platform, renderer, frame loop), the
// pool and memory scenarios run while the window and swapchain are alive, and the engine
// ticks are driven through the frame loop with a render after every batch of ticks:
// memory can behave differently with a GPU's fingers in it.
#include "korvid/cpukiller/module.h"
#include "brawl.h"
#include "floor.h"
#include "korvid/core/format.h"
#include "korvid/core/atomic.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/core/arena.h"
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/job.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/profile.h"
#include "korvid/core/sort.h"
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

u32  g_threads = 0;
u32  g_bodies  = 200000;
u32  g_ticks   = 300;
u32  g_fighters = 100000;
u32  g_reps    = 0;   // 0: each scenario's own default
bool g_quick   = false;
bool g_window  = false;
bool g_sweep   = false;
bool g_md      = false;
const char* g_trace = nullptr;
constexpr u32 kWindowTicksPerFrame = 16; // ticks between renders in window mode; headless, a tick a frame

const char* g_only = nullptr; // --only: one scenario by name
bool wanted(const char* scenario) { return !g_only || strcmp(g_only, scenario) == 0; }

u32 reps(u32 usual) { return g_reps ? g_reps : g_quick ? (usual > 3 ? 3 : usual) : usual; }

// ---- samples ---------------------------------------------------------------------------
// Milliseconds, kept whole so the tail can be read off them.
struct Samples {
    static constexpr u32 kCap = 8192;
    f64 v[kCap];
    u32 n = 0;
    void add(f64 ms) { if (n < kCap) v[n++] = ms; }
};
struct Dist { f64 min = 0, p50 = 0, p99 = 0, max = 0; u32 n = 0; };

Dist dist_of(Samples& s) {
    Dist d;
    if (!s.n) return d;
    kv::sort(s.v, s.n, sizeof(f64), [](const void* a, const void* b) { f64 x = *(const f64*)a, y = *(const f64*)b; return x < y ? -1 : x > y ? 1 : 0; });
    d.n = s.n;
    d.min = s.v[0];
    d.p50 = s.v[s.n / 2];
    d.p99 = s.v[(u64)s.n * 99 / 100 < s.n ? (u64)s.n * 99 / 100 : s.n - 1];
    d.max = s.v[s.n - 1];
    return d;
}

f64 ms_since(f64 t0) { return (time_now() - t0) * 1e3; }

// ---- three ways to run a case -----------------------------------------------------------
// A plain loop over the chunks: what "serial" means everywhere below.
void serial_run(u32 count, u32 grain, JobFn fn, void* user) {
    if (!grain) return;
    u32 chunks = count / grain + (count % grain != 0);
    for (u32 c = 0; c < chunks; ++c) fn(c, c * grain, (c + 1) * grain < count ? (c + 1) * grain : count, user);
}
void nothing(u32, u32, u32, void*) {}

enum { kSerial, kPool, kRef, kWays };
struct Tri { Dist d[kWays]; bool has_ref = false; };

// Times f(way) `inner` times a way, the ways alternated `rounds` times over; `before`,
// when given, runs ahead of each and is not timed. With
// `floor_ref` the reference runs on the floor pool, which is up only while it is timed:
// its threads spin, and would take the cores from the other two.
// The pool's threads spin for a while after their last batch (jobs_idle_spin); what is
// timed next waits until they are asleep, or it would be timed beside them.
void pool_quiet() { if (jobs_worker_count() && jobs_idle_spin() > 0.0) time_sleep(jobs_idle_spin() * 2 + 0.001); }

template <class F>
Tri measure(u32 rounds, u32 inner, bool has_ref, bool floor_ref, F f, void (*before)(u32 way) = nullptr) {
    static Samples s[kWays];
    for (Samples& x : s) x.n = 0;
    for (u32 r = 0; r < rounds; ++r) {
        for (u32 way = 0; way < kWays; ++way) {
            if (way == kRef && !has_ref) continue;
            pool_quiet();
            if (way == kRef && floor_ref) { floor_init(g_threads - 1); floor_run(g_threads * 8, 1, nothing, nullptr); }
            for (u32 i = 0; i < inner; ++i) {
                if (before) before(way);
                f64 t0 = time_now();
                f(way);
                s[way].add(ms_since(t0));
            }
            if (way == kRef && floor_ref) floor_shutdown();
        }
    }
    Tri t;
    t.has_ref = has_ref;
    for (u32 way = 0; way < kWays; ++way) t.d[way] = dist_of(s[way]);
    return t;
}

// A counter over every pool run of a case, as one run's share.
u64 per_run(u64 total, const Tri& t) { return t.d[kPool].n ? total / t.d[kPool].n : 0; }

// ---- the report --------------------------------------------------------------------------
struct Row { char name[20]; char detail[48]; bool us; bool has_serial, has_ref; f64 serial, par, ref, spread; char note[200]; };
Row g_rows[48];
u32 g_row_count = 0;

// A row from a case's medians; `us` prints it in microseconds. The spread is the pool's
// slowest repeat less its fastest, as a share of its median.
void report(const char* name, const char* detail, const Tri& t, bool us, const char* note) {
    if (g_row_count >= KV_COUNTOF(g_rows)) return;
    Row& r = g_rows[g_row_count++];
    format(r.name, sizeof r.name, "%s", name);
    format(r.detail, sizeof r.detail, "%s", detail);
    r.us = us; r.has_serial = t.d[kSerial].n != 0; r.has_ref = t.has_ref;
    r.serial = t.d[kSerial].p50; r.par = t.d[kPool].p50; r.ref = t.d[kRef].p50;
    r.spread = t.d[kPool].p50 > 0 ? 100.0 * (t.d[kPool].max - t.d[kPool].min) / t.d[kPool].p50 : 0.0;
    format(r.note, sizeof r.note, "%s", note);
}

// One table, fixed-width or Markdown. `cells` is rows * cols strings.
void print_table(const char* const* heads, u32 cols, const char* cells, u32 cell_size, u32 rows, const u32* widths) {
    if (g_md) {
        for (u32 c = 0; c < cols; ++c) print("| %s ", heads[c]);
        print("|\n");
        for (u32 c = 0; c < cols; ++c) print("| --- ");
        print("|\n");
    } else {
        for (u32 c = 0; c < cols; ++c) print("%-*s ", (int)widths[c], heads[c]);
        print("\n");
    }
    for (u32 r = 0; r < rows; ++r) {
        for (u32 c = 0; c < cols; ++c) {
            const char* cell = cells + ((usize)r * cols + c) * cell_size;
            if (g_md) print("| %s ", cell); else print("%-*s ", (int)widths[c], cell);
        }
        print(g_md ? "|\n" : "\n");
    }
}

void print_rows() {
    constexpr u32 kCols = 7, kCell = 200;
    static char cells[KV_COUNTOF(g_rows) * kCols][kCell];
    const char* heads[kCols] = {"scenario", "what", "serial", "pool", "reference", "spread", "notes"};
    const u32 widths[kCols] = {16, 40, 11, 11, 11, 7, 0};
    for (u32 i = 0; i < g_row_count; ++i) {
        const Row& r = g_rows[i];
        f64 k = r.us ? 1e3 : 1.0;
        const char* unit = r.us ? "us" : "ms";
        char (*c)[kCell] = &cells[i * kCols];
        format(c[0], kCell, "%s", r.name);
        format(c[1], kCell, "%s", r.detail);
        if (r.has_serial) format(c[2], kCell, "%.2f %s", r.serial * k, unit); else format(c[2], kCell, "-");
        format(c[3], kCell, "%.2f %s", r.par * k, unit);
        if (r.has_ref) format(c[4], kCell, "%.2f %s", r.ref * k, unit); else format(c[4], kCell, "-");
        if (r.us) format(c[5], kCell, "-"); else format(c[5], kCell, "%.0f%%", r.spread); // a latency row has its tail in its notes
        format(c[6], kCell, "%s", r.note);
    }
    print("\n");
    print_table(heads, kCols, cells[0], kCell, g_row_count, widths);
}

// Accounting must come back to where it was, or something leaked across threads.
MemStats g_base;
void mem_checkpoint() { g_base = mem_stats(); }
bool mem_settled(const char* what) {
    MemStats s = mem_stats();
    if (s.live_allocs != g_base.live_allocs || s.live_bytes != g_base.live_bytes) {
        KV_ERROR("cpukiller", "%s: %llu allocations, %llu bytes still live", what,
                 (unsigned long long)(s.live_allocs - g_base.live_allocs), (unsigned long long)(s.live_bytes - g_base.live_bytes));
        return false;
    }
    return true;
}

// ---- 1. dispatch overhead: chunks that do nothing ------------------------------------
void scenario_overhead() {
    KV_ZONE("dispatch");
    const u32 counts[] = {2, 16, 256, 4096, 65536};
    for (u32 chunks : counts) {
        KV_MARK_V("dispatch case", chunks);
        u32 inner = chunks >= 65536 ? 8 : 40;
        jobs_stats_reset();
        Tri t = measure(reps(5), inner, true, true, [&](u32 way) {
            if (way == kSerial) serial_run(chunks, 1, nothing, nullptr);
            else if (way == kPool) jobs_run(chunks, 1, nothing, nullptr);
            else floor_run(chunks, 1, nothing, nullptr);
        });
        JobStats st = jobs_stats();
        char detail[64], note[200];
        format(detail, sizeof detail, "%u empty chunks, a batch", chunks);
        format(note, sizeof note, "pool %.0f ns/chunk, p99 %.1f us, worst %.1f us; reference %.0f ns/chunk; caller ran %llu%%",
               t.d[kPool].p50 * 1e6 / chunks, t.d[kPool].p99 * 1e3, t.d[kPool].max * 1e3, t.d[kRef].p50 * 1e6 / chunks,
               (unsigned long long)(st.tasks ? 100 * st.tasks_by_caller / st.tasks : 0));
        report("dispatch", detail, t, true, note);
    }
}

// ---- 2. wake latency: a small batch from a pool that is asleep, and one that is not ---
// One chunk a thread, each a few tens of microseconds: the batch a system with a little
// work makes. Ideal is one chunk's time; the rest is waking the pool.
struct Spin { u32 iters; u64* sink; };
void spin(u32, u32 begin, u32 end, void* user) {
    Spin& s = *(Spin*)user;
    u64 acc = 0;
    for (u32 i = begin; i < end; ++i)
        for (u32 k = 0; k < s.iters; ++k) acc = hash_mix(acc ^ (i + k));
    kv::atomic_add_fetch(s.sink, acc, kv::MO_RELAXED);
}

// Long enough for every worker to have gone back to its condition variable.
void let_pool_sleep(u32 way) { if (way == kPool) time_sleep(jobs_idle_spin() * 2 + 0.002); }

void scenario_wake() {
    KV_ZONE("wake");
    u64 sink = 0;
    Spin sp{20000, &sink};
    for (u32 cold = 0; cold < 2; ++cold) {
        KV_MARK_V("wake case", cold);
        // Cold: every batch finds the workers parked, by a sleep before it that is not timed.
        // Only the pool's turn needs it: the floor never sleeps, and serial has nobody to wake.
        Tri t = measure(reps(5), cold ? 12 : 60, true, true, [&](u32 way) {
            if (way == kSerial) serial_run(g_threads, 1, spin, &sp);
            else if (way == kPool) jobs_run(g_threads, 1, spin, &sp);
            else floor_run(g_threads, 1, spin, &sp);
        }, cold ? let_pool_sleep : nullptr);
        f64 ideal = t.d[kSerial].p50 / g_threads;
        char detail[64], note[200];
        format(detail, sizeof detail, "%u chunks of %.0f us, pool %s", g_threads, ideal * 1e3, cold ? "asleep" : "awake");
        format(note, sizeof note, "ideal %.0f us; pool p50 %.0f, p99 %.0f, worst %.0f us", ideal * 1e3, t.d[kPool].p50 * 1e3,
               t.d[kPool].p99 * 1e3, t.d[kPool].max * 1e3);
        report("wake", detail, t, true, note);
    }
}

// ---- 3. contention: fixed work, chunk sizes from tiny to large ----------------------
u32 contention_items() { return g_quick ? 1u << 16 : 1u << 18; } // items of 200 hash rounds each

Tri contention_case(u32 grain, u32 rounds) {
    u64 sink = 0;
    Spin sp{200, &sink};
    u32 items = contention_items();
    return measure(rounds, 1, true, true, [&](u32 way) {
        if (way == kSerial) serial_run(items, grain, spin, &sp);
        else if (way == kPool) jobs_run(items, grain, spin, &sp);
        else floor_run(items, grain, spin, &sp);
    });
}

void scenario_contention() {
    KV_ZONE("contention");
    const u32 grains[] = {1, 8, 64, 512, 4096};
    for (u32 grain : grains) {
        KV_MARK_V("contention case", grain);
        jobs_stats_reset();
        Tri t = contention_case(grain, reps(7));
        JobStats st = jobs_stats();
        f64 ser = t.d[kSerial].p50;
        u32 chunks = jobs_chunks(contention_items(), grain);
        char detail[64], note[200];
        format(detail, sizeof detail, "grain %u: %u chunks of %.1f us", grain, chunks, chunks ? ser * 1e3 / chunks : 0.0);
        format(note, sizeof note, "pool %.1fx of %u (best %.1fx), reference %.1fx; %llu worker waits a run", ser / t.d[kPool].p50, g_threads,
               ser / t.d[kPool].min, ser / t.d[kRef].p50, (unsigned long long)per_run(st.worker_waits, t));
        report("contention", detail, t, false, note);
    }
}

// ---- 4. nested batches: every outer chunk runs an inner batch -------------------------
struct Nest { u32 inner; u32 iters; u64* sink; bool pool; };
void nested(u32, u32 begin, u32 end, void* user) {
    Nest& n = *(Nest*)user;
    for (u32 i = begin; i < end; ++i) {
        Spin sp{n.iters, n.sink};
        if (n.pool) jobs_run(n.inner, 1, spin, &sp); else serial_run(n.inner, 1, spin, &sp);
    }
}

void scenario_nested() {
    KV_ZONE("nested");
    u64 sink = 0;
    u32 outer = g_quick ? 16 : 64;
    jobs_stats_reset();
    Tri t = measure(reps(7), 1, false, false, [&](u32 way) {
        Nest n{64, 200, &sink, way == kPool};
        if (way == kSerial) serial_run(outer, 1, nested, &n); else jobs_run(outer, 1, nested, &n);
    });
    JobStats st = jobs_stats();
    char detail[64], note[200];
    format(detail, sizeof detail, "%u outer x %u inner chunks", outer, 64u);
    format(note, sizeof note, "pool %.1fx; %llu batches and %llu helper waits a run", t.d[kSerial].p50 / t.d[kPool].p50,
           (unsigned long long)per_run(st.batches, t), (unsigned long long)per_run(st.helper_waits, t));
    report("nested", detail, t, false, note);
}

// ---- 5. memory under load ------------------------------------------------------------
// The engine's allocator or the C runtime's, by a flag, so the reference does the same work.
inline void* bench_alloc(bool crt, usize size) { return crt ? malloc(size) : mem_alloc(size); }
inline void  bench_free(bool crt, void* p) { if (crt) free(p); else mem_free(p); }

// (a) alloc and free on the same thread, mixed sizes, 64 blocks live a chunk. `fill`
// writes every byte, which makes it a test of the memory bus as much as of the allocator;
// without it only the first byte is touched.
struct Churn { u32 per_chunk; u32 size_span; bool fill; bool crt; };
void churn_same(u32 chunk, u32 begin, u32 end, void* user) {
    Churn& c = *(Churn*)user;
    void* keep[64];
    u32 kept = 0;
    u64 r = chunk + 1;
    for (u32 i = begin; i < end; ++i)
        for (u32 k = 0; k < c.per_chunk; ++k) {
            r = hash_mix(r);
            usize size = 16 + (r % c.size_span);
            u8* p = (u8*)bench_alloc(c.crt, size);
            if (c.fill) memset(p, (int)r, size); else p[0] = (u8)r;
            if (kept < 64) keep[kept++] = p;
            else { bench_free(c.crt, keep[r % 64]); keep[r % 64] = p; }
        }
    for (u32 k = 0; k < kept; ++k) bench_free(c.crt, keep[k]);
}

// (b) remote frees: batch A fills a slot table, batch B frees each chunk's neighbor's slots
struct Remote { void** slots; u32 per_chunk; u32 chunks; bool crt; };
void remote_alloc(u32 chunk, u32, u32, void* user) {
    Remote& r = *(Remote*)user;
    u64 s = chunk * 31 + 7;
    for (u32 k = 0; k < r.per_chunk; ++k) { s = hash_mix(s); r.slots[chunk * r.per_chunk + k] = bench_alloc(r.crt, 16 + (s % 1024)); }
}
void remote_free(u32 chunk, u32, u32, void* user) {
    Remote& r = *(Remote*)user;
    u32 victim = (chunk + 1) % r.chunks;
    for (u32 k = 0; k < r.per_chunk; ++k) { bench_free(r.crt, r.slots[victim * r.per_chunk + k]); r.slots[victim * r.per_chunk + k] = nullptr; }
}

// (c) scratch: every chunk pushes into its thread's scratch inside a scope
void scratch_churn(u32, u32 begin, u32 end, void*) {
    for (u32 i = begin; i < end; ++i) {
        MemScope scope;
        u8* total = nullptr;
        for (u32 k = 0; k < 256; ++k) { u8* p = (u8*)alloc_in(mem_scratch(), 64 + (k % 7) * 16); p[0] = (u8)k; total = p; }
        KV_UNUSED(total);
    }
}

// (d) arrays: grow, use, free, on the general allocator
void array_churn(u32 chunk, u32 begin, u32 end, void*) {
    for (u32 i = begin; i < end; ++i) {
        Array<u32> a;
        for (u32 k = 0; k < 2000; ++k) a.push(k * chunk);
        u32 s = 0;
        for (u32 x : a) s += x;
        a.free();
        KV_UNUSED(s);
    }
}

// The baseline is taken with the pool up and warm (its thread array is its one allocation),
// and the case must come back to it.
void pool_ready() {
    jobs_run(4096, 1, nothing, nullptr);
    mem_checkpoint();
}

u32 mem_chunks() { return g_quick ? 64 : 256; }

// Same-thread churn: the engine's allocator serial and on the pool, the C runtime's on the
// pool as the reference.
Tri churn_case(u32 per_chunk, u32 size_span, bool fill, u32 rounds, bool* settled) {
    pool_ready();
    Tri t = measure(rounds, 1, true, false, [&](u32 way) {
        Churn c{per_chunk, size_span, fill, way == kRef};
        if (way == kSerial) serial_run(mem_chunks(), 1, churn_same, &c); else jobs_run(mem_chunks(), 1, churn_same, &c, "mem churn");
    });
    *settled = mem_settled("mem churn");
    return t;
}

Tri remote_case(u32 per, u32 rounds, bool* settled) {
    u32 chunks = mem_chunks();
    void** slots = alloc_items<void*>(mem_general(), chunks * per);
    pool_ready();
    Tri t = measure(rounds, 1, true, false, [&](u32 way) {
        Remote r{slots, per, chunks, way == kRef};
        if (way == kSerial) { serial_run(chunks, 1, remote_alloc, &r); serial_run(chunks, 1, remote_free, &r); }
        else { jobs_run(chunks, 1, remote_alloc, &r, "mem remote alloc"); jobs_run(chunks, 1, remote_free, &r, "mem remote free"); }
    });
    *settled = mem_settled("mem remote-free");
    free_items(mem_general(), slots, chunks * per);
    return t;
}

void report_memory(const char* name, const char* detail, const Tri& t, u64 ops, const char* ref_name, bool settled) {
    char note[200];
    if (t.has_ref)
        format(note, sizeof note, "pool %.1fx of %u, %.1f M/s; %s on the same threads %.1f M/s%s", t.d[kSerial].p50 / t.d[kPool].p50, g_threads,
               (f64)ops / t.d[kPool].p50 / 1e3, ref_name, (f64)ops / t.d[kRef].p50 / 1e3, settled ? "" : ", LEAK");
    else
        format(note, sizeof note, "pool %.1fx of %u, %.1f M/s%s", t.d[kSerial].p50 / t.d[kPool].p50, g_threads, (f64)ops / t.d[kPool].p50 / 1e3, settled ? "" : ", LEAK");
    report(name, detail, t, false, note);
}

void scenario_memory() {
    KV_ZONE("memory");
    u32 chunks = mem_chunks();
    char detail[64];
    bool ok = true;

    KV_MARK("mem small");
    u32 small = g_quick ? 2000 : 8000;
    Tri t = churn_case(small, 256, false, reps(7), &ok);
    format(detail, sizeof detail, "%llu allocs of 16-271 B, first byte", (unsigned long long)chunks * small);
    report_memory("mem small", detail, t, (u64)chunks * small, "malloc", ok);

    KV_MARK("mem same-thread");
    u32 per = g_quick ? 500 : 2000;
    t = churn_case(per, 2048, true, reps(7), &ok);
    format(detail, sizeof detail, "%llu allocs of 16-2063 B, filled", (unsigned long long)chunks * per);
    report_memory("mem same-thread", detail, t, (u64)chunks * per, "malloc", ok);

    KV_MARK("mem remote-free");
    u32 remote = g_quick ? 2000 : 8000;
    t = remote_case(remote, reps(5), &ok);
    format(detail, sizeof detail, "%llu allocs freed by another chunk", (unsigned long long)chunks * remote);
    report_memory("mem remote-free", detail, t, (u64)chunks * remote, "malloc", ok);

    KV_MARK("mem scratch");
    u32 scopes = g_quick ? 4000 : 20000;
    pool_ready();
    t = measure(reps(7), 1, false, false, [&](u32 way) {
        if (way == kSerial) serial_run(scopes, 16, scratch_churn, nullptr); else jobs_run(scopes, 16, scratch_churn, nullptr, "mem scratch");
    });
    ok = mem_settled("mem scratch");
    format(detail, sizeof detail, "%u scopes of 256 pushes", scopes);
    report_memory("mem scratch", detail, t, scopes, "", ok);

    KV_MARK("mem arrays");
    u32 arrays = g_quick ? 1000 : 4000;
    pool_ready();
    t = measure(reps(7), 1, false, false, [&](u32 way) {
        if (way == kSerial) serial_run(arrays, 4, array_churn, nullptr); else jobs_run(arrays, 4, array_churn, nullptr, "mem arrays");
    });
    ok = mem_settled("mem arrays");
    format(detail, sizeof detail, "%u arrays of 2000", arrays);
    report_memory("mem arrays", detail, t, arrays, "", ok);
}

// ---- 6. the engine: ticks per second, one thread against many, hashes equal ----------
constexpr u32 kZones = 48;
struct ZoneSum { const char* name; f64 ms; };
struct EngineRun {
    f64 ms = 0; u64 hash = 0; u32 live = 0; u32 ticks = 0;
    ZoneSum zones[kZones] = {}; u32 zone_count = 0;
    f64 busy_ms = 0, wait_ms = 0; u64 dropped = 0, dropped_before = 0;
};

// What the collector saw of one frame, added to the run: the zones (work, not waits or
// containers) and the workers' busy and waiting time.
void breakdown_add(EngineRun& r) {
    prof_collect();
    for (const ProfStat& s : prof_frame_stats()) {
        if (s.wait || strcmp(s.name, "ticks") == 0 || strcmp(s.name, "tick") == 0 || strcmp(s.name, "level") == 0) continue;
        u32 z = 0;
        while (z < r.zone_count && strcmp(r.zones[z].name, s.name) != 0) ++z;
        if (z == r.zone_count) { if (z == kZones) continue; r.zones[r.zone_count++] = ZoneSum{s.name, 0.0}; }
        r.zones[z].ms += prof_ms(s.wall);
    }
    u64 dropped = 0;
    for (const ProfThreadStat& t : prof_thread_stats()) {
        if (!t.name) continue;
        dropped += t.dropped;
        if (strncmp(t.name, "worker", 6) != 0) continue;
        r.busy_ms += prof_ms(t.busy); r.wait_ms += prof_ms(t.waiting);
    }
    r.dropped = dropped; // a ring counts from when it was claimed: run_engine takes off what was there before the run
}

void pool_and_memory_scenarios() {
    jobs_init(g_threads - 1);
    if (wanted("dispatch")) scenario_overhead();
    if (wanted("wake")) scenario_wake();
    if (wanted("contention")) scenario_contention();
    if (wanted("nested")) scenario_nested();
    if (wanted("memory")) scenario_memory();
}

// One engine, `ticks` ticks, each frame timed by itself: the run's time is their sum, and
// `per_tick`, when given, gets every tick's. With the window, the pool and memory scenarios
// can run inside the first engine's life, GPU up, when `scenarios_inside` says so; the pool
// is then restored to the engine's count.
EngineRun run_engine(u32 threads, u32 ticks, bool scenarios_inside, Samples* per_tick) {
    EngineRun r{};
    Engine e;
    e.threads = threads;
    u32 per_frame = g_window ? kWindowTicksPerFrame : 1;
    e.clock.max_ticks_per_frame = per_frame;
    cpukiller_set_bodies(g_bodies);
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn); // the schedule log, once, is enough
    bool ok = engine_init(e, g_window ? kv_module_list() : cpukiller_core_modules());
    if (!ok) { log_set_level(was); KV_ERROR("cpukiller", "engine_init failed"); return r; }
    if (scenarios_inside) {
        log_set_level(was);
        if (g_window) frameloop_step(e, 0.0); // show the window, present a frame
        pool_and_memory_scenarios();
        jobs_init(threads ? threads - 1 : 0);
        if (g_window) frameloop_step(e, 0.0);
        log_set_level(LogLevel::Warn);
    }
    f64 dt = per_frame / 60.0;
    prof_collect(); // everything before the ticks is out of the way
    for (const ProfThreadStat& t : prof_thread_stats()) if (t.name) r.dropped_before += t.dropped;
    KV_MARK_V("engine run", threads);
    while (r.ticks < ticks) {
        u64 before = e.clock.tick_total;
        f64 t0 = time_now();
        if (g_window) frameloop_step(e, dt);
        else engine_frame(e, dt);
        f64 ms = ms_since(t0);
        u32 ran = (u32)(e.clock.tick_total - before);
        r.ms += ms;
        r.ticks += ran;
        if (per_tick && ran) per_tick->add(ms / ran);
        breakdown_add(r);
    }
    r.dropped = r.dropped > r.dropped_before ? r.dropped - r.dropped_before : 0;
    kv::sort(r.zones, r.zone_count, sizeof(ZoneSum), [](const void* a, const void* b) { f64 x = ((const ZoneSum*)a)->ms, y = ((const ZoneSum*)b)->ms; return x > y ? -1 : x < y ? 1 : 0; });
    r.hash = engine_hash_state(e);
    r.live = engine_ktable(e, "cpukiller.body")->live();
    engine_shutdown(e); // its log lines are held too: one pair a run would bury the table
    log_set_level(was);
    return r;
}

// The same four systems over bare arrays (floor.h), every tick timed: serial, or on the
// floor pool at g_threads.
Dist run_plain(u32 ticks, bool parallel) {
    static Samples s;
    s.n = 0;
    PlainSim* sim = plain_create(g_bodies);
    if (!sim) { KV_ERROR("cpukiller", "no memory for the plain sim"); return Dist{}; }
    if (parallel) { floor_init(g_threads - 1); floor_run(g_threads * 8, 1, nothing, nullptr); }
    for (u32 t = 0; t < ticks; ++t) {
        f64 t0 = time_now();
        plain_tick(sim, parallel);
        s.add(ms_since(t0));
    }
    if (parallel) floor_shutdown();
    plain_destroy(sim);
    return dist_of(s);
}

void scenario_engine() {
    u32 ticks = g_quick ? 64 : g_ticks;
    if (g_window) ticks = (ticks + kWindowTicksPerFrame - 1) / kWindowTicksPerFrame * kWindowTicksPerFrame;
    static Samples tick_one, tick_many, run_one, run_many;
    EngineRun one{}, many{};
    bool equal = true;
    u64 hash = 0;
    u32 rounds = reps(3);
    for (u32 r = 0; r < rounds; ++r) { // in window mode the other scenarios run inside the first engine
        many = run_engine(g_threads, ticks, g_window && r == 0, &tick_many);
        one  = run_engine(1, ticks, false, &tick_one);
        run_many.add(many.ms); run_one.add(one.ms);
        if (r == 0) hash = one.hash;
        if (one.hash != hash || many.hash != hash) {
            equal = false;
            KV_ERROR("cpukiller", "state hash differs: 1 thread %016llx, %u threads %016llx, first run %016llx",
                     (unsigned long long)one.hash, g_threads, (unsigned long long)many.hash, (unsigned long long)hash);
        }
    }
    Tri t;
    t.d[kSerial] = dist_of(run_one); t.d[kPool] = dist_of(run_many);
    Dist d1 = dist_of(tick_one), dn = dist_of(tick_many);
    Dist plain1 = run_plain(ticks, false), plainn = run_plain(ticks, true);
    t.has_ref = true;
    t.d[kRef] = plain1;
    t.d[kRef].p50 = plain1.p50 * ticks; // the reference column is a run of the plain sim on one thread
    f64 ser = t.d[kSerial].p50, par = t.d[kPool].p50;
    char detail[64], note[200];
    format(detail, sizeof detail, "%u bodies, %u ticks%s", g_bodies, ticks, g_window ? ", window" : "");
    format(note, sizeof note, "%.0f -> %.0f ticks/s, %.1fx of %u, hashes %s over %u runs%s", ticks / (ser / 1e3), ticks / (par / 1e3), ser / par, g_threads,
           equal ? "EQUAL" : "DIFFER", rounds * 2, g_window ? ", a render per 16 ticks" : "");
    report("engine", detail, t, false, note);

    // A tick at a time: the tail is what a frame feels.
    Tri tick;
    tick.d[kSerial] = d1; tick.d[kPool] = dn; tick.has_ref = true; tick.d[kRef] = plain1;
    format(detail, sizeof detail, "one tick, %u bodies", g_bodies);
    format(note, sizeof note, "1 thread p99 %.0f, worst %.0f us; %u threads p99 %.0f, worst %.0f us; bare arrays on %u spinning threads p50 %.0f us",
           d1.p99 * 1e3, d1.max * 1e3, g_threads, dn.p99 * 1e3, dn.max * 1e3, g_threads, plainn.p50 * 1e3);
    report("engine tick", detail, tick, true, note);

    // Where the ticks went, from the profiler: the top zones of the last pair of runs,
    // and the workers' time split into busy and waiting.
    if (!one.zone_count && !many.zone_count) { print("\nengine breakdown: none, this build has no profiler (dev has)\n"); return; }
    print("\nengine breakdown (%u ticks; a unit's zone and its chunks share its name, and both are in its sum):\n", ticks);
    print("  %-32s %10s %10s\n", "zone", "1 thread", "all");
    for (u32 i = 0; i < 6 && i < one.zone_count; ++i) {
        f64 m = 0;
        for (u32 k = 0; k < many.zone_count; ++k) if (strcmp(many.zones[k].name, one.zones[i].name) == 0) m = many.zones[k].ms;
        print("  %-32s %10.1f %10.1f  ms, summed over threads\n", one.zones[i].name, one.zones[i].ms, m);
    }
    print("  %-32s %10s %10.1f  ms busy, %.1f ms waiting (%.0f%% of worker time)\n", "workers", "", many.busy_ms, many.wait_ms,
           many.busy_ms + many.wait_ms > 0 ? 100.0 * many.wait_ms / (many.busy_ms + many.wait_ms) : 0.0);
    if (one.dropped || many.dropped)
        print("  the profiler dropped %llu records: the breakdown is short by them\n", (unsigned long long)(one.dropped + many.dropped));
}

// ---- 7. the brawl: one problem, four ways (brawl.h) -----------------------------------
struct BrawlRun { Dist tick; BrawlBooks books; };

BrawlRun run_bare_brawl(BareBrawl how, bool parallel, u32 ticks) {
    static Samples s;
    s.n = 0;
    BrawlRun r{};
    BareArena* a = bare_arena_create(g_fighters);
    if (!a) { KV_ERROR("cpukiller", "no memory for the brawl"); return r; }
    for (u32 t = 0; t < ticks; ++t) {
        f64 t0 = time_now();
        bare_arena_tick(a, how, parallel);
        s.add(ms_since(t0));
    }
    r.books = bare_arena_books(a);
    r.tick = dist_of(s);
    bare_arena_destroy(a);
    return r;
}

BrawlRun run_engine_brawl(u32 threads, u32 ticks) {
    static Samples s;
    s.n = 0;
    BrawlRun r{};
    Engine e;
    e.threads = threads;
    e.clock.max_ticks_per_frame = 1;
    brawl_set_fighters(g_fighters);
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn);
    if (!engine_init(e, brawl_modules())) { log_set_level(was); KV_ERROR("cpukiller", "engine_init failed for the brawl"); return r; }
    KV_MARK_V("engine run", threads);
    for (u32 done = 0; done < ticks;) {
        u64 before = e.clock.tick_total;
        f64 t0 = time_now();
        engine_frame(e, 1.0 / 60.0);
        f64 ms = ms_since(t0);
        u32 ran = (u32)(e.clock.tick_total - before);
        if (ran) s.add(ms / ran);
        done += ran;
        prof_collect(); // nothing reads it here; it keeps the rings from filling
    }
    r.books = brawl_books(e);
    r.tick = dist_of(s);
    engine_shutdown(e);
    log_set_level(was);
    return r;
}

void scenario_brawl() {
    KV_ZONE("brawl");
    constexpr u32 kVersions = 4, kCols = 8, kCell = 64, kMaxRuns = 16;
    const char* names[kVersions] = {"bare, unsynchronized", "bare, atomic", "bare, a lock a victim", "engine: events, one handler"};
    u32 ticks = g_quick ? 60 : 200;
    u32 runs = reps(5);
    if (runs > kMaxRuns) runs = kMaxRuns;
    static char cells[kVersions][kCols][kCell];
    for (u32 v = 0; v < kVersions; ++v) {
        KV_MARK_V("brawl version", v);
        bool engine = v == 3;
        if (!engine) jobs_init(g_threads - 1);
        BrawlRun one = engine ? run_engine_brawl(1, ticks) : run_bare_brawl((BareBrawl)v, false, ticks);
        // The same fight, `runs` times over on every thread: the worst the books came to,
        // and how many different endings it had.
        static Samples medians;
        medians.n = 0;
        u64 hashes[kMaxRuns];
        u32 distinct = 0;
        i64 lost = 0, extra = 0;
        u64 dealt = 0, deaths = 0;
        bool as_one = true;
        for (u32 r = 0; r < runs; ++r) {
            BrawlRun many = engine ? run_engine_brawl(g_threads, ticks) : run_bare_brawl((BareBrawl)v, true, ticks);
            medians.add(many.tick.p50);
            i64 l = many.books.damage_lost < 0 ? -many.books.damage_lost : many.books.damage_lost;
            i64 x = many.books.extra_deaths < 0 ? -many.books.extra_deaths : many.books.extra_deaths;
            if (l >= lost) { lost = l; dealt = many.books.dealt; }
            if (x >= extra) { extra = x; deaths = many.books.deaths; }
            as_one = as_one && many.books.hash == one.books.hash;
            bool seen = false;
            for (u32 k = 0; k < distinct; ++k) seen = seen || hashes[k] == many.books.hash;
            if (!seen) hashes[distinct++] = many.books.hash;
        }
        if (!engine) jobs_shutdown();
        f64 many_ms = dist_of(medians).p50;
        if (one.books.damage_lost || one.books.extra_deaths)
            KV_ERROR("cpukiller", "brawl, %s, one thread: %lld damage lost, %lld extra deaths", names[v], (long long)one.books.damage_lost, (long long)one.books.extra_deaths);
        format(cells[v][0], kCell, "%s", names[v]);
        format(cells[v][1], kCell, "%.0f us", one.tick.p50 * 1e3);
        format(cells[v][2], kCell, "%.0f us", many_ms * 1e3);
        format(cells[v][3], kCell, "%.1f ns", g_fighters ? many_ms * 1e6 / g_fighters : 0.0);
        format(cells[v][4], kCell, "%lld of %llu", (long long)lost, (unsigned long long)dealt);
        format(cells[v][5], kCell, "%lld of %llu", (long long)extra, (unsigned long long)deaths);
        format(cells[v][6], kCell, "%u in %u runs", distinct, runs);
        format(cells[v][7], kCell, "%s", as_one ? "yes" : "NO");
    }
    char threads_head[32];
    format(threads_head, sizeof threads_head, "%u threads", g_threads);
    const char* heads[kCols] = {"brawl", "1 thread", threads_head, "a swing", "damage lost", "extra deaths", "endings", "as 1 thread"};
    const u32 widths[kCols] = {28, 10, 11, 8, 20, 16, 13, 0};
    print("\nbrawl: %u fighters, each swinging every tick, %u ticks a run. A tick's time; a swing's share of it on %u threads;\n"
          "and of the %u-thread runs, the worst by the books, how many endings they had, and whether they ended as one thread did.\n",
          g_fighters, ticks, g_threads, g_threads);
    print_table(heads, kCols, cells[0][0], kCell, kVersions, widths);
}

// ---- the sweep: what scales, at every thread count -------------------------------------
void scenario_sweep() {
    constexpr u32 kMaxSteps = 8, kCases = 8, kCell = 48;
    u32 steps[kMaxSteps], n = 0, top = g_threads;
    for (u32 t = 1; t < top && n < kMaxSteps - 1; t *= 2) steps[n++] = t;
    steps[n++] = top;
    const char* names[kCases] = {"pool, 0.5 us chunks", "reference, 0.5 us chunks", "pool, 35 us chunks", "reference, 35 us chunks",
                                 "allocator, small blocks", "allocator, remote frees", "engine ticks", "bare arrays, spinning pool"};
    static char cells[kCases][kMaxSteps + 1][kCell];
    static char head_text[kMaxSteps + 1][kCell];
    const char* heads[kMaxSteps + 1];
    u32 widths[kMaxSteps + 1];
    format(head_text[0], kCell, "speedup over serial"); heads[0] = head_text[0]; widths[0] = 28;
    for (u32 c = 0; c < kCases; ++c) format(cells[c][0], kCell, "%s", names[c]);
    u32 ticks = g_quick ? 64 : g_ticks;
    u32 rounds = reps(3);
    f64 plain_serial = run_plain(ticks, false).p50;
    for (u32 i = 0; i < n; ++i) {
        g_threads = steps[i];
        format(head_text[i + 1], kCell, "%u", g_threads); heads[i + 1] = head_text[i + 1]; widths[i + 1] = 7;
        jobs_init(g_threads - 1);
        Tri fine = contention_case(1, rounds), coarse = contention_case(64, rounds);
        bool ok = true;
        Tri small = churn_case(g_quick ? 2000 : 8000, 256, false, rounds, &ok);
        Tri remote = remote_case(g_quick ? 2000 : 8000, rounds, &ok);
        jobs_shutdown();
        static Samples run_one, run_many;
        run_one.n = run_many.n = 0;
        for (u32 r = 0; r < rounds; ++r) {
            run_many.add(run_engine(g_threads, ticks, false, nullptr).ms);
            run_one.add(run_engine(1, ticks, false, nullptr).ms);
        }
        f64 plain = run_plain(ticks, true).p50;
        f64 v[kCases] = {fine.d[kSerial].p50 / fine.d[kPool].p50, fine.d[kSerial].p50 / fine.d[kRef].p50,
                         coarse.d[kSerial].p50 / coarse.d[kPool].p50, coarse.d[kSerial].p50 / coarse.d[kRef].p50,
                         small.d[kSerial].p50 / small.d[kPool].p50, remote.d[kSerial].p50 / remote.d[kPool].p50,
                         dist_of(run_one).p50 / dist_of(run_many).p50, plain_serial / plain};
        for (u32 c = 0; c < kCases; ++c) format(cells[c][i + 1], kCell, "%.1fx", v[c]);
    }
    g_threads = top;
    // print_table wants one block of rows * cols cells: pack the columns in use.
    static char packed[kCases * (kMaxSteps + 1)][kCell];
    for (u32 c = 0; c < kCases; ++c)
        for (u32 k = 0; k <= n; ++k) memcpy(packed[c * (n + 1) + k], cells[c][k], kCell);
    print("\n");
    print_table(heads, n + 1, packed[0], kCell, kCases, widths);
    print("\nThe engine's and the allocator's serial are their own on one thread; the bare arrays' is the bare arrays'.\n");
}

const char* build_name() {
#if KV_DEBUG
    return "debug";
#elif KV_DEV
    return "dev";
#else
    return "ship";
#endif
}

} // namespace

int main(int argc, char** argv) {
    g_threads = os_cpu_count();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) g_threads = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bodies") == 0 && i + 1 < argc) g_bodies = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) g_ticks = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fighters") == 0 && i + 1 < argc) g_fighters = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) g_reps = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) g_only = argv[++i];
        else if (strcmp(argv[i], "--spin") == 0 && i + 1 < argc) jobs_set_idle_spin(atof(argv[++i]) * 1e-6);
        else if (strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (strcmp(argv[i], "--sweep") == 0) g_sweep = true;
        else if (strcmp(argv[i], "--md") == 0) g_md = true;
        else if (strcmp(argv[i], "--window") == 0) g_window = true;
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) g_trace = argv[++i];
        else { print_err("usage: %s [--threads N] [--bodies N] [--ticks N] [--fighters N] [--reps N] [--spin US] [--only SCENARIO] [--quick] [--sweep] [--md] [--window] [--profile FILE]\n", argv[0]); return 2; }
    }
    if (g_threads < 1) g_threads = 1;
    if (g_sweep && g_window) { print_err("%s: --sweep runs without the window\n", argv[0]); return 2; }
    // The profiler is always on here: the breakdown reads it. A trace is written on request.
    prof_enable(true);
    prof_thread_name("main");
    if (g_trace) prof_trace_open(g_trace);
    print("cpukiller: %u threads (%u cpus), %s build, idle spin %.0f us%s%s%s\n", g_threads, os_cpu_count(), build_name(), jobs_idle_spin() * 1e6, g_quick ? ", quick" : "",
          g_window ? ", window" : "", g_sweep ? ", sweep" : "");
    log_set_level(LogLevel::Warn); // the pool says how many workers it has every time it is asked for some

    if (g_sweep) {
        scenario_sweep();
    } else {
        if (g_window) {
            FrameLoopConfig cfg;
            cfg.title = "korvid cpukiller";
            cfg.width = 960; cfg.height = 540;
            cfg.vsync = false;             // never wait on the display
            cfg.report_every_seconds = 0;
            cfg.exit_on_close = cfg.exit_on_escape = false;
            frameloop_set_config(cfg);
        } else {
            pool_and_memory_scenarios();
            jobs_shutdown();
        }
        if (wanted("engine")) scenario_engine();
        if (g_row_count) print_rows();
        if (!g_window && wanted("brawl")) scenario_brawl();
    }
    prof_collect();
    prof_shutdown();
    MemStats s = mem_stats();
    print("\nmemory at exit: %llu live allocations, %llu bytes, %llu KB committed\n",
           (unsigned long long)s.live_allocs, (unsigned long long)s.live_bytes, (unsigned long long)s.committed_bytes / 1024);
    names_shutdown();
    mem_shutdown();
    return 0;
}
