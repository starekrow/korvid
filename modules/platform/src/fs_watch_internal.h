// Korvid platform: what the watcher's backends share. An event sink that folds repeats,
// and the seam a native backend fills (linux/fs_watch_linux.cc, win32/fs_watch_win32.cc,
// macos/fs_watch_macos.cc).
#pragma once
#include "korvid/platform/fs_watch.h"

namespace kv {

struct FsSink {
    struct Ev { FsEventKind kind; u32 root; u32 text; };
    Array<Ev>   evs;
    Array<char> text;

    void clear() { evs.len = 0; text.len = 0; }
    void free()  { evs.free(); text.free(); }
    // dir and name are joined with a slash when both are non-empty.
    void emit(FsEventKind kind, u32 root, const char* dir, const char* name);
};

// Calls fn for every file and directory under dir, depth first, parents before children,
// with paths relative to dir. Skips nothing: the caller decides what a name means.
struct FsWalkEntry { const char* path; bool is_dir; u64 size; i64 mtime_ns; };
void fs_walk(const char* dir, void (*fn)(void* user, const FsWalkEntry& e), void* user);

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32) || (defined(__APPLE__) && TARGET_OS_OSX)
#define KV_FS_NATIVE 1
struct FsNative;
FsNative*   fs_native_create();
void        fs_native_destroy(FsNative* n);
bool        fs_native_add(FsNative* n, u32 root, const char* dir);
void        fs_native_remove(FsNative* n, u32 root);
void        fs_native_poll(FsNative* n, FsSink& sink);
const char* fs_native_name();
// A descriptor that can be read when the backend has news (inotify's), or -1 where its news
// comes some other way (FSEvents', ReadDirectoryChangesW's): then fs_native_set_wake.
i64         fs_native_fd(FsNative* n);
// FsWatcher::set_wake for the native roots: true where the backend calls fn, false where
// it does not (it has a descriptor). Null clears it, and no call is under way after.
bool        fs_native_set_wake(FsNative* n, void (*fn)(void* user), void* user);
// How late the native backend can report a change, in seconds (FsWatcher::lateness).
#if defined(__APPLE__)
#define KV_FS_NATIVE_LATENESS 0.05 // FSEvents: about 11 ms, measured; four times that
#else
#define KV_FS_NATIVE_LATENESS 0.0  // inotify, ReadDirectoryChangesW: at once
#endif
#else
#define KV_FS_NATIVE 0
#endif

} // namespace kv
