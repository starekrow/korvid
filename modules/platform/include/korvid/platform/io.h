// Korvid platform: the I/O queue. Batched async requests with priorities, cancellation
// and completions, the shape of io_uring: submit any time; the pump moves finished work
// onto the completion list once per frame; completions stay there until a consumer reaps
// them, taking ownership of the data. Nothing completes inline.
//
// Two backends behind one shape. On Linux the pump hands reads to an io_uring (raw
// syscalls, no library) and reaps what has finished without waiting; everywhere else, and
// when the ring cannot be set up, the pump performs the reads itself with fopen/fread in
// priority order. Callers see the same queue either way; a test that wants the ordering
// of the synchronous path asks for it.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/core/pool.h"

namespace kv {

struct Engine;
struct IoRing;

enum class IoPriority : u8 { Low, Normal, High };
enum class IoStatus : u8 { Pending, Done, Failed, Cancelled };
enum class IoBackend : u8 { Auto, Sync, Uring };

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
    u64        sequence;  // submission order, for stable priority ordering
    // The ring's bookkeeping while a read is in flight.
    int        fd;
    u8*        buf;
    usize      size;      // the file's size at open
    usize      done;      // bytes read so far
    bool       in_flight;
};

struct IoQueue {
    Pool<IoEntry>       entries;
    Array<Handle>       queued;    // submitted, not yet performed or handed to the ring
    Array<IoCompletion> completed; // finished, not yet reaped
    IoRing*             ring = nullptr; // the io_uring backend when in use
    u64                 next_sequence = 0;
    u16                 kind = 0;
    u32                 reads_done = 0, reads_failed = 0, in_flight = 0;

    bool init(Allocator* storage, u32 max_in_flight, IoBackend backend = IoBackend::Auto);
    void free();
    const char* backend_name() const { return ring ? "io_uring" : "sync"; }

    // Submits a whole-file read. Null handle if the path is too long or the queue is full.
    Handle read_file(const IoRequest& req);
    // Cancels a request that has not completed yet; it completes as Cancelled.
    bool   cancel(Handle request);
    // Once per frame: hand queued reads to the backend and collect what has finished.
    void   pump();
    // Unreaped completions, oldest first. Consumers scan for theirs and reap them.
    Span<const IoCompletion> completions() const { return completed.span(); }
    // Takes a completion off the list; the data is the caller's to free from now on.
    // Returns false if it was not there (already reaped).
    bool   reap(Handle request);
    // Requests neither completed nor reaped: queued plus in flight.
    u32    pending() const { return queued.len + in_flight; }
};

// The platform module's queue, or null if the module is not started.
IoQueue* platform_io(Engine& e);
// Directory of the running executable, with a trailing separator. Assets live beneath it.
const char* platform_base_path(Engine& e);

} // namespace kv
