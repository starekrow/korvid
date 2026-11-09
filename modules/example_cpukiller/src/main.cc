// example_cpukiller: a stress tool. No window. It measures the job pool's dispatch
// overhead and contention, nested batches, the memory system under cross-thread load,
// and the engine's tick rate on one thread against many, with the state hash as the
// referee. Numbers go to stdout as a table; every scenario also checks that the memory
// accounting comes back to where it started.
//
//   example_cpukiller [--threads N] [--bodies N] [--ticks N] [--quick] [--window] [--profile FILE]
//
// Every scenario is a zone and every case a mark, so --profile FILE gives a trace of the
// whole run; the engine scenario's breakdown (what the ticks spent where, and how much of
// the workers' time was waiting) comes from the profiler's collector, not from stopwatches.
//
// With --window the usual stack comes up first (platform, renderer, frame loop), the
// pool and memory scenarios run while the window and swapchain are alive, and the engine
// ticks are driven through the frame loop with a render after every batch of ticks:
// memory can behave differently with a GPU's fingers in it.
#include "korvid/example_cpukiller/module.h"
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
#include "korvid/core/thread.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

u32  g_threads = 0;
u32  g_bodies  = 200000;
u32  g_ticks   = 300;
bool g_quick   = false;
bool g_window  = false;
const char* g_trace = nullptr;
constexpr u32 kTicksPerFrame = 16; // ticks between renders in window mode, and per engine_frame in both

struct Row { char name[48]; char detail[64]; f64 serial_ms; f64 parallel_ms; char note[160]; };
Row g_rows[32];
u32 g_row_count = 0;

