// sockit2m: sock it to me. An I/O torture test the way the engine does I/O:
// nothing blocks, everything is pumped. It writes a working set of files, reads it back
// through the I/O queue on io_uring (cold, then hot) and on the synchronous path with a
// bounded number of requests in flight, blasts loopback TCP through the non-blocking
// sockets with an echo server on the same thread (bulk chunks, then CLI-sized lines),
// and then does the file reads and the socket echo at the same time. Every byte that
// comes back is checked against the pattern that went out.
//
// By default the jobs run inside the engine: the platform module owns the queue and
// pumps it once per frame, "sockit2m.sockets" steps the sockets once per tick in phase
// Input, "sockit2m.files" reaps and refills in PreSim, the frame loop is platform_pump
// plus engine_frame on real time with a fixed tick clock, and the file data lands in the
// module's allocator. That is what a game gets. With --direct the same jobs run in a tight
// loop over the primitives, which is what the primitives can do; --compare prints both.
//
// The numbers: throughput, request latency, message rate and round trip, and how long one
// frame (or one loop step) takes at worst and at the 99th percentile, which is the number
// that says whether the sim would have hitched.
//
//   sockit2m [--dir PATH] [--files N] [--size KB] [--inflight N] [--conns N]
//                    [--chunk KB] [--seconds S] [--tick-hz N] [--budget KB] [--frame-ms N]
//                    [--quick] [--keep] [--direct | --compare]
//
// The working set goes next to the executable unless --dir says otherwise (/tmp is often
// a tmpfs, which would measure memory, not a disk). Files are left behind with --keep.
//
// What it shows: the kernel does work at submission in proportion to the bytes (page cache
// allocation; on a hot cache the whole read), so the queue's byte budget per pump is what
// keeps a frame flat; "both, no budget" lifts it (--budget 0) to show the 30 ms frames
// come back. Cold reads meet the disk and complete asynchronously, which is the case the
// queue is built for. Inside the engine the sockets move once per tick, so their
// throughput is what the kernel buffers hold times the tick rate: --tick-hz shows the
// dependence. The idle scenario flags every read IoPriority::Idle and paces frames at
// --frame-ms (one tick by default): the reads go only through the Work cycle (engine_work,
// the platform's io_idle unit) in the time each frame has left, so the frame stays at its
// budget while the disk is kept busy.
#include "torture.h"
#include "korvid/sockit2m/module.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include <stdlib.h>
#include <string.h>

using namespace kv;

namespace {

// ---- the report ---------------------------------------------------------------------------------
struct Row { char mode[8]; char name[24]; char what[96]; char numbers[64]; char note[400]; };
Row g_rows[24];
u32 g_row_count = 0;
const char* g_mode = "engine";

void report(const char* name, const char* what, const char* numbers, const char* note) {
    if (g_row_count >= KV_COUNTOF(g_rows)) return;
    Row& r = g_rows[g_row_count++];
    format(r.mode, sizeof r.mode, "%s", g_mode);
    format(r.name, sizeof r.name, "%s", name);
    format(r.what, sizeof r.what, "%s", what);
    format(r.numbers, sizeof r.numbers, "%s", numbers);
    format(r.note, sizeof r.note, "%s", note);
}

MemStats g_base;
bool mem_settled(const char* what) {
    MemStats s = mem_stats();
    if (s.live_allocs != g_base.live_allocs || s.live_bytes != g_base.live_bytes) {
        KV_ERROR("sockit2m", "%s: %lld allocations, %lld bytes still live", what,
                 (long long)s.live_allocs - (long long)g_base.live_allocs, (long long)s.live_bytes - (long long)g_base.live_bytes);
        ++g_errors;
        return false;
    }
    return true;
}

// ---- the driver: who steps the jobs ----------------------------------------------------------------
// Direct: the job's own step in a tight loop. Engine: platform_pump and engine_frame on
// real time; the module's systems step the jobs at tick and frame rate.

struct Driver {
    Engine* e = nullptr; // null: direct
    u64 ticks_before = 0;
    f64 pace_s = 0.0;     // > 0: frames are paced to this period, idle work in the slack
    f64 idle_s = 0.0;     // what the paced frames spent on idle work
    u64 paced_frames = 0;

