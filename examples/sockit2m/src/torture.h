// sockit2m: the jobs, shared by the module's systems and the direct loop. A job
// is stepped: FsJob submits reads up to the depth and reaps what the queue has finished,
// SockJob runs the echo server and the clients one round. Neither blocks; whoever steps
// them decides the cadence (a tick, a frame, a tight loop).
#pragma once
#include "korvid/core/array.h"
#include "korvid/platform/io.h"
#include "korvid/platform/net.h"

namespace kv {

struct Options {
    const char* dir      = nullptr;
    u32  files    = 256;
    u32  size_kb  = 1024;
    u32  inflight = 64;
    u32  conns    = 8;
    u32  chunk_kb = 64;
    f64  seconds  = 3.0;
    f64  tick_hz  = 60.0;
    i64  budget_kb = -1;     // the queue's bytes per pump; -1: the queue's default, 0: unlimited
    f64  frame_ms = 0.0;     // pacing for the idle scenario; 0: one tick's worth
    bool quick    = false;
    bool keep     = false;
    bool direct   = false;  // the tight loop instead of the engine
    bool compare  = false;  // both
    IoBackend io  = IoBackend::Auto; // --io: the queue's backend in both modes (the platform's, in engine mode)
};
extern Options g_opt;
extern char    g_work_dir[512];
extern u32     g_errors; // data that came back wrong, drops, failures: anything that must be zero

// The data pattern: a byte for any (stream, offset), cheap enough to verify at line rate.
inline u8 pattern(u32 stream, u64 off) { return (u8)((off ^ (off >> 7) ^ (off >> 13)) + stream * 29u + 11u); }

// How long one step of a loop took, as a log2 histogram of microseconds.
struct PumpClock {
    u64 counts[32] = {};
    u64 total = 0;
    f64 worst_ms = 0.0, sum_ms = 0.0;
    void add(f64 ms);
    f64  percentile_ms(f64 p) const; // the upper edge of the bucket the percentile falls in
    f64  avg_ms() const { return total ? sum_ms / (f64)total : 0.0; }
};

// The working set on disk.
bool make_work_dir();
void file_path(char* out, usize cap, u32 i);
bool write_working_set(f64* mb_per_s);
bool evict_working_set();
void remove_working_set();

// What the working set needs from the OS, one source per OS (src/posix/, src/win32/).
// A new directory from a template ending "XXXXXX", replaced in place; *err on failure.
bool workset_make_unique_dir(char* path, i32* err);
// The working set's writes on the disk, not only in the cache.
void workset_flush();

// One pass over the working set through a queue with at most `inflight` reads out.
struct FsJob {
    IoQueue*   q     = nullptr; // the platform's, or one of its own
    IoQueue    own;
    bool       owns  = false;
    Allocator* alloc = nullptr; // where the file data lands
    Array<f64> submitted_at;    // by entry slot
    u32  inflight_max = 64;
    IoPriority priority = IoPriority::Normal;
    u32  next = 0, completed = 0, in_flight = 0;
    u64  bytes = 0;
    f64  lat_sum = 0.0, lat_max = 0.0;
    u32  pumps = 0;
    f64  t0 = 0.0, t1 = 0.0;
    bool failed = false;

    // A null queue makes one of its own on `backend`; otherwise `backend` is ignored.
    bool begin(IoQueue* queue, IoBackend backend, Allocator* data_alloc, u32 inflight, IoPriority prio = IoPriority::Normal);
    bool done() const { return completed == g_opt.files; }
    void submit();            // reads up to the depth
    void reap();              // what the queue has finished
    void step() { submit(); q->pump(); ++pumps; reap(); } // the direct loop's round
    f64  seconds() const { return t1 - t0; }
    f64  mb_per_s() const { return seconds() > 0 ? (f64)bytes / 1048576.0 / seconds() : 0.0; }
    void end();
};

// Bulk mode: each client streams its pattern as chunks; the server echoes; the client
// checks the echo byte by byte against where it expects to be in the stream.
// Line mode: each client sends "N\n" with a sequence number and times the echo.
struct SockJob {
    bool lines = false;
    NetListener listener;
    struct Server { NetSocket sock; } servers[64];
    struct Client {
        NetSocket sock;
        u64 sent = 0, acked = 0;  // bulk: bytes; lines: messages
        u64 line_in_progress = 0; // lines: the number being parsed
        bool parsing = false;
        f64 sent_at[4096];        // lines: by sequence & 4095
    } clients[64];
    u32 n = 0, server_count = 0;
    u8* chunk = nullptr;
    Allocator* alloc = nullptr;
    u64 tx = 0, rx = 0, echoed = 0, verified = 0, round_trips = 0;
    f64 rtt_sum = 0.0, rtt_max = 0.0;
    f64 t0 = 0.0, t1 = 0.0, until = 0.0;
    u32 steps = 0;
    bool failed = false;

    bool begin(u32 conns, bool line_mode, f64 seconds, Allocator* a);
    bool done() const;
    void step();
    void stop();   // the clock stops now; what is in flight may still land
    void drain();  // steps until every client has its echoes back (bounded)
    f64  seconds() const { return t1 - t0; }
    void end();

private:
    void client_send(Client& c, u32 id);
    void client_receive(Client& c, u32 id, const u8* data, u32 len);
};

} // namespace kv
