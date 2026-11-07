// example_sockit2m: sock it to me. An I/O torture test on one thread, the way the engine
// does I/O: nothing blocks, everything is pumped. It writes a working set of files, reads
// it back through the I/O queue on io_uring and on the synchronous path with a bounded
// number of requests in flight, blasts loopback TCP through the non-blocking sockets with
// an echo server on the same thread (bulk chunks, then CLI-sized lines), and then does
// the file reads and the socket echo at the same time. Every byte that comes back is
// checked against the pattern that went out. The numbers: throughput, request latency,
// message rate and round trip, and how long one pump of the loop takes at worst and at
// the 99th percentile, which is the number that says whether the sim would have hitched.
//
// What it shows on a hot page cache: the kernel completes cached reads inside the
// io_uring enter, so a pump with 64 MB in flight is a 30 ms pump and the sockets starve;
// the bounded run caps the depth at 8 and the pump comes back down. Cold reads meet the
// disk and complete asynchronously, which is the case the queue is built for.
//
//   example_sockit2m [--dir PATH] [--files N] [--size KB] [--inflight N] [--conns N]
//                    [--chunk KB] [--seconds S] [--quick] [--keep]
//
// The working set goes next to the executable unless --dir says otherwise (/tmp is often
// a tmpfs, which would measure memory, not a disk). Files are left behind with --keep.
#include "korvid/core/array.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/net.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace kv;

namespace {

// ---- options ---------------------------------------------------------------------------------
const char* g_dir      = nullptr;
u32  g_files    = 256;
u32  g_size_kb  = 1024;
u32  g_inflight = 64;
u32  g_conns    = 8;
u32  g_chunk_kb = 64;
f64  g_seconds  = 3.0;
bool g_quick    = false;
bool g_keep     = false;

char g_work_dir[512];
u32  g_errors = 0; // data that came back wrong, drops, failures: anything that must be zero

// ---- the report ---------------------------------------------------------------------------------
struct Row { char name[24]; char what[56]; char numbers[64]; char note[200]; };
Row g_rows[16];
u32 g_row_count = 0;

void report(const char* name, const char* what, const char* numbers, const char* note) {
    if (g_row_count >= KV_COUNTOF(g_rows)) return;
    Row& r = g_rows[g_row_count++];
    snprintf(r.name, sizeof r.name, "%s", name);
    snprintf(r.what, sizeof r.what, "%s", what);
    snprintf(r.numbers, sizeof r.numbers, "%s", numbers);
    snprintf(r.note, sizeof r.note, "%s", note);
}

// ---- pump timing: how long one step of the loop took, as a log2 histogram of microseconds ----
struct PumpClock {
    u64 counts[32] = {};
    u64 total = 0;
    f64 worst_ms = 0.0;
    f64 sum_ms = 0.0;

    void add(f64 ms) {
        ++total; sum_ms += ms;
        if (ms > worst_ms) worst_ms = ms;
        f64 us = ms * 1e3;
        u32 b = us < 1.0 ? 0 : (u32)ceil(log2(us));
        if (b > 31) b = 31;
        ++counts[b];
    }
    // The upper edge of the bucket the p-th percentile falls in, in ms.
    f64 percentile_ms(f64 p) const {
        u64 want = (u64)ceil(p * (f64)total), seen = 0;
        if (want == 0) want = 1;
        for (u32 b = 0; b < 32; ++b) { seen += counts[b]; if (seen >= want) return (f64)(1ull << b) / 1e3; }
        return worst_ms;
    }
    f64 avg_ms() const { return total ? sum_ms / (f64)total : 0.0; }
};

// ---- the data pattern: a byte for any (stream, offset), cheap enough to verify at line rate ----
inline u8 pattern(u32 stream, u64 off) { return (u8)((off ^ (off >> 7) ^ (off >> 13)) + stream * 29u + 11u); }

// ---- files -----------------------------------------------------------------------------------------

void file_path(char* out, usize cap, u32 i) { snprintf(out, cap, "%s/f%04u.bin", g_work_dir, i); }

// Writes the working set synchronously (this is setup, and the disk's write number).
bool write_working_set(f64* mb_per_s) {
    usize size = (usize)g_size_kb * 1024;
    u8* buf = (u8*)mem_alloc(size);
    f64 t0 = time_now();
    for (u32 i = 0; i < g_files; ++i) {
        for (usize off = 0; off < size; ++off) buf[off] = pattern(i, off);
        char path[600];
        file_path(path, sizeof path, i);
        FILE* f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, size, f) != size) { KV_ERROR("sockit2m", "cannot write '%s': %s", path, strerror(errno)); if (f) fclose(f); mem_free(buf); return false; }
        fclose(f);
    }
    // What the disk took, not what the page cache took: sync and count it.
    sync();
    f64 s = time_now() - t0;
    mem_free(buf);
    *mb_per_s = (f64)g_files * (f64)size / 1048576.0 / s;
    return true;
}