    IoQueue*   queue() { return e ? platform_io(*e) : nullptr; }
    Allocator* alloc() { return e ? sockit2m_allocator(*e) : nullptr; }
    const char* step_name() const { return e ? "frame" : "pump"; }

    void begin(FsJob* fs, SockJob* sock) {
        if (e) { sockit2m_set_jobs(*e, fs, sock); ticks_before = e->clock.tick_count; }
        idle_s = 0.0; paced_frames = 0;
    }
    // One round; returns how long it took in ms.
    f64 step(FsJob* fs, SockJob* sock) {
        f64 t = time_now();
        if (e) {
            static f64 prev = 0.0;
            f64 now = time_now();
            f64 dt = prev > 0.0 ? now - prev : 0.0;
            prev = now;
            platform_pump(*e);
            engine_frame(*e, dt);
            if (pace_s > 0.0) { // a frame with a deadline: idle work in the slack, then wait for it
                f64 slack = pace_s - (time_now() - t) - 0.0005;
                if (slack > 0.0) idle_s += engine_work(*e, slack);
                f64 busy = (time_now() - t) * 1e3; // the frame's work, idle included; the wait is not work
                f64 left = pace_s - (time_now() - t);
                if (left > 0.0) time_sleep(left);
                ++paced_frames;
                return busy;
            }
        } else {
            if (fs) fs->step();
            if (sock) sock->step();
        }
        return (time_now() - t) * 1e3;
    }
    void finish(FsJob* fs, SockJob* sock) {
        if (!e) return;
        sockit2m_set_jobs(*e, nullptr, nullptr);
        KV_UNUSED(fs); KV_UNUSED(sock);
    }
    u64 ticks() const { return e ? e->clock.tick_count - ticks_before : 0; }
};

f64 g_fs_solo_mbs = 0.0, g_sock_solo_mbs = 0.0;

void clock_note(char* out, usize cap, const Driver& d, const PumpClock& clock) {
    if (d.e)
        format(out, cap, "%llu frames, %llu ticks; frame avg %.1f us, p99 <= %.3f ms, worst %.2f ms",
                 (unsigned long long)clock.total, (unsigned long long)d.ticks(), clock.avg_ms() * 1e3, clock.percentile_ms(0.99), clock.worst_ms);
    else
        format(out, cap, "%llu pumps; pump avg %.1f us, p99 <= %.3f ms, worst %.2f ms",
                 (unsigned long long)clock.total, clock.avg_ms() * 1e3, clock.percentile_ms(0.99), clock.worst_ms);
}

void scenario_fs_read(Driver& d, IoBackend backend, const char* name, bool cold) {
    // The engine's queue chose its backend at startup (--io); only the direct mode adds the sync row.
    FsJob fs;
    PumpClock clock;
    if (cold && !evict_working_set()) KV_WARN("sockit2m", "could not evict the working set; the cold pass may be warm");
    if (!fs.begin(d.queue(), backend, d.alloc(), g_opt.inflight)) { report(name, "could not start", "", "FAILED"); ++g_errors; return; }
    d.begin(&fs, nullptr);
    while (!fs.done()) clock.add(d.step(&fs, nullptr));
    d.finish(&fs, nullptr);
    char what[96], numbers[64], note[400], cn[160];
    format(what, sizeof what, "%u MB in %u files, %u in flight, %s, %s cache", g_opt.files * g_opt.size_kb / 1024, g_opt.files, g_opt.inflight, fs.q->backend_name(), cold ? "cold" : "hot");
    format(numbers, sizeof numbers, "%8.1f MB/s %8.0f files/s", fs.mb_per_s(), (f64)g_opt.files / fs.seconds());
    clock_note(cn, sizeof cn, d, clock);
    format(note, sizeof note, "latency avg %.2f ms, max %.2f ms; %s%s", fs.lat_sum / g_opt.files * 1e3, fs.lat_max * 1e3, cn, fs.failed ? "; DATA ERRORS" : "");
    report(name, what, numbers, note);
    if (backend == g_opt.io && !cold) g_fs_solo_mbs = fs.mb_per_s();
    fs.end();
    mem_settled(name);
}

void scenario_sockets(Driver& d, bool lines, const char* name) {
    SockJob sj;
    PumpClock clock;
    f64 seconds = g_opt.quick ? 1.0 : g_opt.seconds;
    u32 conns = lines ? (g_opt.conns < 4 ? g_opt.conns : 4) : g_opt.conns;
    if (!sj.begin(conns, lines, seconds, d.alloc())) { report(name, "could not start", "", "FAILED"); ++g_errors; sj.end(); return; }
    d.begin(nullptr, &sj);
    while (!sj.done()) clock.add(d.step(nullptr, &sj));
    d.finish(nullptr, &sj);
    sj.drain();
    char what[96], numbers[64], note[400], cn[160];
    f64 s = sj.seconds();
    clock_note(cn, sizeof cn, d, clock);
    if (lines) {
        format(what, sizeof what, "%u conns, \"N\\n\" lines echoed, %.0f s", conns, seconds);
        format(numbers, sizeof numbers, "%8.0f lines/s %7.1f MB/s", (f64)sj.round_trips / s, (f64)(sj.tx + sj.rx) / 1048576.0 / s);
        format(note, sizeof note, "%llu round trips, rtt avg %.3f ms, max %.2f ms; %s%s",
                 (unsigned long long)sj.round_trips, sj.round_trips ? sj.rtt_sum / (f64)sj.round_trips * 1e3 : 0.0, sj.rtt_max * 1e3,
                 cn, sj.failed ? "; DATA ERRORS" : "");
    } else {
        format(what, sizeof what, "%u conns x %u KB chunks echoed, %.0f s", conns, g_opt.chunk_kb, seconds);
        format(numbers, sizeof numbers, "%8.1f MB/s out %7.1f MB/s back", (f64)sj.tx / 1048576.0 / s, (f64)sj.rx / 1048576.0 / s);
        format(note, sizeof note, "%llu MB verified byte for byte; %s%s", (unsigned long long)(sj.verified / 1048576), cn, sj.failed ? "; DATA ERRORS" : "");
        g_sock_solo_mbs = (f64)sj.rx / 1048576.0 / s;
    }
    report(name, what, numbers, note);
    sj.end();
    mem_settled(name);
}

// Files and sockets together, for as long as the file pass takes.
void scenario_both(Driver& d, u32 inflight, const char* name) {
    FsJob fs;
    SockJob sj;
    PumpClock clock;
    if (!fs.begin(d.queue(), g_opt.io, d.alloc(), inflight) || !sj.begin(g_opt.conns, false, 3600.0, d.alloc())) {
        report(name, "could not start", "", "FAILED"); ++g_errors; fs.end(); sj.end(); return;
    }
    d.begin(&fs, &sj);
    while (!fs.done()) clock.add(d.step(&fs, &sj));
    sj.stop();
    d.step(&fs, &sj); // one more round so the sockets see the stop
    d.finish(&fs, &sj);
    sj.drain();
    f64 s = sj.seconds();
    f64 fs_mbs = fs.mb_per_s(), sock_mbs = s > 0 ? (f64)sj.rx / 1048576.0 / s : 0.0;
    char what[96], numbers[64], note[400], cn[160];
    clock_note(cn, sizeof cn, d, clock);
    usize budget = fs.q->budget.bytes_per_pump;
    if (budget) format(what, sizeof what, "reads (%s, %u in flight, %llu MB/pump) + %u echo conns", fs.q->backend_name(), inflight, (unsigned long long)(budget >> 20), g_opt.conns);
    else        format(what, sizeof what, "reads (%s, %u in flight, no budget) + %u echo conns", fs.q->backend_name(), inflight, g_opt.conns);
    format(numbers, sizeof numbers, "%8.1f MB/s files %6.1f MB/s sock", fs_mbs, sock_mbs);
    format(note, sizeof note, "files at %.0f%% of solo, sockets at %.0f%% of solo; file latency avg %.2f ms, max %.2f ms; %s%s",
             g_fs_solo_mbs > 0 ? 100.0 * fs_mbs / g_fs_solo_mbs : 0.0, g_sock_solo_mbs > 0 ? 100.0 * sock_mbs / g_sock_solo_mbs : 0.0,
             fs.lat_sum / g_opt.files * 1e3, fs.lat_max * 1e3, cn, fs.failed || sj.failed ? "; DATA ERRORS" : "");
    report(name, what, numbers, note);
    fs.end();
    sj.end();
    mem_settled(name);
}

// Every read at idle priority, frames paced: the disk is served only from the slack.
void scenario_idle(Driver& d, bool cold, const char* name) {
    if (!d.e) return;
    FsJob fs;
    PumpClock clock;
    if (cold && !evict_working_set()) KV_WARN("sockit2m", "could not evict the working set; the cold pass may be warm");
    if (!fs.begin(d.queue(), g_opt.io, d.alloc(), g_opt.files, IoPriority::Idle)) { report(name, "could not start", "", "FAILED"); ++g_errors; return; }
    IoQueue* q = d.queue();
    u64 idle_calls0 = q->idle_calls, idle_bytes0 = q->idle_bytes;
    f64 period = g_opt.frame_ms > 0.0 ? g_opt.frame_ms * 1e-3 : 1.0 / g_opt.tick_hz;
    d.pace_s = period;
    d.begin(&fs, nullptr);
    while (!fs.done()) clock.add(d.step(&fs, nullptr));
    d.finish(&fs, nullptr);
    d.pace_s = 0.0;
    char what[96], numbers[64], note[400];
    format(what, sizeof what, "%u MB idle-priority reads, frames paced at %.1f ms, %s cache", g_opt.files * g_opt.size_kb / 1024, period * 1e3, cold ? "cold" : "hot");
    format(numbers, sizeof numbers, "%8.1f MB/s %8.0f files/s", fs.mb_per_s(), (f64)g_opt.files / fs.seconds());
    format(note, sizeof note, "latency avg %.2f ms, max %.2f ms; %llu frames, idle %.2f ms/frame in %llu calls, %llu MB admitted idle; frame worst %.2f ms of work incl. idle%s",
             fs.lat_sum / g_opt.files * 1e3, fs.lat_max * 1e3, (unsigned long long)d.paced_frames,
             d.paced_frames ? d.idle_s / (f64)d.paced_frames * 1e3 : 0.0, (unsigned long long)(q->idle_calls - idle_calls0),
             (unsigned long long)((q->idle_bytes - idle_bytes0) >> 20), clock.worst_ms, fs.failed ? "; DATA ERRORS" : "");
    report(name, what, numbers, note);
    fs.end();
    mem_settled(name);
}

void run_all(Driver& d) {
    scenario_fs_read(d, g_opt.io, "fs read cold", true);
    scenario_fs_read(d, g_opt.io, "fs read hot", false);
    if (!d.e && g_opt.io != IoBackend::Sync) scenario_fs_read(d, IoBackend::Sync, "fs read sync", false);
    scenario_sockets(d, false, "sockets bulk");
    scenario_sockets(d, true, "sockets lines");
    scenario_both(d, g_opt.inflight, "both");
    if (g_opt.budget_kb != 0) { // the same with the byte budget lifted: what the budget buys
        i64 saved = g_opt.budget_kb;
        g_opt.budget_kb = 0;
        scenario_both(d, g_opt.inflight, "both, no budget");
        g_opt.budget_kb = saved;
    }
    scenario_idle(d, true, "idle reads cold");
    scenario_idle(d, false, "idle reads hot");
}

bool run_engine_mode() {
    g_mode = "engine";
    Engine e;
    e.clock.tick_dt = 1.0 / g_opt.tick_hz;
    PlatformMounts mounts;
    mounts.io_backend = g_opt.io; // the platform's queue on the backend asked for
    platform_configure_mounts(mounts);
    LogLevel was = log_level();
    log_set_level(LogLevel::Warn); // the schedule log, once, is enough
    bool ok = engine_init(e, kv_module_list());
    log_set_level(was);
    if (!ok) { KV_ERROR("sockit2m", "engine_init failed"); return false; }
    IoQueue* q = platform_io(e);
    if (!q) { KV_ERROR("sockit2m", "no platform queue"); engine_shutdown(e); return false; }
    // The queue's working arrays grow as they are used; size them before the baseline so
    // the leak check sees the jobs' allocations and nothing else.
    q->queued.reserve(g_opt.files + 16);
    q->completed.reserve(g_opt.files + 16);
    print("engine: platform queue on %s, tick %.0f Hz, up to %u ticks per frame\n", q->backend_name(), g_opt.tick_hz, e.clock.max_ticks_per_frame);
    g_base = mem_stats();
    Driver d; d.e = &e;
    run_all(d);
    engine_shutdown(e);
    return true;
}

void run_direct_mode() {
    g_mode = "direct";
    g_base = mem_stats();
    Driver d;
    run_all(d);
}

} // namespace

