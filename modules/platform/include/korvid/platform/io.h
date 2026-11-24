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
// the platform module calls from its Work unit with the time the Work cycle has left.
//
// A path has no limit of the queue's: one of 256 bytes or more is copied into a block
// from the request's allocator, and a path the OS refuses (past 4,096 bytes on Linux)
// completes Failed with the OS's error. Mounts are the exception: IoMount::path is 256.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/atomic.h"
#include "korvid/core/handle.h"
#include "korvid/core/pool.h"
#include "korvid/pack/zip.h"
#include "korvid/platform/khandle.h"

namespace kv {

struct Engine;
struct IoRing;

enum class IoPriority : u8 { Idle, Low, Normal, High }; // Idle: served only from idle time
enum class IoStatus : u8 { Pending, Done, Failed, Cancelled };
// Ring: the platform's async ring, io_uring on Linux and I/O rings on Windows 11.
enum class IoBackend : u8 { Auto, Sync, Ring };

struct IoBudget {
    usize bytes_per_pump      = 8u << 20;  // admitted per pump; 0: unlimited. At least one read always goes.
    usize bytes_in_flight     = 64u << 20; // outstanding at once; 0: unlimited
    usize idle_bytes_per_call = 4u << 20;  // the most one idle call may admit; idle_bytes_for sizes it to the time
};

struct IoRequest {
    const char* path     = nullptr;             // copied; for read_asset, the logical path
    Allocator*  alloc    = nullptr;             // the completion's bytes come from here; null: general
    IoPriority  priority = IoPriority::Normal;
    u64         user     = 0;                   // handed back untouched
    // A part of the file, for read_file: `length` bytes from `offset`. 0: to the end of the
    // file, so the defaults are the whole file. The file's end ends a range that runs past
    // it (the completion's size says how many came), and an offset past the end is Done
    // with 0 bytes. The budget counts what the range reads. read_asset ignores both.
    u64         offset   = 0;
    usize       length   = 0;
};

// A file's bytes, with a NUL after them (not counted in size), so text can be used as a
// C string. What a read gives, blocking (io_read_file) or from the queue (a completion's
// `bytes`): the same thing either way, freed the same way. free() returns them to the
// allocator they came from, and leaves the record empty.
struct IoBytes {
    u8*        data  = nullptr;
    usize      size  = 0;
    Allocator* alloc = nullptr;
    void free();
};

// What a path names, following links: io_stat_file's answer, and stat_file's in its completion.
// `volume` is the file system it is on, for comparing with another stat's and nothing
// else: equal while one volume is mounted there, different for another volume at the same
// path (a disk unplugged leaves its mount point an empty directory on the parent's).
// st_dev on POSIX, the volume serial number on Windows. 0 where it was not asked:
// io_list_files_stat's walk, and io_list_dir without IoListSizes.
// mtime_ns is nanoseconds from the OS's epoch: for comparing with another, nothing else.
// `kind` is what is there: nothing, a file, a directory, or something else (a device, a
// socket), which is neither is_file() nor is_dir().
enum class IoKind : u8 { None, File, Dir, Other };
struct IoStat {
    IoKind kind     = IoKind::None;
    u64    size     = 0;
    i64    mtime_ns = 0;
    u64    volume   = 0;
    bool is_file() const { return kind == IoKind::File; }
    bool is_dir() const { return kind == IoKind::Dir; }
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

// ---- Tasks ----------------------------------------------------------------------------
// Blocking work that is not one read or one write: a child process run to its end, a file
// hashed or copied as it streams. What a tool waits on, as I/O: submitted any time, run off
// the caller's thread, finished onto the same completion list at the pump, reaped the same
// way. A task runs on the queue's task workers (threads of its own: start_tasks), and is
// taken by a worker the moment one is free and its lane has room, not at the next pump: a
// worker that finishes one takes the next, so a lane stays full however often the queue
// is pumped. Within the lanes with room, higher priority first, then submission order.
//
// A task touches only its `arg`, which is the caller's and stays where it is until the
// completion is reaped (the queue never frees it). It returns 0 (Done) or an errno-like
// code (Failed, in the completion's `error`). A task cancelled before it starts never
// runs; one cancelled while it runs sees ctl.cancelled() and may stop early. Either way
// it completes as Cancelled.
struct IoTaskCtl {
    u32  flag = 0;
    bool cancelled() const { return atomic_load(&flag, MO_RELAXED) != 0; }
};
typedef i32 (*IoTaskFn)(void* arg, const IoTaskCtl& ctl);
struct IoTaskRequest {
    IoTaskFn    fn       = nullptr;
    void*       arg      = nullptr;
    const char* name     = "";                  // what describes it: the handle's description, logs; copied
    u8          lane     = 0;                   // [0, kIoLanes)
    IoPriority  priority = IoPriority::Normal;  // Idle runs last, but runs: tasks have no idle pump
    u64         user     = 0;
};
constexpr u32 kIoLanes = 8;
struct IoTask;  // the workers' node (io_task.cc)
struct IoTasks; // the workers, their lists and the lanes
struct IoEntry; // a request while the queue has it: its path, its state, the backend's bookkeeping (src/io_internal.h)
struct IoMount; // a directory or a pack that read_asset looks in (src/io_internal.h)

struct IoCompletion {
    Handle   request;
    IoStatus status  = IoStatus::Pending;
    IoBytes  bytes;            // a read's contents, as io_read_file gives them: the taker's, to bytes.free(). Empty for a write, a stat, a task
    usize    written = 0;      // a write's bytes
    u64      user    = 0;
    i32      error   = 0;      // errno on failure
    IoStat   stat;             // a stat_file's answer: all false and zero for a path where nothing is
};

struct IoQueue {
    Pool<IoEntry>*      entries = nullptr; // the requests the queue has, by handle: the queue's own, made by init
    IoTasks*            tasks = nullptr; // start_tasks's workers; none until then
    void              (*wake_fn)(void* user) = nullptr; // set_wake's
    void*               wake_user = nullptr;
    u32                 tasks_pending = 0, tasks_done = 0, tasks_failed = 0;
    Array<IoMount>*     mounts = nullptr; // searched last first: the queue's own (mount_count, mount_table)
    Array<Handle>       queued;    // submitted, not yet performed or handed to the ring
    Array<IoCompletion> completed; // finished, not yet reaped
    IoRing*             ring = nullptr; // the io_uring backend when in use
    IoBudget            budget;
    u64                 next_sequence = 0;
    u16                 kind = 0;
    u32                 reads_done = 0, reads_failed = 0, in_flight = 0;
    u32                 writes_done = 0, writes_failed = 0;
    u32                 stats_done = 0, stats_failed = 0;
    bool                stat_on_tasks = false; // stats go to the task workers even on a ring with its own (a test of that path)
    u32                 pack_reads = 0, inflated = 0, header_reads = 0, crc_failures = 0;
    usize               bytes_in_flight = 0;
    u64                 pumps = 0, pumps_capped = 0; // pumps that left admissible reads waiting on the budget
    u64                 bytes_submitted = 0, idle_calls = 0, idle_bytes = 0;
    f64                 idle_seconds_per_byte = 0.0; // measured over pump_idle calls, smoothed; 0 until the first

