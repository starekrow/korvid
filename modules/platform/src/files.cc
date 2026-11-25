// Korvid platform: blocking file I/O (io.h), on the same OS layer as the queue's.
#include "os_file.h"
#include "blocking.h"
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/phase.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include <errno.h>

namespace kv {

void IoBytes::free() {
    if (data) alloc->free(alloc, data, size + 1);
    data = nullptr;
    size = 0;
}

bool io_read_file(const char* path, IoBytes* out, Allocator* alloc, i32* err) {
    BlockingCall blocking("io_read_file", path);
    *out = IoBytes{};
    i32 e = 0;
    Handle f = os_file_open(path, false, &e);
    if (!os_handle_open(f)) { if (err) *err = e; return false; }
    u64 size = 0;
    if (!os_file_size(f, &size, &e)) { os_file_close(f); if (err) *err = e; return false; }
    Allocator* a = alloc ? alloc : mem_general();
    u8* data = (u8*)a->alloc(a, (usize)size + 1, 16);
    if (!data) { os_file_close(f); if (err) *err = ENOMEM; return false; }
    usize done = 0;
    while (done < size) {
        i64 n = os_file_read_at(f, data + done, (usize)size - done, done, &e);
        if (n == 0) e = EIO; // shorter than its size said: changed under the read
        if (n <= 0) { a->free(a, data, (usize)size + 1); os_file_close(f); if (err) *err = e; return false; }
        done += (usize)n;
    }
    os_file_close(f);
    data[size] = 0;
    out->data = data;
    out->size = (usize)size;
    out->alloc = a;
    return true;
}

bool io_write_file(const char* path, const void* data, usize size, i32* err) {
    BlockingCall blocking("io_write_file", path);
    IoFileWriter w;
    bool ok = w.open(path) && w.write(data, size);
    ok = w.close() && ok;
    if (!ok && err) *err = w.error;
    return ok;
}

bool IoFileWriter::open(const char* path) {
    BlockingCall blocking("IoFileWriter::open", path);
    error = 0;
    file = os_file_open(path, true, &error);
    return os_handle_open(file);
}

bool IoFileWriter::write(const void* data, usize size) {
    if (!os_handle_open(file) || error) { if (!error) error = EBADF; return false; }
    const u8* p = (const u8*)data;
    while (size) {
        i64 n = os_file_write(file, p, size, &error);
        if (n <= 0) { if (!error) error = EIO; return false; }
        p += n;
        size -= (usize)n;
    }
    return true;
}

bool IoFileWriter::print(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    i32 n = vformat(buf, sizeof buf, fmt, args);
    va_end(args);
    if (n < 0 || (usize)n >= sizeof buf) { if (!error) error = EINVAL; return false; }
    return write(buf, (usize)n);
}

bool IoFileWriter::close() {
    if (os_handle_open(file)) os_file_close(file);
    file = Handle{};
    if (!error) return true;
    return false;
}

bool io_stat_file(const char* path, IoStat* out, i32* err) {
    BlockingCall blocking("io_stat_file", path);
    OsStat st;
    *out = IoStat{};
    if (!os_path_stat(path, &st, err)) return false;
    *out = st;
    return true;
}

bool io_rename(const char* from, const char* to, i32* err) {
    BlockingCall blocking("io_rename", from);
    return os_rename(from, to, err);
}

bool io_path_is_remote(const char* path) { return path && path[0] && os_path_is_remote(path); }

bool io_current_dir(char* out, u32 cap, i32* err) { return os_current_dir(out, cap, err); }

bool io_is_file(const char* path) {
    BlockingCall blocking("io_is_file", path);
    return os_path_kind(path) == OsPathKind::File;
}
bool io_is_dir(const char* path) {
    BlockingCall blocking("io_is_dir", path);
    return os_path_kind(path) == OsPathKind::Dir;
}

} // namespace kv

namespace kv {

bool io_lock_file(const char* path, const char* who, Handle* lock, char* holder, u32 holder_cap, i32* err) {
    *lock = Handle{};
    if (!path || !path[0]) { if (err) *err = ENOENT; return false; }
    i32 why = 0;
    *lock = os_file_lock_try(path, who, holder, holder_cap, &why);
    if (!lock->is_null()) return true;
    if (err) *err = why ? why : EIO;
    return false;
}

void io_unlock_file(Handle lock) { os_file_close(lock); }

} // namespace kv

// ---- blocking calls in a tick (blocking.h) ------------------------------------------------
namespace kv {

namespace {
u32 g_warn_blocking = 0; // atomic: a paced loop is running
u32 g_blocking_warnings = 0; // atomic
}

void io_warn_blocking_in_ticks(bool on) { atomic_store(&g_warn_blocking, on ? 1u : 0u, MO_RELAXED); }
u32  io_blocking_warnings() { return atomic_load(&g_blocking_warnings, MO_RELAXED); }

#if KV_DEV
namespace {
thread_local u32 t_blocking_depth = 0;
struct BlockingSeen { const ScheduledSystem* unit; const char* call; };
BlockingSeen g_blocking_seen[64];
u32 g_blocking_count = 0;
u32 g_blocking_lock = 0; // a spin lock: taken once per call from a system, held for a scan

// True the first time this system makes this call (and for every one past the table's end:
// said again rather than not at all).
bool blocking_first(const ScheduledSystem* unit, const char* call) {
    u32 free = 0;
    while (!atomic_compare_exchange(&g_blocking_lock, &free, 1u, false, MO_ACQ_REL, MO_ACQUIRE)) free = 0;
    bool first = true;
    for (u32 i = 0; i < g_blocking_count && first; ++i)
        if (g_blocking_seen[i].unit == unit && g_blocking_seen[i].call == call) first = false;
    if (first && g_blocking_count < 64) g_blocking_seen[g_blocking_count++] = BlockingSeen{unit, call};
    atomic_store(&g_blocking_lock, 0u, MO_RELEASE);
    return first;
}
} // namespace

BlockingCall::BlockingCall(const char* call, const char* what) {
    if (t_blocking_depth++ || !atomic_load(&g_warn_blocking, MO_RELAXED)) return;
    const ScheduledSystem* unit = ctx_running();
    if (!unit || !blocking_first(unit, call)) return;
    atomic_fetch_add(&g_blocking_warnings, 1u, MO_RELAXED);
    KV_WARN("io", "%s%s%s%s blocks, and the system %s called it in a tick: a tick asks the I/O queue (read_file, stat_file, write_file; run_task for the rest) and takes the answer when it comes",
            call, what ? "('" : "", what ? what : "", what ? "')" : "", unit->name());
}
BlockingCall::~BlockingCall() { --t_blocking_depth; }
#endif

} // namespace kv