int main(int argc, char** argv) {
    Options& o = g_opt;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) o.dir = argv[++i];
        else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) o.files = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) o.size_kb = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--inflight") == 0 && i + 1 < argc) o.inflight = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--conns") == 0 && i + 1 < argc) o.conns = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) o.chunk_kb = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) o.seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--tick-hz") == 0 && i + 1 < argc) o.tick_hz = atof(argv[++i]);
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) o.budget_kb = atoll(argv[++i]);
        else if (strcmp(argv[i], "--frame-ms") == 0 && i + 1 < argc) o.frame_ms = atof(argv[++i]);
        else if (strcmp(argv[i], "--quick") == 0) o.quick = true;
        else if (strcmp(argv[i], "--keep") == 0) o.keep = true;
        else if (strcmp(argv[i], "--direct") == 0) o.direct = true;
        else if (strcmp(argv[i], "--compare") == 0) o.compare = true;
        else if (strcmp(argv[i], "--io") == 0 && i + 1 < argc && !strcmp(argv[i + 1], "sync")) { o.io = IoBackend::Sync; ++i; }
        else if (strcmp(argv[i], "--io") == 0 && i + 1 < argc && (!strcmp(argv[i + 1], "ring") || !strcmp(argv[i + 1], "uring"))) { o.io = IoBackend::Ring; ++i; }
        else { print_err("usage: %s [--dir PATH] [--files N] [--size KB] [--inflight N] [--conns N] [--chunk KB] [--seconds S] [--tick-hz N] [--budget KB] [--frame-ms N] [--io sync|ring] [--quick] [--keep] [--direct | --compare]\n", argv[0]); return 2; }
    }
    if (o.quick) { o.files = o.files > 32 ? 32 : o.files; o.size_kb = o.size_kb > 256 ? 256 : o.size_kb; }
    if (o.files < 1) o.files = 1;
    if (o.size_kb < 1) o.size_kb = 1;
    if (o.inflight < 1) o.inflight = 1;
    if (o.conns < 1) o.conns = 1;
    if (o.conns > 64) o.conns = 64;
    if (o.chunk_kb < 1) o.chunk_kb = 1;
    if (o.tick_hz < 1.0) o.tick_hz = 1.0;
    if (!make_work_dir()) return 1;
    print("sockit2m: %u files x %u KB = %u MB in %s, %u reads in flight, %u connections, %u KB chunks, %.0f s per socket run%s\n",
           o.files, o.size_kb, o.files * o.size_kb / 1024, g_work_dir, o.inflight, o.conns, o.chunk_kb, o.quick ? 1.0 : o.seconds, o.quick ? ", quick" : "");

    g_base = mem_stats();
    f64 write_mbs = 0.0;
    if (!write_working_set(&write_mbs)) { remove_working_set(); return 1; }
    char what[96], numbers[64];
    format(what, sizeof what, "%u files x %u KB, fwrite then sync", o.files, o.size_kb);
    format(numbers, sizeof numbers, "%8.1f MB/s", write_mbs);
    g_mode = "setup";
    report("fs write", what, numbers, "synchronous; the disk's write number");
    mem_settled("fs write");

    if (!o.direct) run_engine_mode();
    if (o.direct || o.compare) run_direct_mode();

    print("\n%-7s %-16s %-62s %-34s %s\n", "mode", "scenario", "what", "numbers", "notes");
    for (u32 i = 0; i < g_row_count; ++i) {
        const Row& r = g_rows[i];
        print("%-7s %-16s %-62s %-34s %s\n", r.mode, r.name, r.what, r.numbers, r.note);
    }
    if (!o.keep) remove_working_set();
    else print("\nworking set kept in %s\n", g_work_dir);
    MemStats s = mem_stats();
    print("\n%s: %u errors; memory at exit: %llu live allocations, %llu bytes\n", g_errors ? "FAILED" : "ok", g_errors,
           (unsigned long long)s.live_allocs, (unsigned long long)s.live_bytes);
    names_shutdown();
    mem_shutdown();
    return g_errors ? 1 : 0;
}
