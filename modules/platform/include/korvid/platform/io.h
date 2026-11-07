// Korvid platform: the I/O queue. Batched async requests with priorities, cancellation
// and completions, the shape of io_uring: submit any time; the pump moves finished work
// onto the completion list once per frame; completions stay there until a consumer reaps
// them, taking ownership of the data. Nothing completes inline.
//
// Slice 1 mocks the asynchrony: the pump performs queued reads synchronously with
// fopen/fread, in priority order. The frame loop and every consumer see the real shape,
// so the backend can become io_uring, DirectStorage or dispatch I/O without any caller
// changing.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/core/pool.h"

namespace kv {

struct Engine;

enum class IoPriority : u8 { Low, Normal, High };
enum class IoStatus : u8 { Pending, Done, Failed, Cancelled };

struct IoRequest {
    const char* path     = nullptr;             // copied
    Allocator*  alloc    = nullptr;             // the completion's data comes from here; null: general
    IoPriority  priority = IoPriority::Normal;
    u64         user     = 0;                   // handed back untouched
};

struct IoCompletion {
    Handle   request;
    IoStatus status = IoStatus::Pending;
    void*    data   = nullptr; // file contents; the consumer frees it with request.alloc
    usize    size   = 0;
    u64      user   = 0;
    i32      error  = 0;       // errno on failure
};

struct IoEntry {
    char       path[256];
    Allocator* alloc;
    IoPriority priority;
    IoStatus   status;
    u64        user;
    u64        sequence; // submission order, for stable priority ordering
};

struct IoQueue {
    Pool<IoEntry>       entries;
    Array<Handle>       queued;    // submitted, not yet performed
    Array<IoCompletion> completed; // finished, not yet reaped
    u64                 next_sequence = 0;
    u16                 kind = 0;
    u32                 reads_done = 0, reads_failed = 0;

    bool init(Allocator* storage, u32 max_in_flight);
    void free();

    // Submits a whole-file read. Null handle if the path is too long or the queue is full.
    Handle read_file(const IoRequest& req);
    // Cancels a request that has not been performed yet; it completes as Cancelled.
    bool   cancel(Handle request);
    // Once per frame: perform what is queued and move it to `completed`.
    void   pump();
    // Unreaped completions, oldest first. Consumers scan for theirs and reap them.
    Span<const IoCompletion> completions() const { return completed.span(); }
    // Takes a completion off the list; the data is the caller's to free from now on.
    // Returns false if it was not there (already reaped).
    bool   reap(Handle request);
    u32    pending() const { return queued.len; }
};

// The platform module's queue, or null if the module is not started.
IoQueue* platform_io(Engine& e);
// Directory of the running executable, with a trailing separator. Assets live beneath it.
const char* platform_base_path(Engine& e);

} // namespace kv
