#include "korvid/platform/io.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "io_ring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kv {

namespace {

constexpr u32 kRingEntries = 256;

void* io_resolve(kv_handle h, void* ctx) { return ((IoQueue*)ctx)->entries.get(Handle{h}); }

int io_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    IoQueue& q = *(IoQueue*)ctx;
    IoEntry* e = q.entries.get(Handle{h});
    if (!e) return snprintf(out, cap, "stale");
    const char* st = e->status == IoStatus::Pending ? (e->in_flight ? "in flight" : "pending") :
                     e->status == IoStatus::Done ? "done" : e->status == IoStatus::Failed ? "failed" : "cancelled";
    return snprintf(out, cap, "%s '%s'", st, e->path);
}

// Higher priority first, then submission order.
int compare_queued(const void* a, const void* b, void* ctx) {
    IoQueue& q = *(IoQueue*)ctx;
    const IoEntry* x = q.entries.get(*(const Handle*)a);
    const IoEntry* y = q.entries.get(*(const Handle*)b);
    if (!x || !y) return 0;
    if (x->priority != y->priority) return (u8)x->priority > (u8)y->priority ? -1 : 1;
    return x->sequence < y->sequence ? -1 : (x->sequence > y->sequence ? 1 : 0);
}

Allocator* entry_alloc(const IoEntry& e) { return e.alloc ? e.alloc : mem_general(); }

void complete_failed(IoQueue& q, Handle h, IoEntry& e, IoCompletion& out, i32 err) {
    out.request = h; out.user = e.user; out.status = IoStatus::Failed; out.error = err;
    e.status = IoStatus::Failed;
    if (e.write) ++q.writes_failed; else ++q.reads_failed;
}

// A write's copy goes once the write is over, whatever happened to it.
void drop_write_copy(IoEntry& e) {
    if (e.write && e.buf) { free_in(entry_alloc(e), e.buf, e.size ? e.size : 1); e.buf = nullptr; }
}

