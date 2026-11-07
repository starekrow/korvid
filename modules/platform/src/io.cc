#include "korvid/platform/io.h"
#include "korvid/core/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

namespace {

void* io_resolve(kv_handle h, void* ctx) { return ((IoQueue*)ctx)->entries.get(Handle{h}); }

int io_describe(kv_handle h, void* ctx, char* out, size_t cap) {
    IoQueue& q = *(IoQueue*)ctx;
    IoEntry* e = q.entries.get(Handle{h});
    if (!e) return snprintf(out, cap, "stale");
    const char* st = e->status == IoStatus::Pending ? "pending" : e->status == IoStatus::Done ? "done" :
                     e->status == IoStatus::Failed ? "failed" : "cancelled";
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

// The mock: one synchronous read. A real backend hands this to the ring.
void perform(IoQueue& q, Handle h, IoCompletion& out) {
    IoEntry* e = q.entries.get(h);
    out.request = h;
    out.user    = e->user;
    if (e->status == IoStatus::Cancelled) { out.status = IoStatus::Cancelled; return; }

    FILE* f = fopen(e->path, "rb");
    if (!f) { out.status = IoStatus::Failed; out.error = errno; e->status = IoStatus::Failed; ++q.reads_failed; return; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); out.status = IoStatus::Failed; out.error = errno; e->status = IoStatus::Failed; ++q.reads_failed; return; }

    Allocator* a = e->alloc ? e->alloc : mem_general();
    void* data = alloc_in(a, (usize)len ? (usize)len : 1);
    bool ok = data && (len == 0 || fread(data, 1, (usize)len, f) == (usize)len);
    fclose(f);
    if (!ok) {
        free_in(a, data, (usize)len ? (usize)len : 1);
        out.status = IoStatus::Failed; out.error = EIO; e->status = IoStatus::Failed; ++q.reads_failed;
        return;
    }
    out.status = IoStatus::Done;
    out.data   = data;
    out.size   = (usize)len;
    e->status  = IoStatus::Done;
    ++q.reads_done;
}

} // namespace

bool IoQueue::init(Allocator* storage, u32 max_in_flight) {
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
    return entries.init(max_in_flight, tag);
}

void IoQueue::free() {
    if (kind) kv_handle_kind_unregister(kind);
    // Completions nobody reaped: free their data with the allocator they named.
    for (const IoCompletion& c : completed) {
        IoEntry* e = entries.get(c.request);
        if (c.data) {
            KV_WARN("io", "completion for '%s' was never reaped", e ? e->path : "?");
            free_in(e && e->alloc ? e->alloc : mem_general(), c.data, c.size ? c.size : 1);
        }
    }
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
    Handle h = entries.create(e);
    if (h.is_null()) { KV_ERROR("io", "queue full"); return h; }
    queued.push(h);
    return h;
}

bool IoQueue::cancel(Handle request) {
    IoEntry* e = entries.get(request);
    if (!e || e->status != IoStatus::Pending) return false;
    e->status = IoStatus::Cancelled;
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
    if (queued.len == 0) return;
    if (queued.len > 1) qsort_r(queued.data, queued.len, sizeof(Handle), compare_queued, this);
    for (Handle h : queued) {
        IoCompletion c;
        perform(*this, h, c);
        completed.push(c);
    }
    queued.clear();
}

} // namespace kv
