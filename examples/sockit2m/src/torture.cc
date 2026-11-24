// sockit2m: the jobs, the working set, and the module whose systems step the jobs
// inside the engine's tick.
#include "torture.h"
#include "korvid/sockit2m/module.h"
#include "korvid/core/format.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include <errno.h>
#include "korvid/math/scalar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

Options g_opt;
char    g_work_dir[512];
u32     g_errors = 0;

// ---- the pump clock -----------------------------------------------------------------------------

void PumpClock::add(f64 ms) {
    ++total; sum_ms += ms;
    if (ms > worst_ms) worst_ms = ms;
    f64 us = ms * 1e3;
    u32 b = us < 1.0 ? 0 : (u32)kv::ceil(kv::log2(us));
    if (b > 31) b = 31;
    ++counts[b];
}

f64 PumpClock::percentile_ms(f64 p) const {
    u64 want = (u64)kv::ceil(p * (f64)total), seen = 0;
    if (want == 0) want = 1;
    for (u32 b = 0; b < 32; ++b) { seen += counts[b]; if (seen >= want) return (f64)(1ull << b) / 1e3; }
    return worst_ms;
}

// ---- the working set ----------------------------------------------------------------------------
// Setup is plain C I/O: the engine reads, it does not write, and the write number is the
// disk's, not the queue's.

bool make_work_dir() {
    char base[480];
    if (g_opt.dir) format(base, sizeof base, "%s", g_opt.dir);
    else if (platform_exe_dir(base, sizeof base)) {
        usize n = strlen(base);
        if (n > 1 && (base[n - 1] == '/' || base[n - 1] == '\\')) base[n - 1] = 0; // joined with '/' below
    } else {
        format(base, sizeof base, ".");
    }
    format(g_work_dir, sizeof g_work_dir, "%s/sockit2m-XXXXXX", base);
    i32 err = 0;
    if (!workset_make_unique_dir(g_work_dir, &err)) { KV_ERROR("sockit2m", "cannot create a working directory under '%s': %s", base, strerror(err)); return false; }
    return true;
}

void file_path(char* out, usize cap, u32 i) { format(out, cap, "%s/f%04u.bin", g_work_dir, i); }

bool write_working_set(f64* mb_per_s) {
    usize size = (usize)g_opt.size_kb * 1024;
    u8* buf = (u8*)mem_alloc(size);
    f64 t0 = time_now();
    for (u32 i = 0; i < g_opt.files; ++i) {
        for (usize off = 0; off < size; ++off) buf[off] = pattern(i, off);
        char path[600];
        file_path(path, sizeof path, i);
        FILE* f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, size, f) != size) { KV_ERROR("sockit2m", "cannot write '%s': %s", path, strerror(errno)); if (f) fclose(f); mem_free(buf); return false; }
        fclose(f);
    }
    workset_flush(); // what the disk took, not what the page cache took
    f64 s = time_now() - t0;
    mem_free(buf);
    *mb_per_s = (f64)g_opt.files * (f64)size / 1048576.0 / s;
    return true;
}

// ---- files -----------------------------------------------------------------------------------------

bool FsJob::begin(IoQueue* queue, IoBackend backend, Allocator* data_alloc, u32 inflight, IoPriority prio) {
    if (queue) { q = queue; owns = false; }
    else {
        if (!own.init(nullptr, g_opt.files + 16, backend)) return false;
        q = &own; owns = true;
    }
    if (g_opt.budget_kb >= 0) q->budget.bytes_per_pump = (usize)g_opt.budget_kb * 1024;
    alloc = data_alloc;
    inflight_max = inflight;
    priority = prio;
    submitted_at.resize_fill(q->entries.max_items, 0.0);
    t0 = time_now();
    return true;
}

void FsJob::submit() {
    while (next < g_opt.files && in_flight < inflight_max) {
        char path[600];
        file_path(path, sizeof path, next);
        IoRequest r; r.path = path; r.user = next; r.alloc = alloc; r.priority = priority;
        Handle h = q->read_file(r);
        if (h.is_null()) { failed = true; ++g_errors; return; }
        submitted_at[h.index()] = time_now();
        ++next; ++in_flight;
    }
}