// ---- the synchronous backend: one read or write, now -----------------------------------------
void perform_sync(IoQueue& q, Handle h, IoCompletion& out) {
    IoEntry* e = q.entries.get(h);
    out.request = h;
    out.user    = e->user;
    if (e->status == IoStatus::Cancelled) { out.status = IoStatus::Cancelled; drop_write_copy(*e); return; }

    if (e->write) {
        FILE* f = fopen(e->path, "wb");
        if (!f) { complete_failed(q, h, *e, out, errno); drop_write_copy(*e); return; }
        bool ok = e->size == 0 || fwrite(e->buf, 1, e->size, f) == e->size;
        ok = (fclose(f) == 0) && ok;
        if (!ok) { complete_failed(q, h, *e, out, EIO); drop_write_copy(*e); return; }
        out.status = IoStatus::Done;
        out.size   = e->size;
        e->status  = IoStatus::Done;
        ++q.writes_done;
        drop_write_copy(*e);
        return;
    }

    FILE* f = fopen(e->path, "rb");
    if (!f) { complete_failed(q, h, *e, out, errno); return; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { int err = errno; fclose(f); complete_failed(q, h, *e, out, err); return; }

    Allocator* a = entry_alloc(*e);
    void* data = alloc_in(a, (usize)len ? (usize)len : 1);
    bool ok = data && (len == 0 || fread(data, 1, (usize)len, f) == (usize)len);
    fclose(f);
    if (!ok) {
        free_in(a, data, (usize)len ? (usize)len : 1);
        complete_failed(q, h, *e, out, EIO);
        return;
    }
    out.status = IoStatus::Done;
    out.data   = data;
    out.size   = (usize)len;
    e->status  = IoStatus::Done;
    ++q.reads_done;
}

// ---- the ring backend --------------------------------------------------------------------------

// Stage one: opens and sizes the file, so the budget can decide. False (with a completion
// pushed) when the file cannot be opened.
bool ring_open(IoQueue& q, Handle h, IoEntry& e) {
    if (e.fd >= 0) return true;
    if (e.write) {
        int fd = open(e.path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) { IoCompletion c{}; complete_failed(q, h, e, c, errno); drop_write_copy(e); q.completed.push(c); return false; }
        e.fd = fd;
        e.done = 0;
        return true;
    }
    int fd = open(e.path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { IoCompletion c{}; complete_failed(q, h, e, c, errno); q.completed.push(c); return false; }
    struct stat st;
    if (fstat(fd, &st) != 0) { int err = errno; close(fd); IoCompletion c{}; complete_failed(q, h, e, c, err); q.completed.push(c); return false; }
    e.fd   = fd;
    e.size = (usize)st.st_size;
    e.done = 0;
    return true;
}

// Stage two: the buffer and the first read, or the first write of the copy.
bool ring_start(IoQueue& q, Handle h, IoEntry& e) {
    if (!e.write) {
        e.buf = (u8*)alloc_in(entry_alloc(e), e.size ? e.size : 1);
        if (!e.buf) { close(e.fd); e.fd = -1; IoCompletion c{}; complete_failed(q, h, e, c, ENOMEM); q.completed.push(c); return false; }
    }
    e.in_flight = true;
    ++q.in_flight;
    q.bytes_in_flight += e.size;
    q.bytes_submitted += e.size;
    // An empty file: nothing to move; the ring still answers with 0 bytes, which is Done.
    u32 len = e.size > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)e.size;
    if (e.write) io_ring_push_write(q.ring, e.fd, e.buf, len, 0, h.bits);
    else io_ring_push_read(q.ring, e.fd, e.buf, len, 0, h.bits);
    return true;
}

void ring_finish(IoQueue& q, Handle h, IoEntry& e, IoStatus status, i32 err, usize size) {
    IoCompletion c{};
    c.request = h; c.user = e.user; c.status = status; c.error = err;
    if (e.write) {
        c.size = size;
        if (status == IoStatus::Done) ++q.writes_done; else if (status == IoStatus::Failed) ++q.writes_failed;
        drop_write_copy(e);
    }
    else if (status == IoStatus::Done) { c.data = e.buf; c.size = size; ++q.reads_done; }
    else { free_in(entry_alloc(e), e.buf, e.size ? e.size : 1); if (status == IoStatus::Failed) ++q.reads_failed; }
    if (e.fd >= 0) close(e.fd);
    q.bytes_in_flight -= e.size;
    e.fd = -1; e.buf = nullptr; e.in_flight = false; e.status = status;
    --q.in_flight;
    q.completed.push(c);
}

// Takes every completion the ring has: whole reads finish, short reads continue.
void ring_reap(IoQueue& q) {
    u64 user; i32 res;
    while (io_ring_pop(q.ring, &user, &res)) {
        Handle h{user};
        IoEntry* e = q.entries.get(h);
        if (!e || !e->in_flight) { KV_WARN("io", "completion for an unknown request"); continue; }
        if (e->status == IoStatus::Cancelled) { ring_finish(q, h, *e, IoStatus::Cancelled, 0, 0); continue; }
        if (res < 0) { ring_finish(q, h, *e, IoStatus::Failed, -res, 0); continue; }
        e->done += (usize)res;
        if (e->write && res == 0 && e->done < e->size) { ring_finish(q, h, *e, IoStatus::Failed, ENOSPC, e->done); continue; }
        if (res == 0 || e->done >= e->size) { ring_finish(q, h, *e, IoStatus::Done, 0, e->done); continue; }
        // A short read or write: ask for the rest. A file that shrank answers 0 and ends it.
        usize left = e->size - e->done;
        u32 len = left > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)left;
        if (e->write) io_ring_push_write(q.ring, e->fd, e->buf + e->done, len, e->done, h.bits);
        else io_ring_push_read(q.ring, e->fd, e->buf + e->done, len, e->done, h.bits);
    }
}