// Drops the working set from the page cache (clean pages after sync; no root needed), so
// the next read pass meets the disk rather than memory.
bool evict_working_set() {
    bool ok = true;
    for (u32 i = 0; i < g_files; ++i) {
        char path[600];
        file_path(path, sizeof path, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) { ok = false; continue; }
        fdatasync(fd);
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) ok = false;
        close(fd);
    }
    return ok;
}

void remove_working_set() {
    for (u32 i = 0; i < g_files; ++i) { char path[600]; file_path(path, sizeof path, i); unlink(path); }
    rmdir(g_work_dir);
}

// One pass over the working set through the queue with at most `inflight` reads out.
// Stepped from a loop so it can share the thread with the sockets.
struct FsJob {
    IoQueue q;
    Array<f64> submitted_at; // by entry slot
    u32  next = 0, completed = 0, in_flight = 0;
    u64  bytes = 0, verified = 0;
    f64  lat_sum = 0.0, lat_max = 0.0;
    u32  pumps = 0;
    f64  t0 = 0.0, t1 = 0.0;
    bool failed = false;

    bool begin(IoBackend backend) {
        if (!q.init(nullptr, g_files + 16, backend)) return false;
        submitted_at.resize_fill(g_files + 16, 0.0);
        t0 = time_now();
        return true;
    }
    bool done() const { return completed == g_files; }
    void step() {
        while (next < g_files && in_flight < g_inflight) {
            char path[600];
            file_path(path, sizeof path, next);
            IoRequest r; r.path = path; r.user = next;
            Handle h = q.read_file(r);
            if (h.is_null()) { failed = true; ++g_errors; return; }
            submitted_at[h.index()] = time_now();
            ++next; ++in_flight;
        }
        q.pump();
        ++pumps;
        f64 now = time_now();
        Span<const IoCompletion> cs = q.completions();
        for (u32 i = 0; i < cs.len;) {
            IoCompletion c = cs[i];
            usize size = (usize)g_size_kb * 1024;
            if (c.status != IoStatus::Done || c.size != size) { KV_ERROR("sockit2m", "file %llu: status %u size %llu", (unsigned long long)c.user, (u32)c.status, (unsigned long long)c.size); failed = true; ++g_errors; }
            else {
                const u8* d = (const u8*)c.data;
                u32 stream = (u32)c.user;
                for (usize off = 0; off < size; off += 4093) if (d[off] != pattern(stream, off)) { ++g_errors; failed = true; break; }
                d = d + size - 1; if (*d != pattern(stream, size - 1)) { ++g_errors; failed = true; }
                bytes += size; verified += size / 4093 + 1;
            }
            f64 lat = now - submitted_at[c.request.index()];
            lat_sum += lat; if (lat > lat_max) lat_max = lat;
            if (c.data) mem_free(c.data);
            q.reap(c.request);
            ++completed; --in_flight;
            cs = q.completions();
        }
        if (done()) t1 = time_now();
    }
    f64 seconds() const { return t1 - t0; }
    f64 mb_per_s() const { return (f64)bytes / 1048576.0 / seconds(); }
    void end() { q.free(); submitted_at.free(); }
};

