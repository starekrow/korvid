// The watcher on macOS: FSEvents, file events. One stream per root on a private serial
// dispatch queue; its callback only queues what it hears (under a lock, in plain malloc'd
// memory: it runs on GCD's thread, not the engine's), and poll turns the queue into events
// on the caller's thread. iOS has no FSEvents and polls.
//
// What FSEvents is not, and what is done about it:
// - Its paths are absolute and canonical (a root under /var/folders is reported under
//   /private/var/folders, in the case on disk): a root is matched by its own canonical
//   path, F_GETPATH's.
// - It coalesces: one event's flags can say a file was created, written and removed since
//   the last, and the flags stay with a path (a write to a file made long ago can still say
//   "created"). What the path is now decides: there, it is Added when it is new since the
//   root was added (born after it) or renamed onto, and Changed when written; gone, it is
//   Removed (after an Added, when it was created in between: scratch made and taken away is
//   two facts).
// - It reports a little behind the writes, so a stream started just after a file was written
//   can hear of it: what has not changed since its root was added (its status-change time is
//   older) is history, and not news.
// - A directory that appears may already hold files: it is walked, as inotify's backend
//   does, and they are Added.
// - Dropped events (its queue or the kernel's overflowed) are Rescan.
// - It reports about 11 ms after a write (fseventsd gathers for about 10 before it passes
//   anything on), and FSEventStreamFlushSync does not shorten that (measured: the same 11 to
//   13 ms, at 3.6 ms a call). So nothing flushes: FsWatcher::lateness says 50 ms instead,
//   for a caller that must have heard everything a process wrote before it exited.
#include "../fs_watch_internal.h"
#include "../os_file.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include "korvid/core/memory.h"
#include "korvid/platform/module.h"
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <new>
#include <os/lock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace kv {

namespace {

// What a callback heard, queued for poll. From the general allocator, which is
// thread-safe: the callback is on GCD's thread. Out of memory, the change is dropped and
// said once; the next poll still sees what was queued.
struct FsevHeard { u32 root; u32 flags; u32 text; };
struct FsevQueue {
    FsevHeard* list = nullptr;
    u32    len = 0, cap = 0;
    char*  text = nullptr;
    u32    text_len = 0, text_cap = 0;
    bool   told = false;

    void push(u32 root, u32 flags, const char* path) {
        u32 n = (u32)strlen(path) + 1;
        if (len == cap) {
            u32 c = cap ? cap * 2 : 64;
            FsevHeard* l = (FsevHeard*)mem_realloc(list, sizeof(FsevHeard) * c);
            if (!l) { dropped(path); return; }
            list = l; cap = c;
        }
        while (text_len + n > text_cap) {
            u32 c = text_cap ? text_cap * 2 : 4096;
            char* t = (char*)mem_realloc(text, c);
            if (!t) { dropped(path); return; }
            text = t; text_cap = c;
        }
        memcpy(text + text_len, path, n);
        list[len++] = FsevHeard{root, flags, text_len};
        text_len += n;
    }
    void clear() { len = 0; text_len = 0; }
    void release() { mem_free(list); mem_free(text); *this = FsevQueue{}; }
    void dropped(const char* path) {
        if (!told) KV_WARN("fs_watch", "out of memory: a change to '%s' (and any after it) not queued", path);
        told = true;
    }
};

} // namespace

struct FsevRoot {
    u32              id = 0;
    char*            dir = nullptr; // canonical, no trailing '/'
    u32              dir_len = 0;
    timespec         added = {};        // when the root was added: older is history
    FSEventStreamRef stream = nullptr;
    struct FsNative* owner = nullptr;
};

struct FsNative {
    dispatch_queue_t  queue = nullptr;
    os_unfair_lock    lock = OS_UNFAIR_LOCK_INIT; // guards heard
    FsevQueue        heard, taking;              // taking: poll's side of the swap
    Array<FsevRoot*>  roots;
};

