#include "korvid/platform/io.h"
#include "korvid/core/format.h"
#include "korvid/core/sha256.h"
#include "korvid/core/sort.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/core/utf8.h"
#include "korvid/inflate/inflate.h"
#include "io_ring.h"
#include "io_task.h"
#include "os_file.h"
#include "os_paths.h"
#include "korvid/platform/module.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

constexpr u32 kRingEntries = 256;

void* io_resolve(kv_handle h, void* ctx) { return ((IoQueue*)ctx)->entries.get(Handle{h}); }

int io_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    IoQueue& q = *(IoQueue*)ctx;
    IoEntry* e = q.entries.get(Handle{h});
    if (!e) return format(out, cap, "stale");
    const char* st = e->status == IoStatus::Pending ? (e->in_flight ? "in flight" : "pending") :
                     e->status == IoStatus::Done ? "done" : e->status == IoStatus::Failed ? "failed" : "cancelled";
    return format(out, cap, "%s '%s'", st, e->path);
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

// The path the OS is given: the whole one, wherever it is kept.
const char* entry_file(const IoEntry& e) { return e.long_path ? e.long_path : e.path; }

// The path into the entry: inline when it fits, else whole in a block of its own (and cut
// inline, for logs). False with a log line if there is no memory for the block.
bool entry_set_path(IoEntry& e, const char* path) {
    usize n = strlen(path);
    if (n < sizeof e.path) { memcpy(e.path, path, n + 1); return true; }
    e.long_path = (char*)alloc_in(entry_alloc(e), n + 1);
    if (!e.long_path) { KV_ERROR("io", "a path of %llu bytes: out of memory", (unsigned long long)n); return false; }
    memcpy(e.long_path, path, n + 1);
    utf8_copy(e.path, sizeof e.path, path);
    return true;
}

void entry_free_path(IoEntry& e) {
    if (e.long_path) free_in(entry_alloc(e), e.long_path, strlen(e.long_path) + 1);
    e.long_path = nullptr;
}

// A stat on the ring: the kernel's answer, then a copy of the path, which must not move
// while the request is in flight (the pool's entries do, when it grows).
usize stat_block_size(const IoEntry& e) { return io_ring_stat_size() + strlen(entry_file(e)) + 1; }
void drop_stat_block(IoEntry& e) {
    if (e.stat && e.buf) { free_in(mem_general(), e.buf, stat_block_size(e)); e.buf = nullptr; }
}

void complete_failed(IoQueue& q, Handle h, IoEntry& e, IoCompletion& out, i32 err) {
    out.request = h; out.user = e.user; out.status = IoStatus::Failed; out.error = err;
    e.status = IoStatus::Failed;
    if (e.stat) ++q.stats_failed; else if (e.write) ++q.writes_failed; else ++q.reads_failed;
}

// A stat's answer onto its completion: 0 is Done (nothing there included), else Failed.
void complete_stat(IoQueue& q, Handle h, IoEntry& e, IoCompletion& out, i32 err, const IoStat& st) {
    if (err) { complete_failed(q, h, e, out, err); return; }
    out.request = h; out.user = e.user; out.status = IoStatus::Done; out.stat = st;
    e.status = IoStatus::Done;
    ++q.stats_done;
}

// What a finished read delivers: a deflated pack entry inflated into a buffer of its own
// size, and the CRC-32 checked where the entry asks. A failure frees the data and turns
// the completion Failed.
void deliver(IoQueue& q, IoEntry& e, IoCompletion& c) {
    if (c.status != IoStatus::Done || e.write) return;
    Allocator* a = entry_alloc(e);
    if (e.ranged && !e.clip && c.size != e.length) { // the file ended inside the pack entry
        KV_WARN("io", "'%s': %llu of %llu bytes", e.path, (unsigned long long)c.size, (unsigned long long)e.length);
        free_in(a, c.data, c.size ? c.size : 1);
        c.data = nullptr; c.size = 0; c.status = IoStatus::Failed; c.error = EIO;
        e.status = IoStatus::Failed; --q.reads_done; ++q.reads_failed;
        return;
    }
    if (e.ranged && e.method == ZipDeflate) {
        void* out = alloc_in(a, e.unpacked ? e.unpacked : 1);
        InflateOut r = out ? inflate_raw(c.data, c.size, out, e.unpacked) : InflateOut{};
        free_in(a, c.data, c.size ? c.size : 1);
        if (!out || r.result != InflateResult::Ok || r.produced != e.unpacked) {
            KV_WARN("io", "'%s': %s", e.path, out ? inflate_result_name(r.result) : "out of memory");
            free_in(a, out, e.unpacked ? e.unpacked : 1);
            c.data = nullptr; c.size = 0; c.status = IoStatus::Failed; c.error = EILSEQ;
            e.status = IoStatus::Failed; --q.reads_done; ++q.reads_failed;
            return;
        }
        c.data = out; c.size = e.unpacked;
        ++q.inflated;
    }
    if (e.check && crc32(0, c.data, c.size) != e.crc) {
        KV_WARN("io", "'%s': CRC-32 mismatch", e.path);
        free_in(a, c.data, c.size ? c.size : 1);
        c.data = nullptr; c.size = 0; c.status = IoStatus::Failed; c.error = EILSEQ;
        e.status = IoStatus::Failed; --q.reads_done; ++q.reads_failed; ++q.crc_failures;
    }
}

// A write's copy goes once the write is over, whatever happened to it.
void drop_write_copy(IoEntry& e) {
    if (e.write && e.buf) { free_in(entry_alloc(e), e.buf, e.size ? e.size : 1); e.buf = nullptr; }
}

// A read_file range against the file as it is now: `length` cut to what is there from
// `offset` (0 bytes for an offset past the end), and 0 meaning to the end.
bool clip_range(IoEntry& e, OsFile fd, i32* err) {
    u64 size = 0;
    if (!os_file_size(fd, &size, err)) return false;
    u64 left = e.offset < size ? size - e.offset : 0;
    if (!e.length || e.length > left) e.length = (usize)left;
    e.size = e.length;
    return true;
}