// One pump's admissions: queued reads in priority order, cancelled ones completed, the
// rest handed over while `budget` (bytes; 0 unlimited) and the in-flight cap allow. Idle
// reads are taken only when `idle` says so. Returns the bytes admitted; sets *capped when
// an admissible read was left waiting on the budget.
usize admit(IoQueue& q, usize budget, bool idle, bool* capped) {
    if (q.queued.len > 1) qsort_r(q.queued.data, q.queued.len, sizeof(Handle), compare_queued, &q);
    usize submitted = 0;
    u32 taken = 0;
    *capped = false;
    for (; taken < q.queued.len; ++taken) {
        Handle h = q.queued[taken];
        IoEntry* e = q.entries.get(h);
        if (e->status == IoStatus::Cancelled) {
            IoCompletion c{};
            c.request = h; c.user = e->user; c.status = IoStatus::Cancelled;
            drop_write_copy(*e);
            q.completed.push(c);
            continue;
        }
        if (e->priority == IoPriority::Idle && !idle) break; // sorted last: nothing admissible follows
        if (!q.ring) {
            // The synchronous path learns the size by reading; it stops once the budget is passed.
            IoCompletion c{};
            perform_sync(q, h, c);
            q.completed.push(c);
            submitted += c.size;
            q.bytes_submitted += c.size;
            if (budget && submitted >= budget) { ++taken; break; }
            continue;
        }
        if (!io_ring_can_push(q.ring)) break;
        if (!ring_open(q, h, *e)) continue;
        if (budget && submitted && submitted + e->size > budget) { *capped = true; break; }
        if (q.budget.bytes_in_flight && q.in_flight && q.bytes_in_flight + e->size > q.budget.bytes_in_flight) { *capped = true; break; }
        if (ring_start(q, h, *e)) submitted += e->size;
    }
    if (taken) {
        for (u32 i = taken; i < q.queued.len; ++i) q.queued[i - taken] = q.queued[i];
        q.queued.len -= taken;
    }
    if (q.ring) {
        io_ring_enter(q.ring, 0);
        ring_reap(q);
        io_ring_enter(q.ring, 0); // continuations of short reads
    }
    return submitted;
}

} // namespace

bool IoQueue::init(Allocator* storage, u32 max_in_flight, IoBackend backend) {
    queued.alloc = completed.alloc = storage;
    kv_handle_kind_desc desc{};
    desc.name     = "platform.io";
    desc.type_key = type_key<IoEntry>();
    desc.ctx      = this;
    desc.resolve  = io_resolve;
    desc.describe = io_describe;
    desc.flags    = KV_HANDLE_GENERATIONAL;
    kind = handle_kind_register(desc);
    if (!kind) return false;
    entries.use_kind(kind);
    MemTag tag;
    if (storage) tag = storage->tag;
    if (!entries.init(max_in_flight, tag)) return false;
    if (backend != IoBackend::Sync) ring = io_ring_create(kRingEntries);
    if (backend == IoBackend::Uring && !ring) KV_WARN("io", "io_uring was asked for and is not available");
    if (!ring) KV_INFO("io", "synchronous reads");
    return true;
}

void IoQueue::free() {
    // Reads still in the ring: wait for them, then let the normal path free their buffers.
    if (ring) {
        while (io_ring_in_flight(ring) > 0) {
            if (!io_ring_enter(ring, io_ring_in_flight(ring))) break;
            ring_reap(*this);
        }
    }
    if (kind) kv_handle_kind_unregister(kind);
    // Completions nobody reaped: free their data with the allocator they named.
    for (const IoCompletion& c : completed) {
        IoEntry* e = entries.get(c.request);
        if (c.data) {
            KV_WARN("io", "completion for '%s' was never reaped", e ? e->path : "?");
            free_in(e && e->alloc ? e->alloc : mem_general(), c.data, c.size ? c.size : 1);
        }
    }
    for (u32 i = 0; i < entries.len; ++i) {
        if (!entries.is_alive(i)) continue;
        IoEntry& e = entries.items[i];
        if (e.fd >= 0 && !e.in_flight) close(e.fd);
        if (!e.in_flight) drop_write_copy(e); // a write never performed
    }
    io_ring_destroy(ring);
    ring = nullptr;
    entries.free();
    queued.free();
    completed.free();
    *this = IoQueue{};
}

