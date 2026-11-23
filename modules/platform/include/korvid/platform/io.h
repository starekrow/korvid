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

namespace kv {

// A file the OS opened: a descriptor on POSIX, a HANDLE on Windows. Never negative when
// open; -1 is none on both.
typedef i64 OsFile;

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
    Allocator*  alloc    = nullptr;             // the completion's data comes from here; null: general
    IoPriority  priority = IoPriority::Normal;
    u64         user     = 0;                   // handed back untouched
    // A part of the file, for read_file: `length` bytes from `offset`. 0: to the end of the
    // file, so the defaults are the whole file. The file's end ends a range that runs past
    // it (the completion's size says how many came), and an offset past the end is Done
    // with 0 bytes. The budget counts what the range reads. read_asset ignores both.
    u64         offset   = 0;
    usize       length   = 0;
};

// What a path names, following links: io_stat's answer, and stat_file's in its completion.
// `volume` is the file system it is on, for comparing with another stat's and nothing
// else: equal while one volume is mounted there, different for another volume at the same
// path (a disk unplugged leaves its mount point an empty directory on the parent's).
// st_dev on POSIX, the volume serial number on Windows. 0 where it was not asked:
// io_list_files_stat's walk, and io_list_dir without IoListSizes.
// mtime_ns is nanoseconds from the OS's epoch: for comparing with another, nothing else.
struct IoStat { bool is_file = false, is_dir = false; u64 size = 0; i64 mtime_ns = 0; u64 volume = 0; };

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

struct IoCompletion {
    Handle   request;
    IoStatus status = IoStatus::Pending;
    void*    data   = nullptr; // a read's file contents; the consumer frees it with request.alloc. Null for a write or a stat
    usize    size   = 0;       // bytes read, or written
    u64      user   = 0;
    i32      error  = 0;       // errno on failure
    IoStat   stat;             // a stat_file's answer: all false and zero for a path where nothing is
};

struct IoEntry {
    char       path[256];     // the file, or "<pack>:<entry>" for a read from a pack; a longer one cut, for logs
    char*      long_path;     // a path of 256 bytes or more, whole, from the entry's allocator; null: `path` is it
    Allocator* alloc;
    IoPriority priority;
    IoStatus   status;
    u64        user;
    u64        sequence;  // submission order, for stable priority ordering
    bool       write;     // a write: `buf` holds the copy to write, `size` its length
    bool       stat;      // a stat: on the ring, `buf` holds the kernel's answer and a copy of the path
    IoTask*    task;      // a task, or a stat on the task workers: its node, until the pump completes it
    // The ring's bookkeeping while a request is in flight.
    OsFile     fd;
    u8*        buf;
    usize      size;      // the file's size at open, or the bytes to write
    usize      done;      // bytes read or written so far
    bool       in_flight;
    // A ranged read: `length` bytes at `offset` (from a pack, on the pack's own descriptor,
    // which the entry borrows). Then, for a deflated entry, `unpacked` bytes inflated;
    // `check` compares the CRC-32 of what is delivered with `crc`. `clip`: a read_file
    // range, cut to the file at open, where the end of the file is not an error.
    bool       ranged;
    bool       clip;
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
    char      path[256] = {}; // the directory (with a trailing '/') or the pack file
    bool      is_pack = false;
    OsFile    fd = -1;     // the pack's file, open for the queue's life
    u64       base = 0;    // where the archive starts in that file (a pack inside another archive)
    char      prefix[64] = {}; // only the archive's entries under this are seen, without it ("assets/")
    PackTable table;
};

struct IoQueue {
    Pool<IoEntry>       entries;
    IoTasks*            tasks = nullptr; // start_tasks's workers; none until then
    u32                 tasks_pending = 0, tasks_done = 0, tasks_failed = 0;
    Array<IoMount>      mounts;    // searched last first
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
    // Submits a stat of `path`: what io_stat says, in the completion's `stat`, with no data.
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
    // with a log line if the directory or the archive is not usable.
    bool   mount_dir(const char* dir);
    // With a prefix, only the entries under it are mounted, named without it: the part of
    // a larger archive that is this program's (an Android package's "assets/").
    // mount_pack_at mounts an archive that is `size` bytes at `offset` inside a file: a pack
    // stored, uncompressed, as an entry of another archive, read in place with the same
    // ranged reads. io_archive_entry finds such an entry.
    bool   mount_pack(const char* path, const char* prefix = nullptr);
    bool   mount_pack_at(const char* path, u64 offset, u64 size, const char* prefix = nullptr);
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
    // Unreaped completions, oldest first. Consumers scan for theirs and reap them.
    Span<const IoCompletion> completions() const { return completed.span(); }
    // Takes a completion off the list; the data is the caller's to free from now on.
    // Returns false if it was not there (already reaped).
    bool   reap(Handle request);
    // Requests neither completed nor reaped: queued plus in flight.
    u32    pending() const { return queued.len + in_flight + tasks_pending; }
};

