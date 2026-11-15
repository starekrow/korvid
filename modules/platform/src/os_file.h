// Korvid platform: files and directories from the OS, one source per OS (posix/file_posix.cc,
// win32/file_win32.cc). Paths are UTF-8 with '/' separators. Errors are errno values on
// every platform: they are what IoCompletion::error carries and what strerror describes.
#pragma once
#include "korvid/platform/io.h"

namespace kv {

// Opens for reading, or for writing (created, or truncated if it exists). -1 with *err set
// on failure. The file is not inherited by child processes.
OsFile os_file_open(const char* path, bool write, i32* err);
void   os_file_close(OsFile f);
bool   os_file_size(OsFile f, u64* size, i32* err);
// Up to n bytes at offset `at`, without moving any file position: the count, 0 at the end,
// or -1 with *err set. Interrupted reads are retried.
i64    os_file_read_at(OsFile f, void* buf, usize n, u64 at, i32* err);
// Up to n bytes at the file's position: the count, or -1 with *err set.
i64    os_file_write(OsFile f, const void* buf, usize n, i32* err);

enum class OsPathKind : u8 { None, File, Dir, Other };
// What a path names, following links.
OsPathKind os_path_kind(const char* path);
// What a path names with its size and modification time (nanoseconds since the OS's
// epoch: only ever compared with another), following links. False if nothing is there.
struct OsStat { OsPathKind kind = OsPathKind::None; u64 size = 0; i64 mtime_ns = 0; };
bool os_path_stat(const char* path, OsStat* out);
// Renames a file over whatever is at `to`.
bool os_rename(const char* from, const char* to);
// The working directory, '/' separated.
bool os_current_dir(char* out, u32 cap);
// Calls fn with each entry's name in dir, '.' and '..' left out, in no particular order.
// False if dir cannot be read.
bool os_dir_each(const char* dir, void (*fn)(void* user, const char* name), void* user);
// The same with what os_path_stat would say of each entry, for walks that would otherwise
// ask it of every one: on Windows the listing itself carries it, no query a file (a
// symbolic link is still asked, so it is followed); elsewhere one stat an entry.
bool os_dir_each_stat(const char* dir, void (*fn)(void* user, const char* name, const OsStat& st), void* user);
// Creates one directory. True if it was made or a directory is already there.
bool os_make_dir(const char* path);
// Removes a file or an empty directory. True if it was removed.
bool os_remove(const char* path);
// Removes an empty directory, never a file or what a link points at. True if no directory
// is there after (removed, or nothing was there); false for a file, a link, or a
// directory with something in it. One call where the caller knows what it has: POSIX's
// remove() tries a directory as a file first.
bool os_remove_dir(const char* path);
// A new, empty directory under the OS's temporary directory whose name starts with
// prefix; its path, with '/' separators and none at the end, in out.
bool os_make_temp_dir(const char* prefix, char* out, u32 cap);

} // namespace kv