    bool init(Allocator* storage, u32 max_in_flight, IoBackend backend = IoBackend::Auto);
    void free();
    const char* backend_name() const; // "io_uring", "ioring" (Windows), "dispatch" (Apple), "threads" (Android) or "sync"

    // Submits a read of a file, or of the part of it that `offset` and `length` say. Null
    // handle with a log line if there is no path, no memory for a long one, or the queue
    // is full.
    Handle read_file(const IoRequest& req);
    // Submits a stat of `path`: what io_stat_file says, in the completion's `stat`, with no data.
    // A path where nothing is completes Done with is_file and is_dir false; Failed is an
    // error (no permission on a directory on the way). The queue's thread never waits on
    // the OS: on io_uring it is one IORING_OP_STATX; on a ring without a stat (Windows,
    // Apple, Android) it runs on the task workers. Under the synchronous backend, or with
    // no task workers started, it is done at the pump, as reads are. Priorities order it
    // with reads; the budget does not count it. offset, length and alloc are not used.
    Handle stat_file(const IoRequest& req);
    // Submits a read of an asset by its logical path ("rocks/rock.png"): normalized
    // ("a//b", "./a", "a\b" are "a/b"; ".." and absolute paths fail), then looked up in the
    // mounts, the last mounted first. A pack hit is a ranged read of the pack's file (and
    // an inflate, for a deflated entry); a directory hit is a whole-file read. The
    // completion is the same either way; a path no mount has completes Failed with ENOENT.
    Handle read_asset(const IoRequest& req);
    // Mounts. A directory is checked to exist; a pack's tail, central directory and index
    // are read now, synchronously, and its descriptor stays open until free(). False
    // with a log line if the directory or the archive is not usable, and *err (if given):
    // ENOTDIR, the OS's errno for a file that cannot be opened or read, EILSEQ for one that
    // is not a usable zip, ENAMETOOLONG for a path past a mount's 255 bytes.
    bool   mount_dir(const char* dir, i32* err = nullptr);
    // With a prefix, only the entries under it are mounted, named without it: the part of
    // a larger archive that is this program's (an Android package's "assets/").
    // mount_pack_at mounts an archive that is `size` bytes at `offset` inside a file: a pack
    // stored, uncompressed, as an entry of another archive, read in place with the same
    // ranged reads. io_archive_entry finds such an entry.
    bool   mount_pack(const char* path, const char* prefix = nullptr, i32* err = nullptr);
    bool   mount_pack_at(const char* path, u64 offset, u64 size, const char* prefix = nullptr, i32* err = nullptr);
    // How many mounts there are, in the order they were made (the last is searched first),
    // and a pack mount's table of entries: null for a directory, or past the count.
    // The most requests the queue holds at once (init's max_in_flight); a request's handle
    // index is below it.
    u32    capacity() const;
    u32    mount_count() const;
    const PackTable* mount_table(u32 mount) const;
    // Submits a whole-file write (create or truncate); the bytes are copied now. Same
    // pump, priorities, budget and completions as a read. Null handle as above.
    Handle write_file(const IoWriteRequest& req);
    // Starts `workers` task workers (once: false if they are already there, or a thread
    // will not start). Every lane is unlimited until set_lane says otherwise, so the
    // workers are the only cap.
    bool   start_tasks(u32 workers);
    // At most `limit` of the lane's tasks at once; 0: no limit but the workers. Any time:
    // a raised limit is taken up by the next worker free.
    void   set_lane(u32 lane, u32 limit);
    // For a loop that waits: fn(user) is called, on a worker's thread, each time a task (or
    // a stat on the workers) finishes, so the pump that collects it need not be polled for.
    // Null clears it. A read queued or on the ring calls nothing: it is collected by the
    // next pump, whenever that is. PlatformWatch::add(queue) sets this to wake its engine.
    void   set_wake(void (*fn)(void* user), void* user);
    // Submits a task. Null handle without workers, with a bad lane, or a full queue.
    Handle run_task(const IoTaskRequest& req);
    // Cancels a request that has not completed yet; it completes as Cancelled.
    bool   cancel(Handle request);
    // Once per frame: hand queued reads to the backend within the budget and collect what
    // has finished.
    void   pump();
    // From spare time (the Work cycle): admits up to `bytes` more, idle-priority reads included, and collects
    // what has finished. Returns the bytes admitted; 0 means nothing could go now.
    usize  pump_idle(usize bytes);
    // How many bytes an idle call may admit in `seconds`, from what earlier calls cost:
    // never more than idle_bytes_per_call, never less than a few pages.
    usize  idle_bytes_for(f64 seconds) const;
    // The request's completion, if it has one, off the list and into *out: the read's bytes
    // are the caller's from now on (out->bytes.free()). False while the request is still
    // on its way (is_pending) and for a handle that is not a request's, or was taken
    // already. The non-blocking twin of the blocking call:
    //     IoBytes b; if (io_read_file(path, &b)) { use(b); b.free(); }             // now
    //     Handle h = io->read_file(req);                                           // later:
    //     IoCompletion c; if (io->take(h, &c)) { if (c.status == IoStatus::Done) use(c.bytes); c.bytes.free(); }
    // With no `out`, the completion is dropped and its bytes freed.
    bool   take(Handle request, IoCompletion* out);
    // Whether the request is still queued or in flight: no completion yet, and one to come.
    bool   is_pending(Handle request) const;
    // Unreaped completions, oldest first: for a consumer of many requests, which scans for
    // its own (by `user`) and reaps each. One request's holder asks take().
    Span<const IoCompletion> completions() const { return completed.span(); }
    // Takes a completion off the list; its bytes (a copy of the completion holds them) are
    // the caller's to free from now on. Returns false if it was not there (already reaped).
    bool   reap(Handle request);
    // Requests neither completed nor reaped: queued plus in flight.
    u32    pending() const { return queued.len + in_flight + tasks_pending; }
};

// ---- Errors ---------------------------------------------------------------------------
// One convention for every blocking call in this header that can fail: its last parameter
// is `i32* err = nullptr`. A failure sets *err (if given) to an errno value, the same on
// every OS (Windows' errors are translated), which strerror describes and
// IoCompletion::error carries for the queue's requests; success leaves it alone. The
// return value alone says whether it failed.

// The files under a directory, recursively, in name order per directory, as paths relative
// to it ("maps/town.tmj"). Hidden entries (a leading '.') are skipped. Synchronous: for
// tools and startup, never for a tick. Returns the count, or -1 (*err) if `dir` cannot be
// read; `fn` returning false stops the walk.
i32  io_list_files(const char* dir, bool (*fn)(void* user, const char* relative), void* user, i32* err = nullptr);
// The same walk with each file's size and modification time, as io_stat_file would give them,
// from the listing itself: for a walk that would stat every file, at no cost a file more.
i32  io_list_files_stat(const char* dir, bool (*fn)(void* user, const char* relative, const IoStat& st), void* user, i32* err = nullptr);
// One directory's own entries, not its descendants, for a walk that chooses its own order
// (breadth first, or the directory someone opened before the rest). Blocking and safe on
// any thread: from a job or a queue task (run_task), never from a tick. Entries come in
// name order, bytewise; `fn` returning false stops the listing, which is not a failure.
// False with *err (if given) set when the directory cannot be listed: ENOENT, ENOTDIR,
// EACCES, EIO or the OS's own errno.
//
// Without IoListSizes an entry's kind is the listing's own (d_type on POSIX), and only a
// link, or an entry whose file system does not type it, is asked of: ten thousand files
// list without ten thousand stats. With it, POSIX asks each entry for its size and time
// (readdir has neither); Windows' listing has them either way.
//
// A name is handed back as the OS gave it and opens the same file when joined to `dir`
// and passed to read_file: bytes on POSIX, which need not be UTF-8, and on Windows WTF-8,
// UTF-8 that can also carry the half surrogate pair a UTF-16 name may hold. `utf8` says
// whether it is valid UTF-8, for whoever shows it. Apple's file systems refuse a name that
// is not, so there it is always true.
enum IoListFlags : u32 {
    IoListHidden = 1u << 0, // hidden entries too (a leading '.'; on Windows, the attribute as well)
    IoListSizes  = 1u << 1, // each entry's size and modification time in st
};
struct IoDirEntry {
    const char* name    = nullptr; // the entry's own name, valid during the call
    IoStat      st;                // what it is, following a link; size, time and volume with IoListSizes, else 0
    bool        is_link = false;   // a symbolic link (or a junction): st is its target's, all false if there is none
    bool        hidden  = false;   // one IoListHidden lets through
    bool        utf8    = true;    // false: the name is not valid UTF-8
};
bool io_list_dir(const char* dir, u32 flags, bool (*fn)(void* user, const IoDirEntry& e), void* user, i32* err = nullptr);
// Creates a directory and its parents (mkdir -p). Synchronous; true if it exists after.
// The path may be as long as the OS allows, as io_remove_tree's (on Apple, 1,024 bytes).
bool io_make_dirs(const char* path, i32* err = nullptr);
// Removes a file, or a directory that is empty. Synchronous; true if it is gone after.
bool io_remove(const char* path, i32* err = nullptr);
// Removes a directory and everything under it, hidden entries included; a link inside is
// removed itself, never followed. Synchronous: for tools and tests cleaning up after
// themselves (io_make_temp_dir's directories). True if `path` is gone after; *err is the
// first thing that would not go.
bool io_remove_tree(const char* path, i32* err = nullptr);
// Makes a new, empty directory under the OS's temporary directory, its name starting with
// `prefix`, for a tool's or a test's scratch work; its path (no trailing '/') in out.
// Synchronous. False if none could be made.
bool io_make_temp_dir(const char* prefix, char* out, u32 cap, i32* err = nullptr);

// ---- Blocking file I/O ----------------------------------------------------------------
// For tools, startup, shutdown and tests: never for a tick, which goes through the queue.
// The same OS layer as the queue's (UTF-8 paths, CreateFileW on Windows), so a file reads
// and writes alike either way. Errors are errno values, as IoCompletion::error (Errors, above).

// Reads a whole file now, its bytes from `alloc` (null: general). False, with `out` empty
// and *err set if given, when it cannot be opened or read.
bool io_read_file(const char* path, IoBytes* out, Allocator* alloc = nullptr, i32* err = nullptr);
// Writes a whole file now: created, or truncated if it exists. False (*err) on failure.
bool io_write_file(const char* path, const void* data, usize size, i32* err = nullptr);
// A file front to back in pieces of up to `chunk` bytes, each handed to fn, which may stop
// the read by returning false. For what does not need the file whole in memory: a hash, a
// copy. False with *err set if it cannot be opened or read (a stop is not a failure).
bool io_read_each(const char* path, bool (*fn)(void* user, const u8* data, usize n), void* user, usize chunk = 1u << 20, i32* err = nullptr);
// Copies a file in pieces, `to` created or truncated; with `hash`, the SHA-256 of what was
// read. False with *err set if either side fails. What was written of `to` stays: a tool
// that must never be seen half done copies to a temporary name and renames it.
bool io_copy_file(const char* from, const char* to, u8 hash[32] = nullptr, i32* err = nullptr);

// A file written in pieces, now: open (created or truncated), write and print as often as
// need be, close. A failure sticks: what follows does nothing, and close reports it.
struct IoFileWriter {
    KHandle file;
    i32    error = 0;
    bool open(const char* path);
    bool write(const void* data, usize size);
    bool print(const char* fmt, ...) KV_PRINTF(2, 3); // formatted, up to 4 KiB a call
    bool close();                                      // false if anything failed
};

// A file that ships with the program, by its name under the program's own directory
// ("shaders/sprite.spv"): beside the executable on the desktop, in an Apple bundle's
// resources, inside the package on Android (a stored or deflated entry under "assets/").
// What must be read before the mounts exist, or by something that has no queue. Blocking,
// as io_read_file, which it is everywhere but Android. False with *err set if given; from a
// package, ENOENT for no such entry and, each logged with the entry's name, EIO for an
// archive that ends inside it and EILSEQ for data that does not inflate or fails its CRC-32.
bool io_read_bundled(const char* name, IoBytes* out, Allocator* alloc = nullptr, i32* err = nullptr);
// Where a stored (uncompressed) entry's bytes are inside a zip archive, for mount_pack_at.
// False if the archive cannot be read (*err: the OS's errno, or EILSEQ for what is not a
// zip), has no such entry (ENOENT), or holds it deflated (EINVAL).
bool io_archive_entry(const char* archive, const char* name, u64* offset, u64* size, i32* err = nullptr);

// Renames a file, replacing one already at `to`: on one file system, all at once, so a
// reader sees the old file or the new and never half of either. How a file is written
// whole: to a temporary name beside it, then renamed.
bool io_rename(const char* from, const char* to, i32* err = nullptr);
// What a path names, with its size and modification time, now, on the calling thread: for
// tools. A tick asks IoQueue::stat_file. False if nothing is there (*err: ENOENT or
// ENOTDIR) or it cannot be asked (any other errno).
bool io_stat_file(const char* path, IoStat* out, i32* err = nullptr);
// Whether the file system under `path` may change without this machine hearing of it:
// a network share (NFS, SMB; on Windows a mapped or \\server path; on Apple a volume
// not local), or on Linux any FUSE mount (sshfs, rclone). An OS's change notifications
// report only this machine's writes there, so FsWatcher polls such a root (fs_watch.h).
// Blocking: one statfs, or on Windows two calls about the volume. False if the path
// cannot be asked.
bool io_path_is_remote(const char* path);
// The process's working directory, '/' separated, no trailing slash.
bool io_current_dir(char* out, u32 cap, i32* err = nullptr);
bool io_is_file(const char* path);
bool io_is_dir(const char* path);

// A lock between processes, by a file's name: for a program that must be the only one
// working on something (a tool on its output tree). Asked for without waiting. True with
// *lock: this process has it, and `who` and its process id are the file's one line
// ("watch 1234"), for whoever finds it taken. It is held until io_unlock_file, or until
// the process ends, however it ends: the OS lets go of it, so no lock is ever stale.
// False with *err EWOULDBLOCK: another process has it, and `holder` (if given) is its
// line. False with any other *err: the file cannot be made. flock on POSIX; LockFileEx on
// Windows, on a byte past the end so the line stays readable.
bool io_lock_file(const char* path, const char* who, KHandle* lock, char* holder = nullptr, u32 holder_cap = 0, i32* err = nullptr);
void io_unlock_file(KHandle lock);

// The platform module's queue, or null if the module is not started.
IoQueue* platform_io(Engine& e);

} // namespace kv