void FsJob::reap() {
    f64 now = time_now();
    usize size = (usize)g_opt.size_kb * 1024;
    Span<const IoCompletion> cs = q->completions();
    for (u32 i = 0; i < cs.len;) {
        IoCompletion c = cs[i];
        if (c.user >= g_opt.files || submitted_at[c.request.index()] == 0.0) { ++i; continue; } // not ours
        if (c.status != IoStatus::Done || c.bytes.size != size) {
            KV_ERROR("sockit2m", "file %llu: status %u size %llu", (unsigned long long)c.user, (u32)c.status, (unsigned long long)c.bytes.size);
            failed = true; ++g_errors;
        } else {
            const u8* d = (const u8*)c.bytes.data;
            u32 stream = (u32)c.user;
            for (usize off = 0; off < size; off += 4093) if (d[off] != pattern(stream, off)) { ++g_errors; failed = true; break; }
            if (d[size - 1] != pattern(stream, size - 1)) { ++g_errors; failed = true; }
            bytes += size;
        }
        f64 lat = now - submitted_at[c.request.index()];
        lat_sum += lat; if (lat > lat_max) lat_max = lat;
        submitted_at[c.request.index()] = 0.0;
        c.bytes.free();
        q->reap(c.request);
        ++completed; --in_flight;
        cs = q->completions();
    }
    if (done() && t1 == 0.0) t1 = time_now();
}

void FsJob::end() {
    if (owns) own.free();
    submitted_at.free();
}

// ---- sockets ------------------------------------------------------------------------------------------

constexpr u32 kServerBuf = 64 * 1024;
constexpr u32 kOutLimit  = 1024 * 1024; // queued bytes a socket may hold before writes are refused
constexpr u32 kFlowMark  = 256 * 1024;  // a client stops writing above this much queued

bool SockJob::begin(u32 conns, bool line_mode, f64 seconds, Allocator* a) {
    lines = line_mode;
    alloc = a;
    n = conns > 64 ? 64 : conns;
    if (!listener.open(0)) return false;
    chunk = (u8*)alloc_in(alloc ? alloc : mem_general(), g_opt.chunk_kb * 1024);
    for (u32 i = 0; i < n; ++i) {
        clients[i] = Client{};
        clients[i].sock.out.alloc = alloc;
        clients[i].sock.out_limit = kOutLimit;
        if (!net_connect_local(listener.port, clients[i].sock)) return false;
    }
    // Accept them all before the clock starts.
    f64 deadline = time_now() + 2.0;
    while (server_count < n && time_now() < deadline) {
        KHandle fd = listener.accept();
        if (!fd.is_open()) { time_sleep(0.0005); continue; }
        servers[server_count].sock.out.alloc = alloc;
        servers[server_count].sock.out_limit = kOutLimit;
        servers[server_count++].sock.adopt(fd);
    }
    if (server_count < n) { KV_ERROR("sockit2m", "only %u of %u connections accepted", server_count, n); return false; }
    t0 = time_now();
    until = t0 + seconds;
    return true;
}

bool SockJob::done() const { return time_now() >= until; }
void SockJob::stop() { until = time_now(); }

void SockJob::client_send(Client& c, u32 id) {
    if (lines) {
        // Keep up to 2048 lines outstanding; the ring of send times is twice that.
        char buf[16];
        while (c.sent - c.acked < 2048 && c.sock.out.len < kFlowMark) {
            int len = format(buf, sizeof buf, "%llu\n", (unsigned long long)c.sent);
            if (!c.sock.write(buf, (u32)len)) break;
            c.sent_at[c.sent & 4095] = time_now();
            ++c.sent; tx += (u64)len;
        }
        return;
    }
    u32 size = g_opt.chunk_kb * 1024;
    while (c.sock.out.len < kFlowMark) {
        for (u32 i = 0; i < size; ++i) chunk[i] = pattern(id, c.sent + i);
        if (!c.sock.write(chunk, size)) break;
        c.sent += size; tx += size;
    }
}

