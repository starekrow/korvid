// Korvid platform: watching directory trees. A watcher is told its roots and polled; it
// never blocks and never spawns a thread, as the sockets do not. Whoever owns one polls
// it once a tick.
//
// Two backends. The native one asks the OS (inotify on Linux, ReadDirectoryChangesW on
// Windows, FSEvents on macOS) and hears of a change as it is made. The polling one walks the trees at an interval and compares sizes and
// modification times with the walk before: slower to notice, correct everywhere, and all
// there is on a platform with no native backend yet.
//
// What a caller may rely on, with either backend:
// - Paths are relative to their root, '/' separated, valid until the next poll.
// - Nothing is reported for what a root holds when it is added: the caller scans.
// - A file that appears is Added (a rename onto an existing file too: check what you
//   hold); one written to is Changed, perhaps many times; one that goes is Removed.
// - A directory that appears is DirAdded, followed by Added for every file already in
//   it. One that goes is DirRemoved, and that is all: files under a directory that was
//   moved away are not reported one by one.
// - Rescan means events were lost (the OS's queue overflowed): scan the root again.
// - Events are a hint that something happened to a path, not a transcript. Compare
//   content before believing a change.
#pragma once
#include "korvid/core/array.h"

namespace kv {

enum class FsEventKind : u8 { Added, Changed, Removed, DirAdded, DirRemoved, Rescan };
const char* fs_event_kind_name(FsEventKind k);

struct FsEvent {
    FsEventKind kind;
    u32         root;   // the id add_root returned
    const char* path;   // relative to the root; "" for Rescan
};

enum class FsWatchBackend : u8 { Auto, Native, Poll };

struct FsWatchState;

struct FsWatcher {
    FsWatchState* state = nullptr;

    // Auto is the native backend where there is one. Asking for Native where there is
    // none fails. poll_interval is the polling backend's time between walks.
    bool init(FsWatchBackend backend = FsWatchBackend::Auto, f64 poll_interval = 0.25);
    void free();
    bool is_open() const { return state != nullptr; }
    const char* backend_name() const; // "inotify", "readdirectorychanges", "fsevents", "poll"
    // How long after a change the backend may still be about to report it, in seconds,
    // even to poll(true): 0 where the OS reports at once (and for polling, whose poll(true)
    // walks), 0.05 for FSEvents, which gathers. A caller that must have heard everything a
    // process wrote before it exited polls for this long after the exit.
    f64  lateness() const;

    // Watches dir and everything under it. The id, or ~0u if the directory cannot be
    // read. Ids are not reused while the watcher lives.
    u32  add_root(const char* dir);
    void remove_root(u32 id);

    // What has happened since the last poll, with repeats of one (kind, path) folded.
    // With `now`, the polling backend walks at once instead of waiting out its
    // interval: the way to ask "is there anything you have not told me".
    Span<const FsEvent> poll(bool now = false);
};

// True where FsWatchBackend::Native exists.
bool fs_watch_has_native();

} // namespace kv