// ---- sockets ------------------------------------------------------------------------------------------

constexpr u32 kServerBuf = 64 * 1024;
constexpr u32 kOutLimit  = 1024 * 1024; // queued bytes a socket may hold before writes are refused
constexpr u32 kFlowMark  = 256 * 1024;  // a client stops writing above this much queued

// Bulk mode: each client streams its pattern as chunks; the server echoes; the client
// checks the echo byte by byte against where it expects to be in the stream.
// Line mode: each client sends "N\n" with a sequence number and times the echo.
struct SockJob {
    bool lines = false;
    NetListener listener;
    struct Server { NetSocket sock; } servers[64];
    struct Client {
        NetSocket sock;
        u64 sent = 0, acked = 0;      // bulk: bytes; lines: messages
        u64 line_in_progress = 0;     // lines: the number being parsed
        bool parsing = false;
        f64 sent_at[4096];            // lines: by sequence & 4095
    } clients[64];
    u32 n = 0, server_count = 0;
    u8* chunk = nullptr;
    u64 tx = 0, rx = 0, echoed = 0, verified = 0, round_trips = 0;
    f64 rtt_sum = 0.0, rtt_max = 0.0;
    f64 t0 = 0.0, t1 = 0.0, until = 0.0;
    bool failed = false;

    bool begin(u32 conns, bool line_mode, f64 seconds) {
        lines = line_mode;
        n = conns > 64 ? 64 : conns;
        if (!listener.open(0)) return false;
        chunk = (u8*)mem_alloc(g_chunk_kb * 1024);
        for (u32 i = 0; i < n; ++i) {
            clients[i] = Client{};
            clients[i].sock.out_limit = kOutLimit;
            if (!net_connect_local(listener.port, clients[i].sock)) return false;
        }
        // Accept them all before the clock starts.
        f64 deadline = time_now() + 2.0;
        while (server_count < n && time_now() < deadline) {
            int fd = listener.accept();
            if (fd < 0) { time_sleep(0.0005); continue; }
            servers[server_count].sock.out_limit = kOutLimit;
            servers[server_count++].sock.adopt(fd);
        }
        if (server_count < n) { KV_ERROR("sockit2m", "only %u of %u connections accepted", server_count, n); return false; }
        t0 = time_now();
        until = t0 + seconds;
        return true;
    }
    bool done() const { return time_now() >= until; }

    void client_send(Client& c, u32 id) {
        if (lines) {
            // Keep up to 2048 lines outstanding; the ring of send times is twice that.
            char buf[16];
            while (c.sent - c.acked < 2048 && c.sock.out.len < kFlowMark) {
                int len = snprintf(buf, sizeof buf, "%llu\n", (unsigned long long)c.sent);
                if (!c.sock.write(buf, (u32)len)) break;
                c.sent_at[c.sent & 4095] = time_now();
                ++c.sent; tx += (u64)len;
            }
            return;
        }
        u32 size = g_chunk_kb * 1024;
        while (c.sock.out.len < kFlowMark) {
            for (u32 i = 0; i < size; ++i) chunk[i] = pattern(id, c.sent + i);
            if (!c.sock.write(chunk, size)) break;
            c.sent += size; tx += size;
        }
    }
    void client_receive(Client& c, u32 id, const u8* data, u32 len) {
        rx += len;
        if (lines) {
            for (u32 i = 0; i < len; ++i) {
                char ch = (char)data[i];
                if (ch >= '0' && ch <= '9') { c.line_in_progress = c.line_in_progress * 10 + (u64)(ch - '0'); c.parsing = true; continue; }
                if (ch != '\n') { ++g_errors; failed = true; continue; }
                if (!c.parsing || c.line_in_progress != c.acked) { ++g_errors; failed = true; }
                f64 rtt = time_now() - c.sent_at[c.line_in_progress & 4095];
                rtt_sum += rtt; if (rtt > rtt_max) rtt_max = rtt;
                ++round_trips; ++c.acked;
                c.line_in_progress = 0; c.parsing = false;
            }
            return;
        }
        for (u32 i = 0; i < len; ++i) {
            if (data[i] != pattern(id, c.acked + i)) { ++g_errors; failed = true; break; }
        }
        c.acked += len; verified += len;
    }