// ---- the synchronous backend: one read, write or stat, now -----------------------------------
void perform_sync(IoQueue& q, Handle h, IoCompletion& out) {
    IoEntry* e = q.entries.get(h);
    out.request = h;
    out.user    = e->user;
    if (e->status == IoStatus::Cancelled) { out.status = IoStatus::Cancelled; drop_write_copy(*e); return; }

    if (e->stat) {
        IoStat st;
        i32 err = io_stat_now(entry_file(*e), &st);
        complete_stat(q, h, *e, out, err, st);
        return;
    }

    if (e->write) {
        i32 err = 0;
        OsFile f = os_file_open(entry_file(*e), true, &err);
        if (f < 0) { complete_failed(q, h, *e, out, err); drop_write_copy(*e); return; }
        usize put = 0;
        while (put < e->size) {
            i64 n = os_file_write(f, e->buf + put, e->size - put, &err);
            if (n <= 0) break;
            put += (usize)n;
        }
        os_file_close(f);
        if (put != e->size) { complete_failed(q, h, *e, out, err ? err : EIO); drop_write_copy(*e); return; }
        out.status = IoStatus::Done;
        out.size   = e->size;
        e->status  = IoStatus::Done;
        ++q.writes_done;
        drop_write_copy(*e);
        return;
    }

    if (e->fail_errno) { complete_failed(q, h, *e, out, e->fail_errno); return; }
    if (e->ranged) {
        i32 err = 0;
        OsFile fd = e->fd_borrowed ? e->fd : os_file_open(entry_file(*e), false, &err);
        if (fd < 0) { complete_failed(q, h, *e, out, err); return; }
        if (e->clip && !clip_range(*e, fd, &err)) { os_file_close(fd); complete_failed(q, h, *e, out, err); return; }
        Allocator* a = entry_alloc(*e);
        u8* data = (u8*)alloc_in(a, e->length ? e->length : 1);
        usize got = 0;
        while (data && got < e->length) {
            i64 n = os_file_read_at(fd, data + got, e->length - got, e->offset + got, &err);
            if (n <= 0) break;
            got += (usize)n;
        }
        if (!e->fd_borrowed) os_file_close(fd);
        if (data && e->clip && got < e->length && !err) e->length = got; // the file shrank since it was sized: what is there
        if (!data || got != e->length) { free_in(a, data, e->length ? e->length : 1); complete_failed(q, h, *e, out, data ? (err ? err : EIO) : ENOMEM); return; }
        out.status = IoStatus::Done; out.data = data; out.size = got;
        e->status = IoStatus::Done;
        ++q.reads_done;
        deliver(q, *e, out);
        return;
    }

    i32 err = 0;
    OsFile f = os_file_open(entry_file(*e), false, &err);
    if (f < 0) { complete_failed(q, h, *e, out, err); return; }
    u64 size = 0;
    if (!os_file_size(f, &size, &err)) { os_file_close(f); complete_failed(q, h, *e, out, err); return; }
    usize len = (usize)size;

    Allocator* a = entry_alloc(*e);
    u8* data = (u8*)alloc_in(a, len ? len : 1);
    usize got = 0;
    while (data && got < len) {
        i64 n = os_file_read_at(f, data + got, len - got, got, &err);
        if (n <= 0) break;
        got += (usize)n;
    }
    os_file_close(f);
    if (!data || got != len) {
        free_in(a, data, len ? len : 1);
        complete_failed(q, h, *e, out, EIO);
        return;
    }
    out.status = IoStatus::Done;
    out.data   = data;
    out.size   = len;
    e->status  = IoStatus::Done;
    ++q.reads_done;
}

// ---- the ring backend --------------------------------------------------------------------------

// Stage one: opens and sizes the file, so the budget can decide. False (with a completion
// pushed) when the file cannot be opened.
bool ring_open(IoQueue& q, Handle h, IoEntry& e) {
    if (e.fail_errno) { IoCompletion c{}; complete_failed(q, h, e, c, e.fail_errno); q.completed.push(c); return false; }
    if (e.fd >= 0) return true; // opened for sizing earlier, or a pack's borrowed descriptor
    i32 err = 0;
    if (e.write) {
        OsFile fd = os_file_open(entry_file(e), true, &err);
        if (fd < 0) { IoCompletion c{}; complete_failed(q, h, e, c, err); drop_write_copy(e); q.completed.push(c); return false; }
        e.fd = fd;
        e.done = 0;
        return true;
    }
    OsFile fd = os_file_open(entry_file(e), false, &err);
    if (fd < 0) { IoCompletion c{}; complete_failed(q, h, e, c, err); q.completed.push(c); return false; }
    if (e.clip && !clip_range(e, fd, &err)) { os_file_close(fd); IoCompletion c{}; complete_failed(q, h, e, c, err); q.completed.push(c); return false; }
    if (e.ranged) { e.fd = fd; e.size = e.length; e.done = 0; return true; }
    u64 size = 0;
    if (!os_file_size(fd, &size, &err)) { os_file_close(fd); IoCompletion c{}; complete_failed(q, h, e, c, err); q.completed.push(c); return false; }
    e.fd   = fd;
    e.size = (usize)size;
    e.done = 0;
    return true;
}

// Stage two: the buffer and the first read, or the first write of the copy.
bool ring_start(IoQueue& q, Handle h, IoEntry& e) {
    if (!e.write) {
        e.buf = (u8*)alloc_in(entry_alloc(e), e.size ? e.size : 1);
        if (!e.buf) { os_file_close(e.fd); e.fd = -1; IoCompletion c{}; complete_failed(q, h, e, c, ENOMEM); q.completed.push(c); return false; }
    }
    e.in_flight = true;
    ++q.in_flight;
    q.bytes_in_flight += e.size;
    q.bytes_submitted += e.size;
    // An empty file: nothing to move; the ring still answers with 0 bytes, which is Done.
    u32 len = e.size > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)e.size;
    if (e.write) io_ring_push_write(q.ring, e.fd, e.buf, len, 0, h.bits);
    else io_ring_push_read(q.ring, e.fd, e.buf, len, e.ranged ? e.offset : 0, h.bits);
    return true;
}

