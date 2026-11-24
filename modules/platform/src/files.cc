// Korvid platform: blocking file I/O (io.h), on the same OS layer as the queue's.
#include "os_file.h"
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
    *out = IoBytes{};
    i32 e = 0;
    KHandle f = os_file_open(path, false, &e);
    if (!f.is_open()) { if (err) *err = e; return false; }
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
    IoFileWriter w;
    bool ok = w.open(path) && w.write(data, size);
    ok = w.close() && ok;
    if (!ok && err) *err = w.error;
    return ok;
}

bool IoFileWriter::open(const char* path) {
    error = 0;
    file = os_file_open(path, true, &error);
    return file.is_open();
}

bool IoFileWriter::write(const void* data, usize size) {
    if (!file.is_open() || error) { if (!error) error = EBADF; return false; }
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
    if (file.is_open()) os_file_close(file);
    file = KHandle{};
    if (!error) return true;
    return false;
}

bool io_stat_file(const char* path, IoStat* out, i32* err) {
    OsStat st;
    *out = IoStat{};
    if (!os_path_stat(path, &st, err)) return false;
    *out = st;
    return true;
}

bool io_rename(const char* from, const char* to, i32* err) { return os_rename(from, to, err); }

bool io_path_is_remote(const char* path) { return path && path[0] && os_path_is_remote(path); }

bool io_current_dir(char* out, u32 cap, i32* err) { return os_current_dir(out, cap, err); }

bool io_is_file(const char* path) { return os_path_kind(path) == OsPathKind::File; }
bool io_is_dir(const char* path) { return os_path_kind(path) == OsPathKind::Dir; }

} // namespace kv

namespace kv {

bool io_lock_file(const char* path, const char* who, KHandle* lock, char* holder, u32 holder_cap, i32* err) {
    *lock = KHandle{};
    if (!path || !path[0]) { if (err) *err = ENOENT; return false; }
    i32 why = 0;
    *lock = os_file_lock_try(path, who, holder, holder_cap, &why);
    if (lock->is_open()) return true;
    if (err) *err = why ? why : EIO;
    return false;
}

void io_unlock_file(KHandle lock) { os_file_close(lock); }

} // namespace kv