Handle IoQueue::read_file(const IoRequest& req) {
    if (!req.path || strlen(req.path) >= sizeof(IoEntry::path)) {
        KV_ERROR("io", "bad path for read: '%s'", req.path ? req.path : "(null)");
        return Handle{};
    }
    IoEntry e{};
    strncpy(e.path, req.path, sizeof e.path - 1);
    e.alloc    = req.alloc;
    e.priority = req.priority;
    e.status   = IoStatus::Pending;
    e.user     = req.user;
    e.sequence = next_sequence++;
    e.fd       = -1;
    Handle h = entries.create(e);
    if (h.is_null()) { KV_ERROR("io", "queue full"); return h; }
    queued.push(h);
    return h;
}

Handle IoQueue::write_file(const IoWriteRequest& req) {
    if (!req.path || strlen(req.path) >= sizeof(IoEntry::path)) {
        KV_ERROR("io", "bad path for write: '%s'", req.path ? req.path : "(null)");
        return Handle{};
    }
    if (req.size && !req.data) { KV_ERROR("io", "write of '%s' has no data", req.path); return Handle{}; }
    IoEntry e{};
    strncpy(e.path, req.path, sizeof e.path - 1);
    e.alloc    = req.alloc;
    e.priority = req.priority;
    e.status   = IoStatus::Pending;
    e.user     = req.user;
    e.sequence = next_sequence++;
    e.fd       = -1;
    e.write    = true;
    e.size     = req.size;
    e.buf      = (u8*)alloc_in(entry_alloc(e), req.size ? req.size : 1);
    if (!e.buf) { KV_ERROR("io", "write of '%s': out of memory", req.path); return Handle{}; }
    if (req.size) memcpy(e.buf, req.data, req.size);
    Handle h = entries.create(e);
    if (h.is_null()) { free_in(entry_alloc(e), e.buf, req.size ? req.size : 1); KV_ERROR("io", "queue full"); return h; }
    queued.push(h);
    return h;
}

bool IoQueue::cancel(Handle request) {
    IoEntry* e = entries.get(request);
    if (!e || e->status != IoStatus::Pending) return false;
    e->status = IoStatus::Cancelled; // queued: completes as Cancelled at the pump; in flight: when the ring answers
    if (!e->in_flight && e->fd >= 0) { close(e->fd); e->fd = -1; } // opened for sizing, never started
    return true;
}

bool IoQueue::reap(Handle request) {
    for (u32 i = 0; i < completed.len; ++i) {
        if (completed[i].request != request) continue;
        completed[i] = completed[completed.len - 1]; // order among the rest does not matter
        --completed.len;
        entries.destroy(request);
        return true;
    }
    return false;
}

void IoQueue::pump() {
    ++pumps;
    bool capped = false;
    admit(*this, budget.bytes_per_pump, false, &capped);
    if (capped) ++pumps_capped;
}

usize IoQueue::pump_idle(usize bytes) {
    ++idle_calls;
    bool capped = false;
    f64 t0 = time_now();
    usize n = admit(*this, bytes ? bytes : 1, true, &capped);
    f64 dt = time_now() - t0;
    idle_bytes += n;
    if (n >= 4096) { // what admission cost per byte, smoothed; small calls say nothing useful
        f64 per_byte = dt / (f64)n;
        idle_seconds_per_byte = idle_seconds_per_byte == 0.0 ? per_byte : idle_seconds_per_byte * 0.7 + per_byte * 0.3;
    }
    return n;
}

usize IoQueue::idle_bytes_for(f64 seconds) const {
    usize most = budget.idle_bytes_per_call ? budget.idle_bytes_per_call : 1u << 20;
    if (seconds <= 0.0) return 0;
    if (idle_seconds_per_byte <= 0.0) return most > (1u << 20) ? 1u << 20 : most; // the first call: a modest guess
    f64 fit = seconds / idle_seconds_per_byte;
    usize bytes = fit >= (f64)most ? most : (usize)fit;
    return bytes < 16384 ? 16384 : bytes;
}

} // namespace kv