// A stat the ring answered: `res` 0 with the kernel's record, or a negative errno, of which
// "nothing there" is an answer and not a failure.
void ring_finish_stat(IoQueue& q, Handle h, IoEntry& e, i32 res) {
    IoCompletion c{};
    if (e.status == IoStatus::Cancelled) { c.request = h; c.user = e.user; c.status = IoStatus::Cancelled; }
    else {
        IoStat st;
        i32 err = res < 0 ? -res : 0;
        if (err == ENOENT || err == ENOTDIR) err = 0;
        else if (!err) io_ring_stat_read(e.buf, &st);
        complete_stat(q, h, e, c, err, st);
    }
    drop_stat_block(e);
    e.in_flight = false;
    --q.in_flight;
    q.completed.push(c);
}

void ring_finish(IoQueue& q, Handle h, IoEntry& e, IoStatus status, i32 err, usize size) {
    IoCompletion c{};
    c.request = h; c.user = e.user; c.status = status; c.error = err;
    if (e.write) {
        c.size = size;
        if (status == IoStatus::Done) ++q.writes_done; else if (status == IoStatus::Failed) ++q.writes_failed;
        drop_write_copy(e);
    }
    else if (status == IoStatus::Done) { c.data = e.buf; c.size = size; ++q.reads_done; e.status = status; deliver(q, e, c); status = e.status; }
    else { free_in(entry_alloc(e), e.buf, e.size ? e.size : 1); if (status == IoStatus::Failed) ++q.reads_failed; }
    if (e.fd >= 0 && !e.fd_borrowed) os_file_close(e.fd);
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
        if (e->stat) { ring_finish_stat(q, h, *e, res); continue; }
        if (e->status == IoStatus::Cancelled) { ring_finish(q, h, *e, IoStatus::Cancelled, 0, 0); continue; }
        if (res < 0) { ring_finish(q, h, *e, IoStatus::Failed, -res, 0); continue; }
        e->done += (usize)res;
        if (e->write && res == 0 && e->done < e->size) { ring_finish(q, h, *e, IoStatus::Failed, ENOSPC, e->done); continue; }
        if (res == 0 || e->done >= e->size) { ring_finish(q, h, *e, IoStatus::Done, 0, e->done); continue; }
        // A short read or write: ask for the rest. A file that shrank answers 0 and ends it.
        usize left = e->size - e->done;
        u32 len = left > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)left;
        if (e->write) io_ring_push_write(q.ring, e->fd, e->buf + e->done, len, e->done, h.bits);
        else io_ring_push_read(q.ring, e->fd, e->buf + e->done, len, (e->ranged ? e->offset : 0) + e->done, h.bits);
    }
}