namespace {

void fsev_heard(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
    FsevRoot* r = (FsevRoot*)info;
    FsNative* n = r->owner;
    const char* const* list = (const char* const*)paths;
    os_unfair_lock_lock(&n->lock);
    for (size_t i = 0; i < count; ++i) n->heard.push(r->id, (u32)flags[i], list[i]);
    os_unfair_lock_unlock(&n->lock);
}

void fsev_nothing(void*) {}

// The directory's own path as the file system holds it, which is how FSEvents names it.
char* fsev_canonical(const char* dir) {
    char path[PATH_MAX];
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok = fd >= 0 && fcntl(fd, F_GETPATH, path) == 0;
    if (fd >= 0) close(fd);
    if (!ok && !realpath(dir, path)) return nullptr;
    usize len = strlen(path);
    while (len > 1 && path[len - 1] == '/') --len;
    char* copy = (char*)mem_alloc(len + 1);
    memcpy(copy, path, len);
    copy[len] = 0;
    return copy;
}

void fsev_stop(FsNative* n, FsevRoot* r) {
    if (r->stream) {
        FSEventStreamStop(r->stream);
        FSEventStreamInvalidate(r->stream);
        dispatch_sync_f(n->queue, nullptr, fsev_nothing); // a callback already running finishes first
        FSEventStreamRelease(r->stream);
        r->stream = nullptr;
    }
}

struct FsevWalk { FsSink* sink; u32 root; const char* rel; };
void fsev_walk_added(void* user, const FsWalkEntry& e) {
    FsevWalk& w = *(FsevWalk*)user;
    w.sink->emit(e.is_dir ? FsEventKind::DirAdded : FsEventKind::Added, w.root, w.rel, e.path);
}

} // namespace

const char* fs_native_name() { return "fsevents"; }
i64         fs_native_fd(FsNative*) { return -1; } // its callbacks queue; poll takes them

FsNative* fs_native_create() {
    FsNative* n = new (mem_alloc(sizeof(FsNative))) FsNative{};
    n->queue = dispatch_queue_create("korvid.fs_watch", DISPATCH_QUEUE_SERIAL);
    if (!n->queue) { mem_free(n); return nullptr; }
    return n;
}

void fs_native_destroy(FsNative* n) {
    if (!n) return;
    for (FsevRoot* r : n->roots) { fsev_stop(n, r); mem_free(r->dir); mem_free(r); }
    n->roots.free();
    dispatch_release(n->queue);
    n->heard.release();
    n->taking.release();
    n->~FsNative();
    mem_free(n);
}

bool fs_native_add(FsNative* n, u32 root, const char* dir) {
    char* canon = fsev_canonical(dir);
    if (!canon) return false;
    FsevRoot* r = new (mem_alloc(sizeof(FsevRoot))) FsevRoot{};
    r->id = root;
    r->dir = canon;
    r->dir_len = (u32)strlen(canon);
    r->owner = n;
    clock_gettime(CLOCK_REALTIME, &r->added);
    FSEventStreamContext ctx{0, r, nullptr, nullptr, nullptr};
    CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault, canon, kCFStringEncodingUTF8);
    CFArrayRef paths = path ? CFArrayCreate(kCFAllocatorDefault, (const void**)&path, 1, &kCFTypeArrayCallBacks) : nullptr;
    // File events, delivered as soon as they are seen (no deferral, a short latency to
    // gather a burst), and the root's own fate.
    if (paths)
        r->stream = FSEventStreamCreate(kCFAllocatorDefault, fsev_heard, &ctx, paths, kFSEventStreamEventIdSinceNow, 0.01,
                                        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    if (paths) CFRelease(paths);
    if (path) CFRelease(path);
    if (r->stream) {
        FSEventStreamSetDispatchQueue(r->stream, n->queue);
        if (!FSEventStreamStart(r->stream)) { FSEventStreamInvalidate(r->stream); FSEventStreamRelease(r->stream); r->stream = nullptr; }
    }
    if (!r->stream) {
        KV_WARN("platform", "fs_watch: FSEvents would not watch '%s'", canon);
        mem_free(r->dir);
        mem_free(r);
        return false;
    }
    n->roots.push(r);
    return true;
}

