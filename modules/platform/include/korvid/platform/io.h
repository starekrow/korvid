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
//
// The pump is bounded by bytes, not requests: handing a read to the kernel costs time in
// proportion to its size (page cache allocation, and on a hot cache the whole read runs
// inside the submit), so a pump admits reads until bytes_per_pump would be passed, and
// never more than bytes_in_flight at once. The rest wait for the next pump. Requests at
// IoPriority::Idle are never admitted by the pump at all: they wait for pump_idle, which
// the platform module calls from the engine's idle time with a budget of its own.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/core/pool.h"
#include "korvid/pack/zip.h"

namespace kv {

// A file the OS opened: a descriptor on POSIX, a HANDLE on Windows. Never negative when
// open; -1 is none on both.
typedef i64 OsFile;

struct Engine;
struct IoRing;

enum class IoPriority : u8 { Idle, Low, Normal, High }; // Idle: served only from idle time
enum class IoStatus : u8 { Pending, Done, Failed, Cancelled };
enum class IoBackend : u8 { Auto, Sync, Uring };

struct IoBudget {
    usize bytes_per_pump      = 8u << 20;  // admitted per pump; 0: unlimited. At least one read always goes.
    usize bytes_in_flight     = 64u << 20; // outstanding at once; 0: unlimited
    usize idle_bytes_per_call = 4u << 20;  // the most one idle call may admit; idle_bytes_for sizes it to the time
};

struct IoRequest {
    const char* path     = nullptr;             // copied; for read_asset, the logical path
    Allocator*  alloc    = nullptr;             // the completion's data comes from here; null: general
    IoPriority  priority = IoPriority::Normal;
    u64         user     = 0;                   // handed back untouched
};

// A whole-file write: the bytes are copied at submission, so the caller's buffer is free
// to go at once. The file is created or truncated. The completion carries no data.
struct IoWriteRequest {
    const char* path     = nullptr;
    const void* data     = nullptr;
    usize       size     = 0;
    Allocator*  alloc    = nullptr;             // the copy lives here until the write is done; null: general
    IoPriority  priority = IoPriority::Normal;
    u64         user     = 0;
};

struct IoCompletion {
    Handle   request;
    IoStatus status = IoStatus::Pending;
    void*    data   = nullptr; // a read's file contents; the consumer frees it with request.alloc. Null for a write
    usize    size   = 0;       // bytes read, or written
    u64      user   = 0;
    i32      error  = 0;       // errno on failure
};

struct IoEntry {
    char       path[256];     // the file, or "<pack>:<entry>" for a read from a pack
    Allocator* alloc;
    IoPriority priority;
    IoStatus   status;
    u64        user;
    u64        sequence;  // submission order, for stable priority ordering
    bool       write;     // a write: `buf` holds the copy to write, `size` its length
    // The ring's bookkeeping while a request is in flight.
    OsFile     fd;
    u8*        buf;
    usize      size;      // the file's size at open, or the bytes to write
    usize      done;      // bytes read or written so far
    bool       in_flight;
    // A ranged read: `length` bytes at `offset` (from a pack, on the pack's own descriptor,
    // which the entry borrows). Then, for a deflated entry, `unpacked` bytes inflated;
    // `check` compares the CRC-32 of what is delivered with `crc`.
    bool       ranged;
    bool       fd_borrowed;
    bool       check;
    u16        method;
    i32        fail_errno; // resolved as missing or unreadable: completes Failed at the pump
    u64        offset;
    usize      length;
    usize      unpacked;
    u32        crc;
};

// Where read_asset looks: a directory, or a pack (a zip) whose table was read at mount.
struct IoMount {
    char      path[256];   // the directory (with a trailing '/') or the pack file
    bool      is_pack;
    OsFile    fd = -1;     // the pack's file, open for the queue's life
    PackTable table;
};

struct IoQueue {
    Pool<IoEntry>       entries;
    Array<IoMount>      mounts;    // searched last first
    Array<Handle>       queued;    // submitted, not yet performed or handed to the ring
    Array<IoCompletion> completed; // finished, not yet reaped
    IoRing*             ring = nullptr; // the io_uring backend when in use
    IoBudget            budget;
    u64                 next_sequence = 0;
    u16                 kind = 0;
    u32                 reads_done = 0, reads_failed = 0, in_flight = 0;
    u32                 writes_done = 0, writes_failed = 0;
    u32                 pack_reads = 0, inflated = 0, header_reads = 0, crc_failures = 0;
    usize               bytes_in_flight = 0;
    u64                 pumps = 0, pumps_capped = 0; // pumps that left admissible reads waiting on the budget
    u64                 bytes_submitted = 0, idle_calls = 0, idle_bytes = 0;
    f64                 idle_seconds_per_byte = 0.0; // measured over pump_idle calls, smoothed; 0 until the first

