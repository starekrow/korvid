// Korvid platform: watching directory trees. A watcher is told its roots and polled; it
// never blocks and never spawns a thread of its own, as the sockets do not (set_wake's
// calls come on the OS's). Whoever owns one polls it once a tick.
//
// Two backends. The native one asks the OS (inotify on Linux, ReadDirectoryChangesW on
// Windows, FSEvents on macOS) and hears of a change as it is made. The polling one walks the trees at an interval and compares sizes and
// modification times with the walk before: slower to notice, correct everywhere, and all
// there is on a platform with no native backend yet.
//
// The choice is a root's. On a share (io_path_is_remote: NFS, SMB, a FUSE mount) the
// OS reports only what this machine wrote, so a watcher made with Auto polls a root that
// is on one, every remote_interval seconds, and watches its other roots natively. A root
// can also be polled by name, for a share the test does not know. A polled root is walked
// on the thread that calls poll: whoever watches a large share polls from a thread of
// their own.
//
// What a caller may rely on, with either backend:
// - Paths are relative to their root, '/' separated, valid until the next poll.
// - Nothing is reported for what a root holds when it is added: the caller scans.
// - A file that appears is Added (a rename onto an existing file too: check what you
//   hold); one written to is Changed, perhaps many times; one that goes is Removed.
// - A directory that appears is DirAdded, followed by Added for every file already in
//   it. One that goes is DirRemoved, and that is all: files under a directory that was
//   moved away are not reported one by one.
// - A link to a directory is not gone down, and is not reported: a link inside a root to
//   the root, or to a tree elsewhere, adds nothing, as a walk with io_list_dir would not
//   follow it. Its removal may still come as a Removed of a path never Added. A link to a
//   file is reported as the file. A root that is itself a link is followed.
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
    // none fails. poll_interval is the polling backend's time between walks of a root;
    // remote_interval is the time between walks of a share's root that Auto chose to poll.
    bool init(FsWatchBackend backend = FsWatchBackend::Auto, f64 poll_interval = 0.25, f64 remote_interval = 5.0);
    void free();
    bool is_open() const { return state != nullptr; }
    const char* backend_name() const; // "inotify", "readdirectorychanges", "fsevents", "poll": the watcher's; root_backend says a root's
    // How long after a change the backend may still be about to report it, in seconds,
    // even to poll(true): 0 where the OS reports at once (and for polling, whose poll(true)
    // walks), 0.05 for FSEvents, which gathers. A caller that must have heard everything a
    // process wrote before it exited polls for this long after the exit.
    f64  lateness() const;
    // For a loop that waits (KR26): a descriptor that can be read when the native backend
    // has news (inotify's), else -1; and the time (time_now's clock) by which poll() should
    // next be called for news that no descriptor or wake tells of (a polled root's next
    // walk, or soon when the native backend has neither), 0 when there is none.
    i64  wake_fd() const;
    f64  due() const;
    // The other way a native backend can tell a waiting loop (KR26): fn(user) is called, on
    // a thread of the OS's (ReadDirectoryChangesW's wait, FSEvents' queue), when it has news
    // for poll(). Once for each root's news until the poll after it takes it, so nobody
    // polling does not mean calls for ever. True where the backend calls it; false where
    // wake_fd() says instead (inotify) or no root is native, and nothing is called. Null
    // clears it: once that returns, no call is under way and none is to come. Set it again
    // after init; free() clears it.
    bool set_wake(void (*fn)(void* user), void* user);

    // Watches dir and everything under it. The id, or ~0u if the directory cannot be
    // read. Ids are not reused while the watcher lives. `how`: Auto is the watcher's own
    // backend, except that a watcher made with Auto polls a root on a share; Poll polls
    // this root whatever the watcher is, at poll_interval; Native insists, and fails on a
    // watcher that has none.
    u32  add_root(const char* dir, FsWatchBackend how = FsWatchBackend::Auto);
    void remove_root(u32 id);
    // What a root got: Native or Poll. Auto for an id that is not a root.
    FsWatchBackend root_backend(u32 id) const;

    // What has happened since the last poll, with repeats of one (kind, path) folded.
    // With `now`, the polling backend walks at once instead of waiting out its
    // interval: the way to ask "is there anything you have not told me".
    Span<const FsEvent> poll(bool now = false);
};

// True where FsWatchBackend::Native exists.
bool fs_watch_has_native();

} // namespace kv