void fs_native_remove(FsNative* n, u32 root) {
    for (u32 i = 0; i < n->roots.len; ++i) {
        FsevRoot* r = n->roots.data[i];
        if (r->id != root) continue;
        fsev_stop(n, r);
        mem_free(r->dir);
        mem_free(r);
        n->roots.data[i] = n->roots.data[n->roots.len - 1];
        --n->roots.len;
        return;
    }
}

void fs_native_poll(FsNative* n, FsSink& sink) {
    os_unfair_lock_lock(&n->lock);
    FsevQueue got = n->heard;
    n->heard = n->taking;
    os_unfair_lock_unlock(&n->lock);
    n->taking = got;
    for (u32 i = 0; i < got.len; ++i) {
        const FsevHeard& h = got.list[i];
        const char* abs = got.text + h.text;
        FsevRoot* r = nullptr;
        for (FsevRoot* x : n->roots) if (x->id == h.root) r = x;
        if (!r) continue; // a root already removed
        u32 f = h.flags;
        if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
            sink.emit(FsEventKind::Rescan, r->id, nullptr, nullptr);
            continue;
        }
        bool is_root = strncmp(abs, r->dir, r->dir_len) == 0 && abs[r->dir_len] == 0;
        if (f & kFSEventStreamEventFlagRootChanged || is_root) {
            // The root itself: gone, or moved away.
            if (os_path_kind(r->dir) != OsPathKind::Dir) sink.emit(FsEventKind::DirRemoved, r->id, nullptr, nullptr);
            continue;
        }
        if (strncmp(abs, r->dir, r->dir_len) != 0 || abs[r->dir_len] != '/') continue; // not under it (another name for it)
        const char* rel = abs + r->dir_len + 1;
        struct stat st;
        bool there = lstat(abs, &st) == 0;
        auto older = [&](const timespec& t) { return t.tv_sec < r->added.tv_sec || (t.tv_sec == r->added.tv_sec && t.tv_nsec < r->added.tv_nsec); };
        if (there && older(st.st_ctimespec)) continue; // nothing has happened to it since the root was added
        // New to this root: made since it was added, or renamed onto its path.
        bool created = (f & kFSEventStreamEventFlagItemRenamed) || ((f & kFSEventStreamEventFlagItemCreated) && (!there || !older(st.st_birthtimespec)));
        if (f & kFSEventStreamEventFlagItemIsDir) {
            if (there && S_ISDIR(st.st_mode) && created) {
                sink.emit(FsEventKind::DirAdded, r->id, nullptr, rel);
                FsevWalk w{&sink, r->id, rel};
                fs_walk(abs, fsev_walk_added, &w);
            } else if (!there && (f & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed))) {
                sink.emit(FsEventKind::DirRemoved, r->id, nullptr, rel);
            }
            continue;
        }
        if (!(f & (kFSEventStreamEventFlagItemIsFile | kFSEventStreamEventFlagItemIsSymlink))) continue;
        if (there && S_ISLNK(st.st_mode) && os_path_is_dir_link(abs)) continue; // not reported, not gone down (KR16)
        if (there && !S_ISDIR(st.st_mode)) {
            if (created) sink.emit(FsEventKind::Added, r->id, nullptr, rel);
            if (f & kFSEventStreamEventFlagItemModified) sink.emit(FsEventKind::Changed, r->id, nullptr, rel);
        } else if (!there) {
            if (f & kFSEventStreamEventFlagItemCreated) sink.emit(FsEventKind::Added, r->id, nullptr, rel); // made and taken away
            if (f & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed | kFSEventStreamEventFlagItemCreated))
                sink.emit(FsEventKind::Removed, r->id, nullptr, rel);
        }
    }
    n->taking.clear();
}

} // namespace kv