    void step() {
        u8 buf[kServerBuf];
        for (u32 i = 0; i < server_count; ++i) {
            NetSocket& s = servers[i].sock;
            // Backpressure: do not read more than can be queued back.
            while (s.out.len < kFlowMark) {
                i32 got = s.read(buf, sizeof buf);
                if (got <= 0) break;
                if (!s.write(buf, (u32)got)) { ++g_errors; failed = true; }
                echoed += (u64)got;
            }
            s.flush();
        }
        for (u32 i = 0; i < n; ++i) {
            Client& c = clients[i];
            if (!done()) client_send(c, i);
            for (;;) {
                i32 got = c.sock.read(buf, sizeof buf);
                if (got <= 0) break;
                client_receive(c, i, buf, (u32)got);
            }
            c.sock.flush();
        }
        if (done() && t1 == 0.0) t1 = time_now();
    }
    // After the clock: let what is in flight land (bounded), so tx and rx agree.
    void drain() {
        f64 deadline = time_now() + 1.0;
        for (;;) {
            bool quiet = true;
            for (u32 i = 0; i < n; ++i) if (clients[i].acked != clients[i].sent) quiet = false;
            if (quiet || time_now() > deadline) break;
            step();
        }
    }
    f64 seconds() const { return t1 - t0; }
    void end() {
        u64 drops = 0;
        for (u32 i = 0; i < n; ++i) { drops += clients[i].sock.dropped; clients[i].sock.close(); clients[i].sock.out.free(); }
        for (u32 i = 0; i < server_count; ++i) { drops += servers[i].sock.dropped; servers[i].sock.close(); servers[i].sock.out.free(); }
        if (drops) { KV_ERROR("sockit2m", "%llu bytes dropped at the queues", (unsigned long long)drops); ++g_errors; }
        listener.close();
        mem_free(chunk);
    }
};

// ---- scenarios --------------------------------------------------------------------------------------------

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

f64 g_fs_solo_mbs = 0.0, g_sock_solo_mbs = 0.0;

void scenario_fs_read(IoBackend backend, const char* name, bool cold) {
    FsJob fs;
    PumpClock clock;
    if (cold && !evict_working_set()) KV_WARN("sockit2m", "could not evict the working set; the cold pass may be warm");
    if (!fs.begin(backend)) { report(name, "could not start", "", "FAILED"); ++g_errors; return; }
    while (!fs.done()) { f64 t = time_now(); fs.step(); clock.add((time_now() - t) * 1e3); }
    char what[64], numbers[64], note[200];
    snprintf(what, sizeof what, "%u MB in %u files, %u in flight, %s, %s", g_files * g_size_kb / 1024, g_files, g_inflight, fs.q.backend_name(), cold ? "cold cache" : "hot cache");
    snprintf(numbers, sizeof numbers, "%8.1f MB/s %8.0f files/s", fs.mb_per_s(), (f64)g_files / fs.seconds());
    snprintf(note, sizeof note, "latency avg %.2f ms, max %.2f ms; %u pumps, pump avg %.3f ms, p99 <= %.3f ms, worst %.2f ms%s",
             fs.lat_sum / g_files * 1e3, fs.lat_max * 1e3, fs.pumps, clock.avg_ms(), clock.percentile_ms(0.99), clock.worst_ms,
             fs.failed ? "; DATA ERRORS" : "");
    report(name, what, numbers, note);
    if (backend == IoBackend::Auto && !cold) g_fs_solo_mbs = fs.mb_per_s();
    fs.end();
    mem_settled(name);
}