void report(const char* name, const char* detail, f64 serial_ms, f64 parallel_ms, const char* note) {
    if (g_row_count >= KV_COUNTOF(g_rows)) return;
    Row& r = g_rows[g_row_count++];
    snprintf(r.name, sizeof r.name, "%s", name);
    snprintf(r.detail, sizeof r.detail, "%s", detail);
    r.serial_ms = serial_ms; r.parallel_ms = parallel_ms;
    snprintf(r.note, sizeof r.note, "%s", note);
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

f64 ms_since(f64 t0) { return (time_now() - t0) * 1e3; }

// ---- 1. dispatch overhead: chunks that do nothing ------------------------------------
void nothing(u32, u32, u32, void*) {}

void scenario_overhead() {
    KV_ZONE("dispatch");
    const u32 counts[] = {2, 16, 256, 4096, 65536};
    for (u32 chunks : counts) {
        KV_MARK_V("dispatch case", chunks);
        u32 reps = g_quick ? 20 : 200;
        jobs_init(g_threads - 1);
        jobs_stats_reset();
        f64 t0 = time_now();
        for (u32 r = 0; r < reps; ++r) jobs_run(chunks, 1, nothing, nullptr);
        f64 par = ms_since(t0);
        JobStats st = jobs_stats();
        jobs_init(0);
        t0 = time_now();
        for (u32 r = 0; r < reps; ++r) jobs_run(chunks, 1, nothing, nullptr);
        f64 ser = ms_since(t0);
        char detail[64], note[160];
        snprintf(detail, sizeof detail, "%u empty chunks x %u", chunks, reps);
        snprintf(note, sizeof note, "%.2f us/batch, %.0f ns/chunk, caller ran %llu%%, %llu helper waits",
                 par * 1e3 / reps, par * 1e6 / reps / chunks, (unsigned long long)(st.tasks ? 100 * st.tasks_by_caller / st.tasks : 0),
                 (unsigned long long)st.helper_waits / reps);
        report("dispatch", detail, ser, par, note);
    }
}

// ---- 2. contention: fixed work, chunk sizes from tiny to large ----------------------
struct Spin { u32 iters; u64* sink; };
void spin(u32, u32 begin, u32 end, void* user) {
    Spin& s = *(Spin*)user;
    u64 acc = 0;
    for (u32 i = begin; i < end; ++i)
        for (u32 k = 0; k < s.iters; ++k) acc = hash_mix(acc ^ (i + k));
    kv::atomic_add_fetch(s.sink, acc, kv::MO_RELAXED);
}

void scenario_contention() {
    KV_ZONE("contention");
    const u32 total_items = g_quick ? 1u << 16 : 1u << 18; // items of 200 hash rounds each
    const u32 grains[] = {1, 8, 64, 512, 4096};
    for (u32 grain : grains) {
        KV_MARK_V("contention case", grain);
        u64 sink = 0;
        Spin sp{200, &sink};
        jobs_init(0);
        f64 t0 = time_now();
        jobs_run(total_items, grain, spin, &sp);
        f64 ser = ms_since(t0);
        jobs_init(g_threads - 1);
        jobs_stats_reset();
        t0 = time_now();
        jobs_run(total_items, grain, spin, &sp);
        f64 par = ms_since(t0);
        JobStats st = jobs_stats();
        char detail[64], note[160];
        snprintf(detail, sizeof detail, "%u items, grain %u (%u chunks)", total_items, grain, jobs_chunks(total_items, grain));
        snprintf(note, sizeof note, "speedup %.1fx of %u, %llu worker waits, caller ran %llu%%", ser / par, g_threads,
                 (unsigned long long)st.worker_waits, (unsigned long long)(st.tasks ? 100 * st.tasks_by_caller / st.tasks : 0));
        report("contention", detail, ser, par, note);
    }
}

// ---- 3. nested batches: every outer chunk runs an inner batch -------------------------
struct Nest { u32 inner; u32 iters; u64* sink; };
void nested(u32, u32 begin, u32 end, void* user) {
    Nest& n = *(Nest*)user;
    for (u32 i = begin; i < end; ++i) {
        Spin sp{n.iters, n.sink};
        jobs_run(n.inner, 1, spin, &sp);
    }
}

void scenario_nested() {
    KV_ZONE("nested");
    u64 sink = 0;
    Nest n{64, 200, &sink};
    u32 outer = g_quick ? 16 : 64;
    jobs_init(0);
    f64 t0 = time_now();
    jobs_run(outer, 1, nested, &n);
    f64 ser = ms_since(t0);
    jobs_init(g_threads - 1);
    jobs_stats_reset();
    t0 = time_now();
    jobs_run(outer, 1, nested, &n);
    f64 par = ms_since(t0);
    JobStats st = jobs_stats();
    char detail[64], note[160];
    snprintf(detail, sizeof detail, "%u outer x %u inner chunks", outer, n.inner);
    snprintf(note, sizeof note, "speedup %.1fx, %llu batches, %llu helper waits", ser / par, (unsigned long long)st.batches, (unsigned long long)st.helper_waits);
    report("nested", detail, ser, par, note);
}

// ---- 4. memory under load ------------------------------------------------------------
// (a) general allocator, alloc and free on the same thread, mixed sizes
struct Churn { u32 per_chunk; };
void churn_same(u32 chunk, u32 begin, u32 end, void* user) {
    Churn& c = *(Churn*)user;
    void* keep[64];
    u32 kept = 0;
    u64 r = chunk + 1;
    for (u32 i = begin; i < end; ++i)
        for (u32 k = 0; k < c.per_chunk; ++k) {
            r = hash_mix(r);
            usize size = 16 + (r % 2048);
            void* p = mem_alloc(size);
            memset(p, (int)r, size);
            if (kept < 64) keep[kept++] = p;
            else { mem_free(keep[r % 64]); keep[r % 64] = p; }
        }
    for (u32 k = 0; k < kept; ++k) mem_free(keep[k]);
}

// (b) remote frees: batch A fills a slot table, batch B frees each chunk's neighbor's slots
struct Remote { void** slots; u32 per_chunk; u32 chunks; };
void remote_alloc(u32 chunk, u32, u32, void* user) {
    Remote& r = *(Remote*)user;
    u64 s = chunk * 31 + 7;
    for (u32 k = 0; k < r.per_chunk; ++k) { s = hash_mix(s); r.slots[chunk * r.per_chunk + k] = mem_alloc(16 + (s % 1024)); }
}
void remote_free(u32 chunk, u32, u32, void* user) {
    Remote& r = *(Remote*)user;
    u32 victim = (chunk + 1) % r.chunks;
    for (u32 k = 0; k < r.per_chunk; ++k) { mem_free(r.slots[victim * r.per_chunk + k]); r.slots[victim * r.per_chunk + k] = nullptr; }
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

// The pool owns two allocations while it is up (its thread array and its task queue, which
// grows with the largest batch), so the baseline is taken with the pool up and its queue
// already as large as any case needs, and the case must come back to it.
void pool_ready(u32 workers) {
    jobs_init(workers);
    jobs_run(4096, 1, nothing, nullptr);
    mem_checkpoint();
}

void run_memory_case(const char* name, u32 count, u32 grain, JobFn fn, void* user, const char* unit, u64 ops) {
    KV_MARK(name);
    pool_ready(0);
    f64 t0 = time_now();
    jobs_run(count, grain, fn, user, name);
    f64 ser = ms_since(t0);
    bool ok1 = mem_settled(name);
    pool_ready(g_threads - 1);
    t0 = time_now();
    jobs_run(count, grain, fn, user, name);
    f64 par = ms_since(t0);
    bool ok2 = mem_settled(name);
    char detail[64], note[160];
    snprintf(detail, sizeof detail, "%llu %s", (unsigned long long)ops, unit);
    snprintf(note, sizeof note, "speedup %.1fx, %.1f M%s/s parallel%s", ser / par, (f64)ops / par / 1e3, unit, ok1 && ok2 ? "" : ", LEAK");
    report(name, detail, ser, par, note);
}

void scenario_memory() {
    KV_ZONE("memory");
    u32 chunks = g_quick ? 64 : 256;
    Churn c{g_quick ? 500u : 2000u};
    run_memory_case("mem same-thread", chunks, 1, churn_same, &c, "allocs", (u64)chunks * c.per_chunk);

    u32 per = g_quick ? 2000 : 8000;
    Remote r{nullptr, per, chunks};
    r.slots = alloc_items<void*>(mem_general(), chunks * per);
    pool_ready(0);
    f64 t0 = time_now();
    jobs_run(chunks, 1, remote_alloc, &r);
    jobs_run(chunks, 1, remote_free, &r);
    f64 ser = ms_since(t0);
    bool ok1 = mem_settled("mem remote-free");
    pool_ready(g_threads - 1);
    t0 = time_now();
    jobs_run(chunks, 1, remote_alloc, &r);
    jobs_run(chunks, 1, remote_free, &r);
    f64 par = ms_since(t0);
    bool ok2 = mem_settled("mem remote-free");
    free_items(mem_general(), r.slots, chunks * per);
    char detail[64], note[160];
    snprintf(detail, sizeof detail, "%llu allocs freed by another chunk", (unsigned long long)chunks * per);
    snprintf(note, sizeof note, "speedup %.1fx, %.1f M allocs+frees/s%s", ser / par, (f64)chunks * per / par / 1e3, ok1 && ok2 ? "" : ", LEAK");
    report("mem remote-free", detail, ser, par, note);

    u32 scopes = g_quick ? 4000 : 20000;
    run_memory_case("mem scratch", scopes, 16, scratch_churn, nullptr, "scopes of 256 pushes", scopes);
    u32 arrays = g_quick ? 1000 : 4000;
    run_memory_case("mem arrays", arrays, 4, array_churn, nullptr, "arrays of 2000", arrays);
}

// ---- 5. the engine: ticks per second, one thread against many, hashes equal ----------
constexpr u32 kTop = 8;
struct EngineRun { f64 ms; u64 hash; u32 live; f64 top_ms[kTop]; const char* top[kTop]; f64 busy_ms, wait_ms; };

// What the collector saw of the run: the top zones (work, not waits or containers) and
// the workers' busy and waiting time.
void breakdown(EngineRun& r) {
    prof_collect();
    Span<const ProfStat> st = prof_frame_stats();
    u32 n = 0;
    for (const ProfStat& s : st) {
        if (n >= kTop) break;
        if (s.wait || strcmp(s.name, "ticks") == 0 || strcmp(s.name, "tick") == 0 || strcmp(s.name, "level") == 0) continue;
        r.top[n] = s.name; r.top_ms[n] = prof_ms(s.wall); ++n;
    }
    for (const ProfThreadStat& t : prof_thread_stats()) {
        if (!t.name || strncmp(t.name, "worker", 6) != 0) continue;
        r.busy_ms += prof_ms(t.busy); r.wait_ms += prof_ms(t.waiting);
    }
}

void pool_and_memory_scenarios() {
    scenario_overhead();
    scenario_contention();
    scenario_nested();
    scenario_memory();
}

// With the window, the pool and memory scenarios can run inside the first engine's life,
// GPU up, when `scenarios_inside` says so; the pool is then restored to the engine's count.
EngineRun run_engine(u32 threads, u32 ticks, bool scenarios_inside) {
    EngineRun r{};
    Engine e;
    e.threads = threads;
    e.clock.max_ticks_per_frame = kTicksPerFrame;
    cpukiller_set_bodies(g_bodies);
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn); // the schedule log, once, is enough
    bool ok = engine_init(e, g_window ? kv_module_list() : cpukiller_core_modules());
    log_set_level(was);
    if (!ok) { KV_ERROR("cpukiller", "engine_init failed"); return r; }
    if (scenarios_inside) {
        if (g_window) frameloop_step(e, 0.0); // show the window, present a frame
        pool_and_memory_scenarios();
        jobs_init(threads ? threads - 1 : 0);
        if (g_window) frameloop_step(e, 0.0);
    }
    f64 dt = kTicksPerFrame / 60.0;
    prof_collect(); // everything before the ticks is out of the way
    KV_MARK_V("engine run", threads);
    f64 t0 = time_now();
    for (u32 t = 0; t < ticks; t += kTicksPerFrame) {
        if (g_window) frameloop_step(e, dt);
        else engine_frame(e, dt);
    }
    r.ms = ms_since(t0);
    breakdown(r);
    r.hash = engine_hash_state(e);
    r.live = engine_table(e, "cpukiller.body")->live();
    engine_shutdown(e);
    return r;
}

void scenario_engine() {
    u32 ticks = g_quick ? 64 : g_ticks;
    ticks = (ticks + kTicksPerFrame - 1) / kTicksPerFrame * kTicksPerFrame;
    EngineRun many = run_engine(g_threads, ticks, g_window); // in window mode the other scenarios run in here
    EngineRun one  = run_engine(1, ticks, false);
    char detail[64], note[160];
    snprintf(detail, sizeof detail, "%u bodies, %u ticks%s", g_bodies, ticks, g_window ? ", window" : "");
    snprintf(note, sizeof note, "%.0f -> %.0f ticks/s, speedup %.1fx, hashes %s%s", ticks / (one.ms / 1e3), ticks / (many.ms / 1e3),
             one.ms / many.ms, one.hash == many.hash ? "EQUAL" : "DIFFER", g_window ? ", a render per 16 ticks" : "");
    report("engine", detail, one.ms, many.ms, note);
    if (one.hash != many.hash) KV_ERROR("cpukiller", "state hash differs between 1 and %u threads: %016llx vs %016llx",
                                        g_threads, (unsigned long long)one.hash, (unsigned long long)many.hash);
    // Where the ticks went, from the profiler: the top zones of each run, and the workers'
    // time split into busy and waiting.
    printf("\nengine breakdown (%u ticks):\n", ticks);
    printf("  %-32s %10s %10s\n", "zone", "1 thread", "all");
    for (u32 i = 0; i < 5 && i < kTop && one.top[i]; ++i) {
        f64 m = 0;
        for (u32 k = 0; k < kTop; ++k) if (many.top[k] && strcmp(many.top[k], one.top[i]) == 0) m = many.top_ms[k];
        printf("  %-32s %10.1f %10.1f  ms, summed over threads\n", one.top[i], one.top_ms[i], m);
    }
    printf("  %-32s %10s %10.1f  ms busy, %.1f ms waiting (%.0f%% of worker time)\n", "workers", "", many.busy_ms, many.wait_ms,
           many.busy_ms + many.wait_ms > 0 ? 100.0 * many.wait_ms / (many.busy_ms + many.wait_ms) : 0.0);
}

} // namespace

