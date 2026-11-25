// Korvid platform: files and directories from the OS, one source per OS (posix/file_posix.cc,
// win32/file_win32.cc). Paths are UTF-8 with '/' separators. Errors are errno values on
// every platform: they are what IoCompletion::error carries and what os_error_text describes.
#pragma once
#include "korvid/platform/io.h"
#include "os_handle_table.h"
#include "os_err.h"

namespace kv {

// Opens for reading, or for writing (created, or truncated if it exists). -1 with *err set
// on failure. The file is not inherited by child processes.
Handle os_file_open(const char* path, bool write, i32* err);
void   os_file_close(Handle f);
bool   os_file_size(Handle f, u64* size, i32* err);
// Up to n bytes at offset `at`, without moving any file position: the count, 0 at the end,
// or -1 with *err set. Interrupted reads are retried.
i64    os_file_read_at(Handle f, void* buf, usize n, u64 at, i32* err);
// Up to n bytes at the file's position: the count, or -1 with *err set.
i64    os_file_write(Handle f, const void* buf, usize n, i32* err);
// What was written, on the disk (fsync, FlushFileBuffers). False with *err.
bool   os_file_sync(Handle f, i32* err);
// Opens path (made if missing) and holds an exclusive lock on it until os_file_close:
// another process that asks waits. Between processes only (flock, LockFileEx). -1 with *err.
Handle os_file_lock(const char* path, i32* err);
// The same lock asked for without waiting, for a program that must be the only one at
// something: none with *err EWOULDBLOCK when another process holds it, `holder` (if given)
// then its line; else `who` and this process's id written as the file's one line.
Handle os_file_lock_try(const char* path, const char* who, char* holder, u32 cap, i32* err);
// That a rename into dir is on the disk: the directory's own entry flushed (POSIX; nothing
// on Windows, whose MoveFileEx is asked to write through).
void   os_dir_sync(const char* dir);

// The public record is the OS layer's too: one stat, one kind.
using OsPathKind = IoKind;
// What a path names, following links.
OsPathKind os_path_kind(const char* path);
// Whether path is itself a symbolic link (a junction too, on Windows) that reaches a
// directory: what a recursive watch does not go down (KR16).
bool os_path_is_dir_link(const char* path);
// What a path names with its size and modification time (nanoseconds since the OS's
// epoch: only ever compared with another) and the volume it is on (IoStat::volume),
// following links. False if nothing is there, or it cannot be asked, with *err (if given)
// saying which: ENOENT or ENOTDIR for nothing. os_dir_each_stat's volume is 0 on Windows.
using OsStat = IoStat;
bool os_path_stat(const char* path, OsStat* out, i32* err = nullptr);
// Whether the file system under path may change without this machine hearing of it (a
// share, a FUSE mount): io_path_is_remote. False if it cannot be asked.
bool os_path_is_remote(const char* path);
// Every call below that can fail takes a last `i32* err`, set (if given) to the errno of a
// failure and left alone otherwise.
// Renames a file over whatever is at `to`.
bool os_rename(const char* from, const char* to, i32* err = nullptr);
// The working directory, '/' separated.
bool os_current_dir(char* out, u32 cap, i32* err = nullptr);
// Calls fn with each entry's name in dir, '.' and '..' left out, in no particular order.
// False if dir cannot be read.
bool os_dir_each(const char* dir, void (*fn)(void* user, const char* name), void* user, i32* err = nullptr);
// The same with what os_path_stat would say of each entry, for walks that would otherwise
// ask it of every one: on Windows the listing itself carries it, no query a file (a
// symbolic link is still asked, so it is followed); elsewhere one stat an entry.
bool os_dir_each_stat(const char* dir, void (*fn)(void* user, const char* name, const OsStat& st), void* user, i32* err = nullptr);
// One entry of os_dir_list: its name (valid during the call), what it is, following a
// link, and whether it is one. With `sizes`, st has the size, time and volume; without, only
// the kind, and from the listing where the OS has it there. A link whose target is not
// there has kind None. Hidden: a leading '.', or Windows' hidden attribute.
struct OsDirEntry { const char* name; OsStat st; bool is_link = false; bool hidden = false; };
// Calls fn with each of dir's entries, '.' and '..' left out, in no particular order.
// False with *err set (errno) when dir cannot be read, or the listing fails part way.
bool os_dir_list(const char* dir, bool sizes, void (*fn)(void* user, const OsDirEntry& e), void* user, i32* err);
// Creates one directory. True if it was made or a directory is already there.
bool os_make_dir(const char* path, i32* err = nullptr);
// Removes a file or an empty directory. True if it was removed.
bool os_remove(const char* path, i32* err = nullptr);
// Removes an empty directory, never a file or what a link points at. True if no directory
// is there after (removed, or nothing was there); false for a file, a link, or a
// directory with something in it. One call where the caller knows what it has: POSIX's
// remove() tries a directory as a file first.
bool os_remove_dir(const char* path, i32* err = nullptr);
// A new, empty directory under the OS's temporary directory whose name starts with
// prefix; its path, with '/' separators and none at the end, in out.
bool os_make_temp_dir(const char* prefix, char* out, u32 cap, i32* err = nullptr);
// The OS's temporary directory itself: its path, '/' separators, none at the end.
bool os_temp_dir(char* out, u32 cap, i32* err = nullptr);

} // namespace kv