// One pump's admissions: queued reads in priority order, cancelled ones completed, the
// rest handed over while `budget` (bytes; 0 unlimited) and the in-flight cap allow. Idle
// reads are taken only when `idle` says so. Returns the bytes admitted; sets *capped when
// an admissible read was left waiting on the budget.
usize admit(IoQueue& q, usize budget, bool idle, bool* capped) {
    if (q.queued.len > 1) kv::sort(q.queued.data, q.queued.len, sizeof(Handle), compare_queued, &q);
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
        if (e->stat && q.ring && (!io_ring_can_stat(q.ring) || q.stat_on_tasks) && q.tasks) {
            if (!io_tasks_stat(q, h, *e)) { IoCompletion c{}; complete_failed(q, h, *e, c, ENOMEM); q.completed.push(c); }
            continue;
        }
        if (!q.ring || (e->stat && (!io_ring_can_stat(q.ring) || q.stat_on_tasks))) {
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
        if (e->stat) {
            e->buf = (u8*)alloc_in(mem_general(), stat_block_size(*e));
            if (!e->buf) { IoCompletion c{}; complete_failed(q, h, *e, c, ENOMEM); q.completed.push(c); continue; }
            char* path = (char*)e->buf + io_ring_stat_size();
            memcpy(path, entry_file(*e), strlen(entry_file(*e)) + 1);
            e->in_flight = true;
            ++q.in_flight;
            io_ring_push_stat(q.ring, path, e->buf, h.bits);
            continue;
        }
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

i32 io_stat_now(const char* path, IoStat* out) {
    *out = IoStat{};
    OsStat st;
    i32 err = 0;
    if (!os_path_stat(path, &st, &err)) return err == ENOENT || err == ENOTDIR ? 0 : (err ? err : EIO);
    out->is_file  = st.kind == OsPathKind::File;
    out->is_dir   = st.kind == OsPathKind::Dir;
    out->size     = st.size;
    out->mtime_ns = st.mtime_ns;
    return 0;
}

bool IoQueue::init(Allocator* storage, u32 max_in_flight, IoBackend backend) {
    queued.alloc = completed.alloc = mounts.alloc = storage;
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
    if (backend == IoBackend::Ring && !ring) KV_WARN("io", "the async ring was asked for and is not available");
    if (!ring) KV_INFO("io", "synchronous reads");
    return true;
}

void IoQueue::free() {
    // Tasks first: each is asked to stop, and the workers are joined before anything a
    // task might be using goes.
    io_tasks_stop(*this);
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
        if (e.fd >= 0 && !e.in_flight && !e.fd_borrowed) os_file_close(e.fd);
        if (!e.in_flight) drop_write_copy(e); // a write never performed
        if (!e.in_flight) drop_stat_block(e);
        entry_free_path(e);
    }
    for (IoMount& m : mounts) {
        if (m.fd >= 0) os_file_close(m.fd);
        pack_table_free(m.table, mounts.allocator());
    }
    mounts.free();
    io_ring_destroy(ring);
    ring = nullptr;
    entries.free();
    queued.free();
    completed.free();
    *this = IoQueue{};
}

namespace {

// An entry made for a request, queued; or, if the pool is full, its blocks freed and a null
// handle.
Handle queue_entry(IoQueue& q, IoEntry& e) {
    Handle h = q.entries.create(e);
    if (h.is_null()) {
        drop_write_copy(e);
        entry_free_path(e);
        KV_ERROR("io", "queue full");
        return h;
    }
    q.queued.push(h);
    return h;
}

// What read_file, write_file and stat_file start from: the request's fields and its path.
// False with a log line if there is no path or no memory for a long one.
bool entry_start(IoQueue& q, IoEntry& e, const char* path, Allocator* alloc, IoPriority priority, u64 user, const char* what) {
    e = IoEntry{};
    e.alloc    = alloc;
    e.priority = priority;
    e.status   = IoStatus::Pending;
    e.user     = user;
    e.fd       = -1;
    if (!path || !path[0]) { KV_ERROR("io", "%s with no path", what); return false; }
    if (!entry_set_path(e, path)) return false;
    e.sequence = q.next_sequence++;
    return true;
}

} // namespace

Handle IoQueue::read_file(const IoRequest& req) {
    IoEntry e;
    if (!entry_start(*this, e, req.path, req.alloc, req.priority, req.user, "read")) return Handle{};
    if (req.offset || req.length) {
        e.ranged = true;
        e.clip   = true;
        e.offset = req.offset;
        e.length = req.length;
    }
    return queue_entry(*this, e);
}

Handle IoQueue::stat_file(const IoRequest& req) {
    IoEntry e;
    if (!entry_start(*this, e, req.path, nullptr, req.priority, req.user, "stat")) return Handle{};
    e.stat = true;
    return queue_entry(*this, e);
}

Handle IoQueue::write_file(const IoWriteRequest& req) {
    if (req.size && !req.data) { KV_ERROR("io", "write of '%s' has no data", req.path ? req.path : "(null)"); return Handle{}; }
    IoEntry e;
    if (!entry_start(*this, e, req.path, req.alloc, req.priority, req.user, "write")) return Handle{};
    e.write    = true;
    e.size     = req.size;
    e.buf      = (u8*)alloc_in(entry_alloc(e), req.size ? req.size : 1);
    if (!e.buf) { KV_ERROR("io", "write of '%s': out of memory", e.path); entry_free_path(e); return Handle{}; }
    if (req.size) memcpy(e.buf, req.data, req.size);
    return queue_entry(*this, e);
}

// ---- mounts --------------------------------------------------------------------------------------

namespace {

bool pread_all(OsFile fd, void* buf, usize n, u64 at) {
    usize got = 0;
    i32 err = 0;
    while (got < n) {
        i64 r = os_file_read_at(fd, (u8*)buf + got, n - got, at + got, &err);
        if (r <= 0) return false;
        got += (usize)r;
    }
    return true;
}

// A request entry like read_file's, before it is queued.
IoEntry entry_for(IoQueue& q, const IoRequest& req) {
    IoEntry e{};
    e.alloc    = req.alloc;
    e.priority = req.priority;
    e.status   = IoStatus::Pending;
    e.user     = req.user;
    e.sequence = q.next_sequence++;
    e.fd       = -1;
    return e;
}

} // namespace

bool IoQueue::mount_dir(const char* dir) {
    if (!dir || os_path_kind(dir) != OsPathKind::Dir) { KV_WARN("io", "mount: '%s' is not a directory", dir ? dir : "?"); return false; }
    IoMount m{};
    usize n = strlen(dir);
    if (n + 2 > sizeof m.path) { KV_ERROR("io", "mount: path too long: '%s'", dir); return false; }
    memcpy(m.path, dir, n + 1);
    if (n && m.path[n - 1] != '/') { m.path[n] = '/'; m.path[n + 1] = 0; }
    m.is_pack = false;
    mounts.push(m);
    KV_INFO("io", "mounted directory %s", m.path);
    return true;
}

namespace {

// Opens an archive (a whole file, or `span` bytes at `base` inside one) and reads its
// table into `m`, allocating from `a`. What a mount is, without a queue to hold it:
// mount_pack_at pushes it, io_read_bundled and io_archive_entry use it as it is.
bool archive_open(IoMount& m, Allocator* a, const char* path, u64 base, u64 span, const char* prefix, bool quiet = false) {
    if (prefix && strlen(prefix) >= sizeof m.prefix) { KV_ERROR("io", "mount: prefix too long: '%s'", prefix); return false; }
    if (prefix) memcpy(m.prefix, prefix, strlen(prefix) + 1);
    m.base = base;
    if (!path || strlen(path) >= sizeof m.path) { KV_ERROR("io", "mount: bad pack path"); return false; }
    i32 err = 0;
    OsFile fd = os_file_open(path, false, &err);
    if (fd < 0) { KV_WARN("io", "mount: cannot open '%s': %s", path, strerror(err)); return false; }
    const char* why = nullptr;
    ZipTail tail;
    u8* buf = nullptr;
    usize buf_len = 0;
    auto give_up = [&](const char* reason) {
        KV_WARN("io", "mount: '%s' is not a usable zip: %s", path, reason);
        free_in(a, buf, buf_len ? buf_len : 1);
        pack_table_free(m.table, a);
        os_file_close(fd);
        return false;
    };
    u64 size = 0;
    if (!os_file_size(fd, &size, &err)) return give_up(strerror(err));
    // An archive inside the file: every offset below is the archive's own, read at base.
    if (base > size || span > size - base) return give_up("the archive runs past the end of the file");
    size = span ? span : size - base;
    auto rd = [&](void* to, usize n, u64 at) { return pread_all(fd, to, n, base + at); };
    // The tail, then the Zip64 end record if the tail points at one.
    buf_len = size < KV_ZIP_TAIL_MAX ? (usize)size : KV_ZIP_TAIL_MAX;
    buf = (u8*)alloc_in(a, buf_len ? buf_len : 1);
    if (!buf || !rd(buf, buf_len, size - buf_len)) return give_up("cannot read the end");
    if (!zip_find_end(buf, buf_len, size - buf_len, &tail, &why)) return give_up(why);
    if (tail.needs_zip64) {
        u8 rec[56];
        if (tail.zip64_end + sizeof rec > size || !rd(rec, sizeof rec, tail.zip64_end)) return give_up("cannot read the Zip64 end");
        if (!zip_read_zip64_end(rec, sizeof rec, &tail, &why)) return give_up(why);
    }
    free_in(a, buf, buf_len ? buf_len : 1);
    buf = nullptr; buf_len = 0;
    if (tail.cd_offset + tail.cd_size > size) return give_up("the central directory runs past the end");
    // The central directory.
    buf_len = (usize)tail.cd_size;
    buf = (u8*)alloc_in(a, buf_len ? buf_len : 1);
    if (!buf || !rd(buf, buf_len, tail.cd_offset)) return give_up("cannot read the central directory");
    if (!zip_read_central(buf, buf_len, tail, a, &m.table, &why)) return give_up(why);
    free_in(a, buf, buf_len ? buf_len : 1);
    buf = nullptr; buf_len = 0;
    // The index, if this is a pack and the archive has not changed since.
    const char* index_note = "no index";
    if (const PackEntry* ie = pack_find(m.table, KV_PACK_INDEX_NAME)) {
        u8 hdr[30];
        u64 data_at = 0; usize need = 0;
        PackTable indexed;
        if (ie->method == ZipStored && rd(hdr, sizeof hdr, ie->header_offset)) {
            zip_local_data_offset(hdr, sizeof hdr, ie->header_offset, &data_at, &need);
            if (need) data_at = ie->header_offset + need;
            buf_len = (usize)ie->size;
            buf = (u8*)alloc_in(a, buf_len ? buf_len : 1);
            if (need && buf && data_at + buf_len <= size && rd(buf, buf_len, data_at) && crc32(0, buf, buf_len) == ie->crc &&
                kpak_read_index(buf, buf_len, tail, a, &indexed, &why)) {
                pack_table_free(m.table, a);
                m.table = indexed;
                index_note = "indexed";
            } else {
                index_note = "index refused";
                KV_WARN("io", "mount: '%s': index refused (%s); reading it as a plain zip", path, why ? why : "unreadable");
            }
            free_in(a, buf, buf_len ? buf_len : 1);
            buf = nullptr; buf_len = 0;
        }
    }
    memcpy(m.path, path, strlen(path) + 1);
    m.is_pack = true;
    m.fd = fd;
    const PackTable& t = m.table;
    char names[160] = "";
    if (t.skipped || t.duplicates || t.not_utf8)
        format(names, sizeof names, "; names: %u skipped (unsafe or empty), %u duplicated (the last kept), %u not UTF-8 (kept as bytes)",
                 t.skipped, t.duplicates, t.not_utf8);
    char where[160] = "";
    if (base || m.prefix[0])
        format(where, sizeof where, " (%s%s%s%s)", m.prefix[0] ? "under " : "", m.prefix, base && m.prefix[0] ? ", " : "", base ? "an archive inside it" : "");
    if (!quiet)
        KV_INFO("io", "mounted pack %s%s: %u entries, %s, %u deflated, %u unaligned, %u unsupported%s", path, where, t.count, index_note,
                t.deflated, t.unaligned, t.unsupported, names);
    return true;
}

void archive_close(IoMount& m, Allocator* a) {
    pack_table_free(m.table, a);
    if (m.fd >= 0) os_file_close(m.fd);
    m.fd = -1;
}

} // namespace

bool IoQueue::mount_pack(const char* path, const char* prefix) { return mount_pack_at(path, 0, 0, prefix); }

bool IoQueue::mount_pack_at(const char* path, u64 base, u64 span, const char* prefix) {
    IoMount m{};
    if (!archive_open(m, mounts.allocator(), path, base, span, prefix)) return false;
    mounts.push(m);
    return true;
}

namespace {

// One entry of a mounted archive, read now: for io_read_bundled and io_archive_entry, which
// run before there is a queue or beside it. The data's place comes from the local header.
bool entry_data_at(const IoMount& m, const PackEntry& pe, u64* at) {
    if (pe.data_offset) { *at = m.base + pe.data_offset; return true; }
    u8 hdr[30];
    u64 unused = 0; usize need = 0;
    if (!pread_all(m.fd, hdr, sizeof hdr, m.base + pe.header_offset)) return false;
    zip_local_data_offset(hdr, sizeof hdr, pe.header_offset, &unused, &need);
    if (!need) return false;
    *at = m.base + pe.header_offset + need;
    return true;
}

// The program's own archive (os_bundle_archive), opened at its first use and kept for the
// process's life.
constinit IoMount g_bundle;
constinit int     g_bundle_state = 0; // 0 not tried, 1 open, -1 failed

IoMount* bundle_mount() {
    if (g_bundle_state == 0) {
        const char* archive = os_bundle_archive();
        g_bundle_state = archive && archive_open(g_bundle, mem_general(), archive, 0, 0, kBundlePrefix, true) ? 1 : -1;
    }
    return g_bundle_state == 1 ? &g_bundle : nullptr;
}

} // namespace

void io_bundle_close() {
    if (g_bundle_state == 1) archive_close(g_bundle, mem_general());
    g_bundle = IoMount{};
    g_bundle_state = 0;
}

bool io_read_bundled(const char* name, IoBytes* out, Allocator* alloc, i32* err) {
    *out = IoBytes{};
    i32 unused = 0;
    if (!err) err = &unused;
    if (!os_bundle_archive()) {
        char path[KV_PLATFORM_PATH_MAX + 256];
        char base[KV_PLATFORM_PATH_MAX];
        if (!os_base_path(base, sizeof base)) base[0] = 0;
        format(path, sizeof path, "%s%s", base, name);
        return io_read_now(path, out, alloc, err);
    }
    IoMount* m = bundle_mount();
    char norm[KV_PACK_PATH_MAX], full[sizeof m->prefix + KV_PACK_PATH_MAX];
    if (!m) { *err = EIO; return false; }
    if (!pack_normalize(name, norm, sizeof norm)) { *err = EINVAL; return false; }
    format(full, sizeof full, "%s%s", m->prefix, norm);
    const PackEntry* pe = pack_find(m->table, full);
    if (!pe) { *err = ENOENT; return false; }
    u64 at = 0;
    if ((pe->flags & PackEntryUnsupported) || !entry_data_at(*m, *pe, &at)) { *err = ENOTSUP; return false; }
    Allocator* a = alloc ? alloc : mem_general();
    u8* data = (u8*)a->alloc(a, (usize)pe->size + 1, 16);
    if (!data) { *err = ENOMEM; return false; }
    // Each failure is logged here, by the entry's name; *err says which kind it was.
    *err = 0;
    if (pe->method == ZipStored) {
        if (pe->csize != pe->size) { KV_WARN("io", "bundled '%s': stored, but %llu bytes packed for %llu unpacked", name, (unsigned long long)pe->csize, (unsigned long long)pe->size); *err = EILSEQ; }
        else if (!pread_all(m->fd, data, (usize)pe->size, at)) { KV_WARN("io", "bundled '%s': the archive ends inside its %llu bytes", name, (unsigned long long)pe->size); *err = EIO; }
    } else {
        u8* packed = (u8*)alloc_in(mem_general(), (usize)pe->csize + 1);
        if (!packed) *err = ENOMEM;
        else if (!pread_all(m->fd, packed, (usize)pe->csize, at)) { KV_WARN("io", "bundled '%s': the archive ends inside its %llu packed bytes", name, (unsigned long long)pe->csize); *err = EIO; }
        else {
            InflateOut r = inflate_raw(packed, (usize)pe->csize, data, (usize)pe->size);
            if (r.result != InflateResult::Ok || r.produced != pe->size) {
                KV_WARN("io", "bundled '%s': inflating gave %llu of %llu bytes (%s)", name, (unsigned long long)r.produced, (unsigned long long)pe->size, inflate_result_name(r.result));
                *err = EILSEQ;
            }
        }
        free_in(mem_general(), packed, (usize)pe->csize + 1);
    }
    if (!*err && crc32(0, data, (usize)pe->size) != pe->crc) { KV_WARN("io", "bundled '%s': CRC-32 mismatch", name); *err = EILSEQ; }
    if (*err) { a->free(a, data, (usize)pe->size + 1); return false; }
    data[pe->size] = 0;
    out->data = data;
    out->size = (usize)pe->size;
    out->alloc = a;
    return true;
}

bool io_archive_entry(const char* archive, const char* name, u64* offset, u64* size) {
    IoMount m{};
    if (!archive_open(m, mem_general(), archive, 0, 0, nullptr, true)) return false;
    char norm[KV_PACK_PATH_MAX];
    const PackEntry* pe = pack_normalize(name, norm, sizeof norm) ? pack_find(m.table, norm) : nullptr;
    bool ok = pe && pe->method == ZipStored && pe->csize == pe->size && entry_data_at(m, *pe, offset);
    if (ok) *size = pe->size;
    archive_close(m, mem_general());
    return ok;
}

Handle IoQueue::read_asset(const IoRequest& req) {
    char norm[KV_PACK_PATH_MAX];
    IoEntry e = entry_for(*this, req);
    if (!req.path || !pack_normalize(req.path, norm, sizeof norm)) {
        utf8_copy(e.path, sizeof e.path, req.path ? req.path : "(null)");
        e.fail_errno = EINVAL;
    } else {
        utf8_copy(e.path, sizeof e.path, norm);
        e.fail_errno = ENOENT;
        for (u32 i = mounts.len; i-- > 0;) {
            IoMount& m = mounts[i];
            if (!m.is_pack) {
                char full[sizeof m.path + KV_PACK_PATH_MAX];
                int n = format(full, sizeof full, "%s%s", m.path, norm);
                if (n < 0 || (usize)n >= sizeof full || os_path_kind(full) != OsPathKind::File) continue;
                if (!entry_set_path(e, full)) { e.fail_errno = ENOMEM; break; }
                e.fail_errno = 0;
                break;
            }
            const char* in_archive = norm;
            char prefixed[sizeof m.prefix + KV_PACK_PATH_MAX];
            if (m.prefix[0]) { format(prefixed, sizeof prefixed, "%s%s", m.prefix, norm); in_archive = prefixed; }
            PackEntry* pe = const_cast<PackEntry*>(pack_find(m.table, in_archive));
            if (!pe) continue;
            char where[sizeof m.path + KV_PACK_PATH_MAX + 2];
            format(where, sizeof where, "%s:%s", m.path, norm);
            utf8_copy(e.path, sizeof e.path, where); // for logs only: a long one is cut
            if (pe->flags & PackEntryUnsupported) { e.fail_errno = ENOTSUP; break; }
            if (!pe->data_offset) { // a plain zip: the local header says where the data is
                u8 hdr[30];
                u64 at = 0; usize need = 0;
                ++header_reads;
                if (!pread_all(m.fd, hdr, sizeof hdr, m.base + pe->header_offset)) { e.fail_errno = EIO; break; }
                zip_local_data_offset(hdr, sizeof hdr, pe->header_offset, &at, &need);
                if (!need) { e.fail_errno = EILSEQ; break; }
                pe->data_offset = pe->header_offset + need;
            }
            e.ranged      = true;
            e.fd_borrowed = true;
            e.fd          = m.fd;
            e.offset      = m.base + pe->data_offset;
            e.length      = (usize)pe->csize;
            e.size        = e.length;
            e.method      = pe->method;
            e.unpacked    = (usize)pe->size;
            e.crc         = pe->crc;
            e.check       = KV_DEV != 0;
            e.fail_errno  = 0;
            ++pack_reads;
            break;
        }
    }
    return queue_entry(*this, e);
}

namespace {

// One directory level: entries sorted by name, then files reported and directories walked.
// What each entry is, its size and time come with the listing (os_dir_each_stat): on
// Windows nothing is asked of a file, where asking twice (what it is, then its stat) was
// most of a big tree's scan.
struct ListEntry { char* name; OsStat st; };

int compare_entries(const void* a, const void* b) { return strcmp(((const ListEntry*)a)->name, ((const ListEntry*)b)->name); }

void collect_entry(void* user, const char* name, const OsStat& st) {
    if (name[0] == '.') return;
    usize n = strlen(name);
    char* copy = (char*)alloc_in(mem_general(), n + 1);
    memcpy(copy, name, n + 1);
    ((Array<ListEntry>*)user)->push(ListEntry{copy, st});
}

i32 walk(char* full, usize root_len, usize len, usize cap, bool (*fn)(void*, const char*, const IoStat&), void* user, bool* stop) {
    Array<ListEntry> entries;
    if (!os_dir_each_stat(full, collect_entry, &entries)) { entries.free(); return -1; }
    if (entries.len > 1) kv::sort(entries.data, entries.len, sizeof(ListEntry), compare_entries);
    i32 count = 0;
    for (const ListEntry& e : entries) {
        usize n = strlen(e.name);
        if (!*stop && len + 1 + n < cap) {
            full[len] = '/';
            memcpy(full + len + 1, e.name, n + 1);
            if (e.st.kind == OsPathKind::Dir) { i32 sub = walk(full, root_len, len + 1 + n, cap, fn, user, stop); if (sub > 0) count += sub; }
            else if (e.st.kind == OsPathKind::File) {
                ++count;
                IoStat st;
                st.is_file  = true;
                st.size     = e.st.size;
                st.mtime_ns = e.st.mtime_ns;
                if (!fn(user, full + root_len + 1, st)) *stop = true;
            }
            full[len] = 0;
        }
        free_in(mem_general(), e.name, n + 1);
    }
    entries.free();
    return count;
}

struct NameOnly { bool (*fn)(void*, const char*); void* user; };
bool name_only(void* user, const char* relative, const IoStat&) { NameOnly* c = (NameOnly*)user; return c->fn(c->user, relative); }

} // namespace

i32 io_list_files_stat(const char* dir, bool (*fn)(void* user, const char* relative, const IoStat& st), void* user) {
    char full[1024];
    usize n = dir ? strlen(dir) : 0;
    if (!n || n >= sizeof full) return -1;
    memcpy(full, dir, n + 1);
    while (n > 1 && full[n - 1] == '/') full[--n] = 0;
    bool stop = false;
    return walk(full, n, n, sizeof full, fn, user, &stop);
}

i32 io_list_files(const char* dir, bool (*fn)(void* user, const char* relative), void* user) {
    NameOnly c{fn, user};
    return io_list_files_stat(dir, name_only, &c);
}

namespace {

// A listing gathered whole, then sorted: names in one block of text, by offset, since the
// block moves as it grows.
struct DirItem { u32 at; IoDirEntry e; };
struct DirList { Array<char> text; Array<DirItem> items; u32 flags; };

void dir_collect(void* user, const OsDirEntry& oe) {
    DirList* l = (DirList*)user;
    if (oe.hidden && !(l->flags & IoListHidden)) return;
    u32 len = (u32)strlen(oe.name);
    DirItem it{l->text.len, IoDirEntry{}};
    memcpy(l->text.push_n(len + 1), oe.name, len + 1);
    it.e.st.is_file  = oe.st.kind == OsPathKind::File;
    it.e.st.is_dir   = oe.st.kind == OsPathKind::Dir;
    it.e.st.size     = oe.st.size;
    it.e.st.mtime_ns = oe.st.mtime_ns;
    it.e.is_link     = oe.is_link;
    it.e.hidden      = oe.hidden;
    it.e.utf8        = utf8_valid(oe.name, len);
    l->items.push(it);
}

int dir_item_cmp(const void* a, const void* b, void* ctx) {
    const char* t = (const char*)ctx;
    return strcmp(t + ((const DirItem*)a)->at, t + ((const DirItem*)b)->at);
}

} // namespace

bool io_list_dir(const char* dir, u32 flags, bool (*fn)(void* user, const IoDirEntry& e), void* user, i32* err) {
    if (!dir || !dir[0]) { if (err) *err = ENOENT; return false; }
    DirList l{};
    l.flags = flags;
    i32 e = 0;
    bool ok = os_dir_list(dir, (flags & IoListSizes) != 0, dir_collect, &l, &e);
    if (ok) {
        if (l.items.len > 1) kv::sort(l.items.data, l.items.len, sizeof(DirItem), dir_item_cmp, l.text.data);
        for (DirItem& it : l.items) {
            it.e.name = l.text.data + it.at;
            if (!fn(user, it.e)) break;
        }
    } else if (err) {
        *err = e ? e : EIO;
    }
    l.text.free();
    l.items.free();
    return ok;
}

bool io_read_each(const char* path, bool (*fn)(void* user, const u8* data, usize n), void* user, i32* err, usize chunk) {
    i32 e = 0;
    OsFile f = os_file_open(path, false, &e);
    if (f < 0) { if (err) *err = e; return false; }
    if (!chunk) chunk = 1u << 20;
    // A buffer the size of the file, up to `chunk`, and on the stack when it is small:
    // a tool hashing ten thousand small files from a dozen threads must not map and unmap
    // a megabyte for each (every unmap stops each core the process runs on).
    u64 size = 0;
    if (os_file_size(f, &size, &e) && size < chunk) chunk = size ? (usize)size : 1;
    u8 small[16384];
    u8* buf = chunk <= sizeof small ? small : (u8*)mem_alloc(chunk);
    bool ok = buf != nullptr;
    if (!ok) e = ENOMEM;
    for (u64 at = 0; ok;) {
        i64 n = os_file_read_at(f, buf, chunk, at, &e);
        if (n < 0) { ok = false; break; }
        if (n == 0 || !fn(user, buf, (usize)n)) break;
        at += (u64)n;
    }
    if (buf != small) mem_free(buf);
    os_file_close(f);
    if (!ok && err) *err = e;
    return ok;
}

namespace {
struct CopyCtx { OsFile to; Sha256* hash; i32 err; };
bool copy_piece(void* user, const u8* data, usize n) {
    CopyCtx& c = *(CopyCtx*)user;
    if (c.hash) c.hash->update(data, n);
    while (n) {
        i64 w = os_file_write(c.to, data, n, &c.err);
        if (w <= 0) { if (!c.err) c.err = EIO; return false; }
        data += w; n -= (usize)w;
    }
    return true;
}
} // namespace

bool io_copy_now(const char* from, const char* to, u8 hash[32], i32* err) {
    i32 e = 0;
    OsFile out = os_file_open(to, true, &e);
    if (out < 0) { if (err) *err = e; return false; }
    Sha256 h;
    h.init();
    CopyCtx c{out, hash ? &h : nullptr, 0};
    bool ok = io_read_each(from, copy_piece, &c, &e) && !c.err;
    os_file_close(out);
    if (!ok) { if (err) *err = c.err ? c.err : e; return false; }
    if (hash) h.finish(hash);
    return true;
}

bool io_make_dirs(const char* path) {
    char p[1024];
    usize n = path ? strlen(path) : 0;
    if (!n || n >= sizeof p) return false;
    memcpy(p, path, n + 1);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    // The leaf first: a directory already there, or one whose parent is, costs one call
    // (and the check below). Only past a missing parent is the path cut back, a level at a
    // time, and made again from there down: making every level from the root each time
    // was a dozen failed calls for every directory a tool asked for.
    usize end = n;
    while (!os_make_dir(p)) {
        usize s = end;
        do --s; while (s > 0 && p[s] != '/');
        if (s == 0) return false; // nothing above it to make
        p[s] = 0;
        end = s;
    }
    while (end < n) {
        p[end] = '/';
        usize next = end + 1;
        while (next < n && p[next]) ++next;
        if (!os_make_dir(p)) return false;
        end = next;
    }
    return os_path_kind(path) == OsPathKind::Dir; // what was there may have been a file
}

bool io_remove(const char* path) {
    if (!path || !path[0]) return false;
    return os_remove(path) || os_path_kind(path) == OsPathKind::None;
}

namespace {

void collect_entry(void* user, const char* name) {
    usize n = strlen(name);
    char* copy = (char*)alloc_in(mem_general(), n + 1);
    memcpy(copy, name, n + 1);
    ((Array<char*>*)user)->push(copy);
}

// Each entry is removed as it is (a file, a link, an empty directory); only one that will
// not go and is a directory is emptied first, so a link to a directory goes as a link and
// what it points at is never touched.
bool remove_tree(char* full, usize len, usize cap) {
    Array<char*> names;
    bool ok = os_dir_each(full, collect_entry, &names);
    for (char* name : names) {
        usize n = strlen(name);
        if (len + 1 + n < cap) {
            full[len] = '/';
            memcpy(full + len + 1, name, n + 1);
            if (!os_remove(full)) ok = os_path_kind(full) == OsPathKind::Dir ? remove_tree(full, len + 1 + n, cap) && ok : false;
            full[len] = 0;
        } else {
            ok = false;
        }
        free_in(mem_general(), name, n + 1);
    }
    names.free();
    return os_remove_dir(full) && ok; // emptied above: a directory
}

} // namespace

bool io_remove_tree(const char* path) {
    char full[1024];
    usize n = path ? strlen(path) : 0;
    if (!n || n >= sizeof full) return false;
    memcpy(full, path, n + 1);
    while (n > 1 && full[n - 1] == '/') full[--n] = 0;
    // Mostly a directory: an empty one, or nothing, goes in one call. Then a file or a
    // link, and only then a directory with something in it.
    if (os_remove_dir(full)) return true;
    if (os_remove(full) || os_path_kind(full) == OsPathKind::None) return true;
    return os_path_kind(full) == OsPathKind::Dir && remove_tree(full, n, sizeof full);
}

bool io_make_temp_dir(const char* prefix, char* out, u32 cap) {
    return prefix && out && cap && os_make_temp_dir(prefix, out, cap);
}

bool IoQueue::cancel(Handle request) {
    IoEntry* e = entries.get(request);
    if (!e || e->status != IoStatus::Pending) return false;
    if (e->task) return io_tasks_cancel(*this, *e); // completes as Cancelled when a worker is done with it
    e->status = IoStatus::Cancelled; // queued: completes as Cancelled at the pump; in flight: when the ring answers
    if (!e->in_flight && e->fd >= 0 && !e->fd_borrowed) { os_file_close(e->fd); e->fd = -1; } // opened for sizing, never started
    return true;
}

bool IoQueue::reap(Handle request) {
    for (u32 i = 0; i < completed.len; ++i) {
        if (completed[i].request != request) continue;
        completed[i] = completed[completed.len - 1]; // order among the rest does not matter
        --completed.len;
        if (IoEntry* e = entries.get(request)) entry_free_path(*e);
        entries.destroy(request);
        return true;
    }
    return false;
}

const char* IoQueue::backend_name() const { return io_ring_name(ring); }

void IoQueue::pump() {
    ++pumps;
    io_tasks_complete(*this);
    bool capped = false;
    admit(*this, budget.bytes_per_pump, false, &capped);
    if (capped) ++pumps_capped;
}

usize IoQueue::pump_idle(usize bytes) {
    ++idle_calls;
    io_tasks_complete(*this);
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