// The files under a directory, recursively, in name order per directory, as paths relative
// to it ("maps/town.tmj"). Hidden entries (a leading '.') are skipped. Synchronous: for
// tools and startup, never for a tick. Returns the count, or -1 if `dir` cannot be read;
// `fn` returning false stops the walk.
i32  io_list_files(const char* dir, bool (*fn)(void* user, const char* relative), void* user);
// The same walk with each file's size and modification time, as io_stat would give them,
// from the listing itself: for a walk that would stat every file, at no cost a file more.
i32  io_list_files_stat(const char* dir, bool (*fn)(void* user, const char* relative, const IoStat& st), void* user);
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
// whether it is valid UTF-8, for whoever shows it.
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
bool io_make_dirs(const char* path);
// Removes a file, or a directory that is empty. Synchronous; true if it is gone after.
bool io_remove(const char* path);
// Removes a directory and everything under it, hidden entries included; a link inside is
// removed itself, never followed. Synchronous: for tools and tests cleaning up after
// themselves (io_make_temp_dir's directories). True if `path` is gone after.
bool io_remove_tree(const char* path);
// Makes a new, empty directory under the OS's temporary directory, its name starting with
// `prefix`, for a tool's or a test's scratch work; its path (no trailing '/') in out.
// Synchronous. False if none could be made.
bool io_make_temp_dir(const char* prefix, char* out, u32 cap);

// ---- Blocking file I/O ----------------------------------------------------------------
// For tools, startup, shutdown and tests: never for a tick, which goes through the queue.
// The same OS layer as the queue's (UTF-8 paths, CreateFileW on Windows), so a file reads
// and writes alike either way. Errors are errno values, as IoCompletion::error.

// A whole file's bytes, with a NUL after them (not counted in size), so text can be used
// as a C string. free() returns them to the allocator they came from.
struct IoBytes {
    u8*        data  = nullptr;
    usize      size  = 0;
    Allocator* alloc = nullptr;
    void free();
};
// Reads a whole file now, its bytes from `alloc` (null: general). False, with `out` empty
// and *err set if given, when it cannot be opened or read.
bool io_read_now(const char* path, IoBytes* out, Allocator* alloc = nullptr, i32* err = nullptr);
// Writes a whole file now: created, or truncated if it exists. False (*err) on failure.
bool io_write_now(const char* path, const void* data, usize size, i32* err = nullptr);
// A file front to back in pieces of up to `chunk` bytes, each handed to fn, which may stop
// the read by returning false. For what does not need the file whole in memory: a hash, a
// copy. False with *err set if it cannot be opened or read (a stop is not a failure).
bool io_read_each(const char* path, bool (*fn)(void* user, const u8* data, usize n), void* user, i32* err = nullptr, usize chunk = 1u << 20);
// Copies a file in pieces, `to` created or truncated; with `hash`, the SHA-256 of what was
// read. False with *err set if either side fails. What was written of `to` stays: a tool
// that must never be seen half done copies to a temporary name and renames it.
bool io_copy_now(const char* from, const char* to, u8 hash[32] = nullptr, i32* err = nullptr);

// A file written in pieces, now: open (created or truncated), write and print as often as
// need be, close. A failure sticks: what follows does nothing, and close reports it.
struct IoFileWriter {
    OsFile file  = -1;
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
// as io_read_now, which it is everywhere but Android. False with *err set if given; from a
// package, ENOENT for no such entry and, each logged with the entry's name, EIO for an
// archive that ends inside it and EILSEQ for data that does not inflate or fails its CRC-32.
bool io_read_bundled(const char* name, IoBytes* out, Allocator* alloc = nullptr, i32* err = nullptr);
// Where a stored (uncompressed) entry's bytes are inside a zip archive, for mount_pack_at.
// False if the archive cannot be read, has no such entry, or holds it deflated.
bool io_archive_entry(const char* archive, const char* name, u64* offset, u64* size);

// Renames a file, replacing one already at `to`: on one file system, all at once, so a
// reader sees the old file or the new and never half of either. How a file is written
// whole: to a temporary name beside it, then renamed.
bool io_rename(const char* from, const char* to);
// What a path names, with its size and modification time, now, on the calling thread: for
// tools. A tick asks IoQueue::stat_file. False if nothing is there.
bool io_stat(const char* path, IoStat* out);
// Whether the file system under `path` may change without this machine hearing of it:
// a network share (NFS, SMB; on Windows a mapped or \\server path; on Apple a volume
// not local), or on Linux any FUSE mount (sshfs, rclone). An OS's change notifications
// report only this machine's writes there, so FsWatcher polls such a root (fs_watch.h).
// Blocking: one statfs, or on Windows two calls about the volume. False if the path
// cannot be asked.
bool io_path_is_remote(const char* path);
// The process's working directory, '/' separated, no trailing slash.
bool io_current_dir(char* out, u32 cap);
bool io_is_file(const char* path);
bool io_is_dir(const char* path);

// The platform module's queue, or null if the module is not started.
IoQueue* platform_io(Engine& e);
// Directory of the running executable, with a trailing separator. Assets live beneath it.
const char* platform_base_path(Engine& e);
// The same, asked of the OS directly, for code with no engine (a test). False if the OS
// will not say.
bool        platform_exe_dir(char* out, u32 cap);
// Where this program's loose assets are, with no trailing separator: the output tree
// goldberg cooked for it, named by <exe>.assets beside the executable (one line, which
// kormake writes), or assets/ beside the executable when there is no such file (a
// packaged program). Without an engine: for tools, and tests that read a file directly.
// False if neither is a directory.
bool        platform_assets_dir(char* out, u32 cap);
// The per-user directory for saves and settings, created if need be, with a trailing
// separator: $XDG_DATA_HOME/<org>/<app>/, or ~/.local/share/<org>/<app>/, on Linux;
// ~/Library/Application Support/<org>/<app>/ on macOS (where SDL puts it on each). The first
// call fixes the names for the process; "" if the OS has none.
const char* platform_pref_path(Engine& e, const char* org, const char* app);

} // namespace kv