void scenario_sockets(bool lines, const char* name) {
    SockJob sj;
    PumpClock clock;
    f64 seconds = g_quick ? 1.0 : g_seconds;
    u32 conns = lines ? (g_conns < 4 ? g_conns : 4) : g_conns;
    if (!sj.begin(conns, lines, seconds)) { report(name, "could not start", "", "FAILED"); ++g_errors; sj.end(); return; }
    while (!sj.done()) { f64 t = time_now(); sj.step(); clock.add((time_now() - t) * 1e3); }
    sj.drain();
    char what[64], numbers[64], note[200];
    f64 s = sj.seconds();
    if (lines) {
        snprintf(what, sizeof what, "%u conns, \"N\\n\" lines echoed, %.0f s", conns, seconds);
        snprintf(numbers, sizeof numbers, "%8.0f lines/s %7.1f MB/s", (f64)sj.round_trips / s, (f64)(sj.tx + sj.rx) / 1048576.0 / s);
        snprintf(note, sizeof note, "%llu round trips, rtt avg %.3f ms, max %.2f ms; pump avg %.3f ms, p99 <= %.3f ms, worst %.2f ms%s",
                 (unsigned long long)sj.round_trips, sj.round_trips ? sj.rtt_sum / (f64)sj.round_trips * 1e3 : 0.0, sj.rtt_max * 1e3,
                 clock.avg_ms(), clock.percentile_ms(0.99), clock.worst_ms, sj.failed ? "; DATA ERRORS" : "");
    } else {
        snprintf(what, sizeof what, "%u conns x %u KB chunks echoed, %.0f s", conns, g_chunk_kb, seconds);
        snprintf(numbers, sizeof numbers, "%8.1f MB/s out %7.1f MB/s back", (f64)sj.tx / 1048576.0 / s, (f64)sj.rx / 1048576.0 / s);
        snprintf(note, sizeof note, "%llu MB verified byte for byte, server echoed %llu MB; pump avg %.3f ms, p99 <= %.3f ms, worst %.2f ms%s",
                 (unsigned long long)(sj.verified / 1048576), (unsigned long long)(sj.echoed / 1048576),
                 clock.avg_ms(), clock.percentile_ms(0.99), clock.worst_ms, sj.failed ? "; DATA ERRORS" : "");
        g_sock_solo_mbs = (f64)sj.rx / 1048576.0 / s;
    }
    report(name, what, numbers, note);
    sj.end();
    mem_settled(name);
}

// Files and sockets in one loop, for as long as the file pass takes.
void scenario_both(u32 inflight, const char* name) {
    FsJob fs;
    SockJob sj;
    PumpClock clock;
    u32 saved = g_inflight;
    g_inflight = inflight;
    if (!fs.begin(IoBackend::Auto) || !sj.begin(g_conns, false, 3600.0)) { report(name, "could not start", "", "FAILED"); ++g_errors; fs.end(); sj.end(); g_inflight = saved; return; }
    while (!fs.done()) {
        f64 t = time_now();
        fs.step();
        sj.step();
        clock.add((time_now() - t) * 1e3);
    }
    sj.until = time_now(); // the sockets stop when the files are done
    sj.step();
    sj.drain();
    f64 s = sj.seconds();
    f64 fs_mbs = fs.mb_per_s(), sock_mbs = (f64)sj.rx / 1048576.0 / s;
    char what[64], numbers[64], note[200];
    snprintf(what, sizeof what, "file reads (%s, %u in flight) + %u echo conns", fs.q.backend_name(), inflight, g_conns);
    snprintf(numbers, sizeof numbers, "%8.1f MB/s files %6.1f MB/s sock", fs_mbs, sock_mbs);
    snprintf(note, sizeof note, "files at %.0f%% of solo, sockets at %.0f%% of solo; file latency avg %.2f ms, max %.2f ms; pump avg %.3f ms, p99 <= %.3f ms, worst %.2f ms%s",
             g_fs_solo_mbs > 0 ? 100.0 * fs_mbs / g_fs_solo_mbs : 0.0, g_sock_solo_mbs > 0 ? 100.0 * sock_mbs / g_sock_solo_mbs : 0.0,
             fs.lat_sum / g_files * 1e3, fs.lat_max * 1e3, clock.avg_ms(), clock.percentile_ms(0.99), clock.worst_ms,
             fs.failed || sj.failed ? "; DATA ERRORS" : "");
    report(name, what, numbers, note);
    fs.end();
    sj.end();
    mem_settled(name);
    g_inflight = saved;
}

