// The watcher on Windows: ReadDirectoryChangesW. One handle per root watches the whole tree
// under it (bWatchSubtree), overlapped, so a poll only asks whether the read has finished
// and never waits; a finished read is taken apart and the next one started at once, so
// what happens between polls queues in the OS's buffer, as inotify's does.
//
// Windows says what changed by name but not whether the name was a directory, and once a
// thing is gone it cannot be asked. So each root keeps the directories it knows of: a name
// that was one is reported DirRemoved, a directory's own "modified" (its listing changed)
// is not reported at all, and a directory that appears is walked for what it already holds
// (the contract: DirAdded, then Added for every file in it). The OS's buffer overflowing
// (a read that finishes with nothing in it) is Rescan, as inotify's IN_Q_OVERFLOW is.
#include "../fs_watch_internal.h"
#include "../os_file.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/platform/module.h"
#include "wide.h"
#include <new>
#include <string.h>

namespace kv {

namespace {

constexpr DWORD kBufBytes = 64 * 1024; // what one read can hold; the OS's limit for a network share
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

char* win_dup_str(const char* s) {
    usize n = strlen(s) + 1;
    char* p = (char*)mem_alloc(n);
    memcpy(p, s, n);
    return p;
}

} // namespace

struct FsWinRoot {
    u32         id = 0;
    char*       dir = nullptr;       // as given, without a trailing '/'
    HANDLE      handle = INVALID_HANDLE_VALUE;
    OVERLAPPED  ov{};
    void*       buf = nullptr;       // kBufBytes, DWORD aligned, owned by the pending read
    bool        pending = false;
    bool        gone = false;        // the root itself went: reported, nothing more to read
    Array<char*> dirs;               // directories under the root, relative, '/' separated
    Map<u64, u32> dir_at;            // a directory's path hash -> its index in dirs: a big tree's
                                     // thousands of directories are not each compared with all
};

struct FsNative {
    Array<FsWinRoot*> roots;
};

namespace {

bool is_known_dir(const FsWinRoot& r, const char* rel) {
    const u32* at = r.dir_at.find(hash_str(rel));
    if (!at) return false;
    if (strcmp(r.dirs.data[*at], rel) == 0) return true;
    for (char* d : r.dirs) if (strcmp(d, rel) == 0) return true; // two paths, one hash: look the long way
    return false;
}

void add_dir(FsWinRoot& r, const char* rel) {
    if (is_known_dir(r, rel)) return;
    u64 h = hash_str(rel);
    if (!r.dir_at.has(h)) r.dir_at.set(h, r.dirs.len);
    r.dirs.push(win_dup_str(rel));
}

void index_dirs(FsWinRoot& r) {
    r.dir_at.clear();
    for (u32 i = 0; i < r.dirs.len; ++i) { u64 h = hash_str(r.dirs.data[i]); if (!r.dir_at.has(h)) r.dir_at.set(h, i); }
}

// Forgets rel and every directory under it. Rare (a directory went), so the index is
// simply made again after.
void drop_dirs(FsWinRoot& r, const char* rel) {
    usize n = strlen(rel);
    bool dropped = false;
    for (u32 i = r.dirs.len; i-- > 0;) {
        char* d = r.dirs.data[i];
        if (strncmp(d, rel, n) != 0 || (d[n] != 0 && d[n] != '/')) continue;
        mem_free(d);
        r.dirs.data[i] = r.dirs.data[r.dirs.len - 1];
        --r.dirs.len;
        dropped = true;
    }
    if (dropped) index_dirs(r);
}

void forget_dirs(FsWinRoot& r) {
    for (char* d : r.dirs) mem_free(d);
    r.dirs.len = 0;
    r.dir_at.clear();
}

struct WalkInto { FsWinRoot* root; const char* prefix; FsSink* sink; };

// What fs_walk finds under a directory that appeared: remembered, and said when there is
// a sink.
void walk_entry(void* user, const FsWalkEntry& e) {
    WalkInto* w = (WalkInto*)user;
    char rel[KV_PLATFORM_PATH_MAX * 2];
    usize pl = strlen(w->prefix);
    if (pl + 1 + strlen(e.path) + 1 > sizeof rel) return;
    if (pl) { memcpy(rel, w->prefix, pl); rel[pl] = '/'; memcpy(rel + pl + 1, e.path, strlen(e.path) + 1); }
    else memcpy(rel, e.path, strlen(e.path) + 1);
    if (e.is_dir) {
        add_dir(*w->root, rel);
        if (w->sink) w->sink->emit(FsEventKind::DirAdded, w->root->id, rel, nullptr);
    } else if (w->sink) {
        w->sink->emit(FsEventKind::Added, w->root->id, rel, nullptr);
    }
}

// Walks root/rel ("" for the root) and remembers its directories; with a sink, says what is there.
void walk_under(FsWinRoot& r, const char* rel, FsSink* sink) {
    char abs[KV_PLATFORM_PATH_MAX * 2];
    usize dl = strlen(r.dir), rl = strlen(rel);
    if (dl + 1 + rl + 1 > sizeof abs) return;
    memcpy(abs, r.dir, dl);
    if (rl) { abs[dl] = '/'; memcpy(abs + dl + 1, rel, rl + 1); }
    else abs[dl] = 0;
    WalkInto w{&r, rel, sink};
    fs_walk(abs, walk_entry, &w);
}

// Starts the next read. False if the OS will not (the root went, or the handle is bad).
bool arm(FsWinRoot& r) {
    r.pending = false;
    ResetEvent(r.ov.hEvent);
    if (!ReadDirectoryChangesW(r.handle, r.buf, kBufBytes, TRUE, kFilter, nullptr, &r.ov, nullptr)) return false;
    r.pending = true;
    return true;
}

void close_root(FsWinRoot* r) {
    if (r->handle != INVALID_HANDLE_VALUE) {
        if (r->pending) {
            CancelIoEx(r->handle, &r->ov);
            DWORD n = 0;
            GetOverlappedResult(r->handle, &r->ov, &n, TRUE); // the OS lets go of the buffer
        }
        CloseHandle(r->handle);
    }
    if (r->ov.hEvent) CloseHandle(r->ov.hEvent);
    forget_dirs(*r);
    r->dirs.free();
    r->dir_at.free();
    if (r->buf) mem_free(r->buf);
    if (r->dir) mem_free(r->dir);
    r->~FsWinRoot();
    mem_free(r);
}

bool root_still_there(const FsWinRoot& r) { return os_path_kind(r.dir) == OsPathKind::Dir; }

// The records of one finished read, in the order the OS wrote them.
void take(FsWinRoot& r, DWORD bytes, FsSink& sink) {
    const u8* p = (const u8*)r.buf;
    const u8* end = p + bytes;
    for (;;) {
        const FILE_NOTIFY_INFORMATION* fi = (const FILE_NOTIFY_INFORMATION*)p;
        if ((const u8*)fi + sizeof(FILE_NOTIFY_INFORMATION) > end) return;
        wchar_t wname[KV_WIDE_PATH_MAX];
        u32 wn = fi->FileNameLength / sizeof(wchar_t);
        char rel[KV_PLATFORM_PATH_MAX * 2];
        bool named = wn > 0 && wn < KV_WIDE_PATH_MAX;
        if (named) {
            memcpy(wname, fi->FileName, wn * sizeof(wchar_t));
            wname[wn] = 0;
            named = narrow(wname, rel, sizeof rel);
            if (named) forward_slashes(rel);
        }
        if (named) {
            switch (fi->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME: {
                char abs[KV_PLATFORM_PATH_MAX * 2];
                format(abs, sizeof abs, "%s/%s", r.dir, rel);
                OsPathKind k = os_path_kind(abs);
                if (k == OsPathKind::Dir) {
                    add_dir(r, rel);
                    sink.emit(FsEventKind::DirAdded, r.id, rel, nullptr);
                    walk_under(r, rel, &sink); // what landed in it before this was read
                } else if (k == OsPathKind::File) {
                    sink.emit(FsEventKind::Added, r.id, rel, nullptr);
                }
                // Neither: gone again already, and its removal follows.
                break;
            }
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME:
                if (is_known_dir(r, rel)) {
                    drop_dirs(r, rel);
                    sink.emit(FsEventKind::DirRemoved, r.id, rel, nullptr);
                } else {
                    sink.emit(FsEventKind::Removed, r.id, rel, nullptr);
                }
                break;
            case FILE_ACTION_MODIFIED:
                if (!is_known_dir(r, rel)) sink.emit(FsEventKind::Changed, r.id, rel, nullptr); // a directory's listing changed: its entries say what
                break;
            default: break;
            }
        }
        if (!fi->NextEntryOffset) return;
        p += fi->NextEntryOffset;
    }
}

// Everything the OS has finished for one root, reading until a read is still pending.
void poll_root(FsWinRoot& r, FsSink& sink) {
    for (u32 rounds = 0; rounds < 64 && !r.gone; ++rounds) {
        if (!r.pending && !arm(r)) {
            if (!root_still_there(r)) { r.gone = true; sink.emit(FsEventKind::DirRemoved, r.id, nullptr, nullptr); }
            return;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(r.handle, &r.ov, &bytes, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return; // nothing yet
            r.pending = false;
            if (e == ERROR_NOTIFY_ENUM_DIR) { // the OS's buffer filled: look at everything again
                forget_dirs(r);
                walk_under(r, "", nullptr);
                sink.emit(FsEventKind::Rescan, r.id, nullptr, nullptr);
                continue;
            }
            if (!root_still_there(r)) { r.gone = true; sink.emit(FsEventKind::DirRemoved, r.id, nullptr, nullptr); }
            return;
        }
        r.pending = false;
        if (bytes == 0) { // finished with nothing: the same overflow, said differently
            forget_dirs(r);
            walk_under(r, "", nullptr);
            sink.emit(FsEventKind::Rescan, r.id, nullptr, nullptr);
            continue;
        }
        take(r, bytes, sink);
    }
}

} // namespace

const char* fs_native_name() { return "readdirectorychanges"; }

FsNative* fs_native_create() { return new (mem_alloc(sizeof(FsNative))) FsNative{}; }

void fs_native_destroy(FsNative* n) {
    if (!n) return;
    for (FsWinRoot* r : n->roots) close_root(r);
    n->roots.free();
    mem_free(n);
}

bool fs_native_add(FsNative* n, u32 root, const char* dir) {
    usize len = strlen(dir);
    while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) --len;
    FsWinRoot* r = new (mem_alloc(sizeof(FsWinRoot))) FsWinRoot{};
    r->id  = root;
    r->dir = (char*)mem_alloc(len + 1);
    memcpy(r->dir, dir, len);
    r->dir[len] = 0;
    wchar_t wdir[KV_WIDE_PATH_MAX];
    if (widen(r->dir, wdir, KV_WIDE_PATH_MAX)) {
        // Sharing everything, delete included: watching a tree must not stop anyone
        // removing or renaming what is in it.
        r->handle = CreateFileW(wdir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    }
    r->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->buf = mem_alloc_aligned(kBufBytes, 8);
    if (r->handle == INVALID_HANDLE_VALUE || !r->ov.hEvent || !r->buf) { close_root(r); return false; }
    walk_under(*r, "", nullptr); // the directories already there; nothing is said about them
    if (!arm(*r)) { close_root(r); return false; }
    n->roots.push(r);
    return true;
}

void fs_native_remove(FsNative* n, u32 root) {
    for (u32 i = 0; i < n->roots.len; ++i) {
        if (n->roots.data[i]->id != root) continue;
        close_root(n->roots.data[i]);
        n->roots.data[i] = n->roots.data[n->roots.len - 1];
        --n->roots.len;
        return;
    }
}

void fs_native_poll(FsNative* n, FsSink& sink) {
    for (FsWinRoot* r : n->roots) poll_root(*r, sink);
}

} // namespace kv