void SockJob::client_receive(Client& c, u32 id, const u8* data, u32 len) {
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
    for (u32 i = 0; i < len; ++i)
        if (data[i] != pattern(id, c.acked + i)) { ++g_errors; failed = true; break; }
    c.acked += len; verified += len;
}

void SockJob::step() {
    u8 buf[kServerBuf];
    ++steps;
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
    bool sending = !done();
    for (u32 i = 0; i < n; ++i) {
        Client& c = clients[i];
        if (sending) client_send(c, i);
        for (;;) {
            i32 got = c.sock.read(buf, sizeof buf);
            if (got <= 0) break;
            client_receive(c, i, buf, (u32)got);
        }
        c.sock.flush();
    }
    if (!sending && t1 == 0.0) t1 = time_now();
}

void SockJob::drain() {
    if (t1 == 0.0) t1 = time_now(); // the clock stops here if no round saw the stop
    f64 deadline = time_now() + 1.0;
    for (;;) {
        bool quiet = true;
        for (u32 i = 0; i < n; ++i) if (clients[i].acked != clients[i].sent) quiet = false;
        if (quiet || time_now() > deadline) break;
        step();
    }
}

void SockJob::end() {
    u64 drops = 0;
    for (u32 i = 0; i < n; ++i) { drops += clients[i].sock.dropped; clients[i].sock.close(); clients[i].sock.out.free(); }
    for (u32 i = 0; i < server_count; ++i) { drops += servers[i].sock.dropped; servers[i].sock.close(); servers[i].sock.out.free(); }
    if (drops) { KV_ERROR("sockit2m", "%llu bytes dropped at the queues", (unsigned long long)drops); ++g_errors; }
    listener.close();
    free_in(alloc ? alloc : mem_general(), chunk, g_opt.chunk_kb * 1024);
    chunk = nullptr;
}

// ---- the module -----------------------------------------------------------------------------------------

namespace {

struct Sockit2m {
    FsJob*     fs   = nullptr;
    SockJob*   sock = nullptr;
    Allocator* general = nullptr;
    u64        ticks = 0;
};

bool sockit2m_startup(Engine&, ModuleInstance& self) {
    Sockit2m* s = arena_push<Sockit2m>(self.arena);
    if (!s) return false;
    s->general = &self.general;
    self.set_state(s);
    return true;
}

// Phase Input: the sockets, one round per tick, the way a CLI or a game's net code runs.
void sockit2m_sockets(TickContext& ctx) {
    Sockit2m& s = ctx.state_as<Sockit2m>();
    ++s.ticks;
    if (s.sock) s.sock->step();
}

// Phase PreSim, after the platform pumped the queue this frame: reap what landed and
// keep the depth full, the way asset.pump does.
void sockit2m_files(TickContext& ctx) {
    Sockit2m& s = ctx.state_as<Sockit2m>();
    if (!s.fs) return;
    s.fs->reap();
    s.fs->submit();
}

constinit const SystemDesc k_systems[] = {
    {.name = "sockit2m.sockets", .phase = Phase::Input,  .fn = sockit2m_sockets, .main_thread = true},
    {.name = "sockit2m.files",   .phase = Phase::PreSim, .fn = sockit2m_files,   .main_thread = true},
};

} // namespace

KV_MODULE_DEFINE(sockit2m_module) = {
    .name    = "sockit2m",
    .systems = k_systems,
    .startup = sockit2m_startup,
    .memory  = {.reserve = 64 * 1024},
};

void sockit2m_set_jobs(Engine& e, FsJob* fs, SockJob* sock) {
    Sockit2m* s = engine_module_state_as<Sockit2m>(e, "sockit2m");
    if (!s) return;
    s->fs = fs;
    s->sock = sock;
}

Allocator* sockit2m_allocator(Engine& e) {
    Sockit2m* s = engine_module_state_as<Sockit2m>(e, "sockit2m");
    return s ? s->general : mem_general();
}

} // namespace kv