bool make_work_dir() {
    char base[480];
    if (g_dir) snprintf(base, sizeof base, "%s", g_dir);
    else {
        ssize_t n = readlink("/proc/self/exe", base, sizeof base - 1);
        if (n <= 0) snprintf(base, sizeof base, ".");
        else { base[n] = 0; char* slash = strrchr(base, '/'); if (slash) *slash = 0; }
    }
    snprintf(g_work_dir, sizeof g_work_dir, "%s/sockit2m-XXXXXX", base);
    if (!mkdtemp(g_work_dir)) { KV_ERROR("sockit2m", "cannot create a working directory under '%s': %s", base, strerror(errno)); return false; }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) g_dir = argv[++i];
        else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) g_files = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) g_size_kb = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--inflight") == 0 && i + 1 < argc) g_inflight = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--conns") == 0 && i + 1 < argc) g_conns = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) g_chunk_kb = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) g_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (strcmp(argv[i], "--keep") == 0) g_keep = true;
        else { fprintf(stderr, "usage: %s [--dir PATH] [--files N] [--size KB] [--inflight N] [--conns N] [--chunk KB] [--seconds S] [--quick] [--keep]\n", argv[0]); return 2; }
    }
    if (g_quick) { g_files = g_files > 32 ? 32 : g_files; g_size_kb = g_size_kb > 256 ? 256 : g_size_kb; }
    if (g_files < 1) g_files = 1;
    if (g_size_kb < 1) g_size_kb = 1;
    if (g_inflight < 1) g_inflight = 1;
    if (g_conns < 1) g_conns = 1;
    if (g_conns > 64) g_conns = 64;
    if (g_chunk_kb < 1) g_chunk_kb = 1;
    if (!make_work_dir()) return 1;
    printf("sockit2m: %u files x %u KB = %u MB in %s, %u reads in flight, %u connections, %u KB chunks, %.0f s per socket run%s\n",
           g_files, g_size_kb, g_files * g_size_kb / 1024, g_work_dir, g_inflight, g_conns, g_chunk_kb, g_quick ? 1.0 : g_seconds, g_quick ? ", quick" : "");

    g_base = mem_stats();
    f64 write_mbs = 0.0;
    if (!write_working_set(&write_mbs)) { remove_working_set(); return 1; }
    char what[64], numbers[64];
    snprintf(what, sizeof what, "%u files x %u KB, fwrite then sync", g_files, g_size_kb);
    snprintf(numbers, sizeof numbers, "%8.1f MB/s", write_mbs);
    report("fs write", what, numbers, "setup, synchronous; the disk's write number");
    mem_settled("fs write");

    scenario_fs_read(IoBackend::Auto, "fs read cold", true);
    scenario_fs_read(IoBackend::Auto, "fs read hot", false);
    scenario_fs_read(IoBackend::Sync, "fs read sync", false);
    scenario_sockets(false, "sockets bulk");
    scenario_sockets(true, "sockets lines");
    scenario_both(g_inflight, "both");
    if (g_inflight > 8) scenario_both(8, "both, bounded"); // what a sim that must not hitch would ask for

    printf("\n%-14s %-52s %-34s %s\n", "scenario", "what", "numbers", "notes");
    for (u32 i = 0; i < g_row_count; ++i) {
        const Row& r = g_rows[i];
        printf("%-14s %-52s %-34s %s\n", r.name, r.what, r.numbers, r.note);
    }
    if (!g_keep) remove_working_set();
    else printf("\nworking set kept in %s\n", g_work_dir);
    MemStats s = mem_stats();
    printf("\n%s: %u errors; memory at exit: %llu live allocations, %llu bytes\n", g_errors ? "FAILED" : "ok", g_errors,
           (unsigned long long)s.live_allocs, (unsigned long long)s.live_bytes);
    names_shutdown();
    mem_shutdown();
    return g_errors ? 1 : 0;
}