    bool init(Allocator* storage, u32 max_in_flight, IoBackend backend = IoBackend::Auto);
    void free();
    const char* backend_name() const { return ring ? "io_uring" : "sync"; }

    // Submits a whole-file read. Null handle if the path is too long or the queue is full.
    Handle read_file(const IoRequest& req);
    // Submits a read of an asset by its logical path ("example_rocks/rock.png"): normalized
    // ("a//b", "./a", "a\b" are "a/b"; ".." and absolute paths fail), then looked up in the
    // mounts, the last mounted first. A pack hit is a ranged read of the pack's file (and
    // an inflate, for a deflated entry); a directory hit is a whole-file read. The
    // completion is the same either way; a path no mount has completes Failed with ENOENT.
    Handle read_asset(const IoRequest& req);
    // Mounts. A directory is checked to exist; a pack's tail, central directory and index
    // are read now, synchronously, and its descriptor stays open until free(). False
    // with a log line if the directory or the archive is not usable.
    bool   mount_dir(const char* dir);
    bool   mount_pack(const char* path);
    // Submits a whole-file write (create or truncate); the bytes are copied now. Same
    // pump, priorities, budget and completions as a read. Null handle as above.
    Handle write_file(const IoWriteRequest& req);
    // Cancels a request that has not completed yet; it completes as Cancelled.
    bool   cancel(Handle request);
    // Once per frame: hand queued reads to the backend within the budget and collect what
    // has finished.
    void   pump();
    // From idle time: admits up to `bytes` more, idle-priority reads included, and collects
    // what has finished. Returns the bytes admitted; 0 means nothing could go now.
    usize  pump_idle(usize bytes);
    // How many bytes an idle call may admit in `seconds`, from what earlier calls cost:
    // never more than idle_bytes_per_call, never less than a few pages.
    usize  idle_bytes_for(f64 seconds) const;
    // Unreaped completions, oldest first. Consumers scan for theirs and reap them.
    Span<const IoCompletion> completions() const { return completed.span(); }
    // Takes a completion off the list; the data is the caller's to free from now on.
    // Returns false if it was not there (already reaped).
    bool   reap(Handle request);
    // Requests neither completed nor reaped: queued plus in flight.
    u32    pending() const { return queued.len + in_flight; }
};

// The files under a directory, recursively, in name order per directory, as paths relative
// to it ("maps/town.tmj"). Hidden entries (a leading '.') are skipped. Synchronous: for
// tools and startup, never for a tick. Returns the count, or -1 if `dir` cannot be read;
// `fn` returning false stops the walk.
i32  io_list_files(const char* dir, bool (*fn)(void* user, const char* relative), void* user);
// Creates a directory and its parents (mkdir -p). Synchronous; true if it exists after.
bool io_make_dirs(const char* path);
// Removes a file, or a directory that is empty. Synchronous; true if it is gone after.
bool io_remove(const char* path);
// Makes a new, empty directory under the OS's temporary directory, its name starting with
// `prefix`, for a tool's or a test's scratch work; its path (no trailing '/') in out.
// Synchronous. False if none could be made.
bool io_make_temp_dir(const char* prefix, char* out, u32 cap);

// The platform module's queue, or null if the module is not started.
IoQueue* platform_io(Engine& e);
// Directory of the running executable, with a trailing separator. Assets live beneath it.
const char* platform_base_path(Engine& e);
// The per-user directory for saves and settings, created if need be, with a trailing
// separator: $XDG_DATA_HOME/<org>/<app>/, or ~/.local/share/<org>/<app>/, on
// Linux (where SDL put it). The first call fixes the names for the process; "" if the OS has none.
const char* platform_pref_path(Engine& e, const char* org, const char* app);

} // namespace kv