int main(int argc, char** argv) {
    g_threads = os_cpu_count();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) g_threads = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bodies") == 0 && i + 1 < argc) g_bodies = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) g_ticks = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (strcmp(argv[i], "--window") == 0) g_window = true;
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) g_trace = argv[++i];
        else { fprintf(stderr, "usage: %s [--threads N] [--bodies N] [--ticks N] [--quick] [--window] [--profile FILE]\n", argv[0]); return 2; }
    }
    if (g_threads < 1) g_threads = 1;
    // The profiler is always on here: the breakdown reads it. A trace is written on request.
    prof_enable(true);
    prof_thread_name("main");
    if (g_trace) prof_trace_open(g_trace);
    printf("cpukiller: %u threads (%u cpus)%s%s\n", g_threads, os_cpu_count(), g_quick ? ", quick" : "", g_window ? ", window" : "");

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
    scenario_engine();

    printf("\n%-16s %-40s %10s %10s  %s\n", "scenario", "what", "serial ms", "par ms", "notes");
    for (u32 i = 0; i < g_row_count; ++i) {
        const Row& r = g_rows[i];
        printf("%-16s %-40s %10.2f %10.2f  %s\n", r.name, r.detail, r.serial_ms, r.parallel_ms, r.note);
    }
    prof_collect();
    prof_shutdown();
    MemStats s = mem_stats();
    printf("\nmemory at exit: %llu live allocations, %llu bytes, %llu KB committed\n",
           (unsigned long long)s.live_allocs, (unsigned long long)s.live_bytes, (unsigned long long)s.committed_bytes / 1024);
    names_shutdown();
    mem_shutdown();
    return 0;
}
