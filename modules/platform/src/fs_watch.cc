#include "korvid/platform/fs_watch.h"
#include "fs_watch_internal.h"
#include "os_file.h"
#include "korvid/core/hash.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/core/sort.h"
#include "korvid/core/time.h"
#include "korvid/platform/module.h"
#include <new>
#include <errno.h>
#include <string.h>

namespace kv {

const char* fs_event_kind_name(FsEventKind k) {
    switch (k) {
    case FsEventKind::Added:      return "added";
    case FsEventKind::Changed:    return "changed";
    case FsEventKind::Removed:    return "removed";
    case FsEventKind::DirAdded:   return "dir-added";
    case FsEventKind::DirRemoved: return "dir-removed";
    case FsEventKind::Rescan:     return "rescan";
    }
    return "?";
}

// ---- the sink ------------------------------------------------------------------------

void FsSink::emit(FsEventKind kind, u32 root, const char* dir, const char* name) {
    u32 dl = dir ? (u32)strlen(dir) : 0, nl = name ? (u32)strlen(name) : 0;
    u32 at = text.len;
    char* p = text.push_n(dl + nl + 2);
    u32 n = 0;
    if (dl) { memcpy(p, dir, dl); n = dl; }
    if (dl && nl) p[n++] = '/';
    if (nl) { memcpy(p + n, name, nl); n += nl; }
    p[n] = 0;
    text.len = at + n + 1;
    // A repeat of the last thing said about this path says nothing new. Only the last:
    // added, removed, added is three facts.
    for (u32 i = evs.len; i-- > 0;) {
        const Ev& e = evs.data[i];
        if (e.root != root || strcmp(text.data + e.text, p) != 0) continue;
        if (e.kind == kind) { text.len = at; return; }
        break;
    }
    evs.push(Ev{kind, root, at});
}

// ---- walking -------------------------------------------------------------------------

namespace {

// A directory's entries with what each is, its size and time: from the listing itself
// (os_dir_list), where asking of each entry by its path was most of a walk's time on
// Windows, a watch's start included. A link to a directory is left out, so a walk never
// goes down one (a link inside a root to the root was the tree again, down to the length
// limit); a link to a file is the file.
struct NameStat { u32 at; OsStat st; };
struct Names { Array<char> text; Array<NameStat> list; };

void fsw_collect_name(void* user, const OsDirEntry& e) {
    if (e.is_link && e.st.kind == OsPathKind::Dir) return;
    Names* n = (Names*)user;
    u32 len = (u32)strlen(e.name);
    n->list.push(NameStat{n->text.len, e.st});
    memcpy(n->text.push_n(len + 1), e.name, len + 1);
}

struct NameCmp { const char* text; };
int name_cmp(const void* a, const void* b, void* ctx) {
    const char* t = ((NameCmp*)ctx)->text;
    return strcmp(t + ((const NameStat*)a)->at, t + ((const NameStat*)b)->at);
}

void walk_in(char* abs, u32 abs_len, u32 abs_cap, u32 rel_at, void (*fn)(void*, const FsWalkEntry&), void* user, u32 depth) {
    if (depth > 64) return;
    Names names{};
    if (!os_dir_list(abs, true, fsw_collect_name, &names, nullptr)) { names.text.free(); names.list.free(); return; }
    NameCmp cmp{names.text.data};
    sort(names.list.data, names.list.len, sizeof(NameStat), name_cmp, &cmp);
    for (u32 i = 0; i < names.list.len; ++i) {
        const NameStat& ns = names.list.data[i];
        const char* name = names.text.data + ns.at;
        u32 nl = (u32)strlen(name);
        if (abs_len + 1 + nl + 1 > abs_cap) continue; // too deep a path to name: not seen
        if (ns.st.kind != OsPathKind::File && ns.st.kind != OsPathKind::Dir) continue;
        abs[abs_len] = '/';
        memcpy(abs + abs_len + 1, name, nl + 1);
        FsWalkEntry e{abs + rel_at, ns.st.kind == OsPathKind::Dir, ns.st.size, ns.st.mtime_ns};
        fn(user, e);
        if (e.is_dir) walk_in(abs, abs_len + 1 + nl, abs_cap, rel_at, fn, user, depth + 1);
        abs[abs_len] = 0;
    }
    names.text.free();
    names.list.free();
}

} // namespace

void fs_walk(const char* dir, void (*fn)(void* user, const FsWalkEntry& e), void* user) {
    char abs[KV_PLATFORM_PATH_MAX * 2];
    u32 n = (u32)strlen(dir);
    while (n > 1 && dir[n - 1] == '/') --n;
    if (n + 2 > sizeof abs) return;
    memcpy(abs, dir, n);
    abs[n] = 0;
    walk_in(abs, n, (u32)sizeof abs, n + 1, fn, user, 0);
}

// ---- the polling backend -------------------------------------------------------------

struct FsPollEntry { u32 text; bool is_dir; u64 size; i64 mtime_ns; };

struct FsPollRoot {
    u32                id;
    char*              dir;
    f64                interval;  // between walks: the watcher's poll_interval, or remote_interval for a share
    f64                last_walk;
    Array<FsPollEntry> entries; // sorted by path, a directory's contents right after it
    Array<char>        text;
};

namespace {

// Orders paths so that everything under a directory follows it directly: '/' sorts
// before every other byte.
int path_cmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        u8 x = (u8)*a, y = (u8)*b;
        if (x == y) { if (!x) return 0; continue; }
        if (!x) return -1;
        if (!y) return 1;
        if (x == '/') return -1;
        if (y == '/') return 1;
        return x < y ? -1 : 1;
    }
}

struct EntryCmp { const char* text; };
int entry_cmp(const void* a, const void* b, void* ctx) {
    const char* t = ((EntryCmp*)ctx)->text;
    return path_cmp(t + ((const FsPollEntry*)a)->text, t + ((const FsPollEntry*)b)->text);
}

struct Snapshot { Array<FsPollEntry> entries; Array<char> text; };

void snap_entry(void* user, const FsWalkEntry& e) {
    Snapshot* s = (Snapshot*)user;
    u32 len = (u32)strlen(e.path);
    FsPollEntry pe{s->text.len, e.is_dir, e.size, e.mtime_ns};
    memcpy(s->text.push_n(len + 1), e.path, len + 1);
    s->entries.push(pe);
}

void snapshot(const char* dir, Snapshot& s) {
    fs_walk(dir, snap_entry, &s);
    EntryCmp cmp{s.text.data};
    sort(s.entries.data, s.entries.len, sizeof(FsPollEntry), entry_cmp, &cmp);
}

bool under(const char* path, const char* dir) {
    usize n = strlen(dir);
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

void poll_root(FsPollRoot& r, FsSink& sink) {
    Snapshot now{};
    snapshot(r.dir, now);
    u32 i = 0, j = 0;
    const char* gone_dir = nullptr; // the removed directory whose contents are being passed over
    while (i < r.entries.len || j < now.entries.len) {
        const FsPollEntry* a = i < r.entries.len ? &r.entries.data[i] : nullptr;
        const FsPollEntry* b = j < now.entries.len ? &now.entries.data[j] : nullptr;
        const char* ap = a ? r.text.data + a->text : nullptr;
        const char* bp = b ? now.text.data + b->text : nullptr;
        int c = !a ? 1 : !b ? -1 : path_cmp(ap, bp);
        if (c == 0 && a->is_dir != b->is_dir) c = -1; // one kind went and the other came: the going first
        if (c < 0) {
            if (!(gone_dir && under(ap, gone_dir))) {
                sink.emit(a->is_dir ? FsEventKind::DirRemoved : FsEventKind::Removed, r.id, ap, nullptr);
                gone_dir = a->is_dir ? ap : nullptr;
            }
            ++i;
        } else if (c > 0) {
            sink.emit(b->is_dir ? FsEventKind::DirAdded : FsEventKind::Added, r.id, bp, nullptr);
            ++j;
        } else {
            if (!a->is_dir && (a->size != b->size || a->mtime_ns != b->mtime_ns)) sink.emit(FsEventKind::Changed, r.id, bp, nullptr);
            ++i; ++j;
        }
    }
    r.entries.free();
    r.text.free();
    r.entries = now.entries;
    r.text    = now.text;
}

char* fsw_dup_str(const char* s) {
    usize n = strlen(s) + 1;
    char* p = (char*)mem_alloc(n);
    memcpy(p, s, n);
    return p;
}

} // namespace

struct FsWatchState {
    bool            native = false;
    FsWatchBackend  asked = FsWatchBackend::Auto; // what init was given: only Auto polls a share by itself
    f64             remote_interval = 5.0;
#if KV_FS_NATIVE
    FsNative*       n = nullptr;
    bool            wakes = false; // set_wake's fn is called by the native backend
    void          (*wake_fn)(void*) = nullptr; // what set_wake was last given: the same again is no work
    void*           wake_user = nullptr;
#endif
    Array<FsPollRoot> roots;  // the polled roots, under either backend
    Array<u32>      native_roots; // the ids the native backend has
    f64             interval = 0.25;
    u32             next_id = 0;
    FsSink          sink;
    Array<FsEvent>  out;
};

bool fs_watch_has_native() { return KV_FS_NATIVE != 0; }

bool FsWatcher::open(FsWatchBackend backend, f64 poll_interval, f64 remote_interval, i32* err) {
    close();
    bool native = backend != FsWatchBackend::Poll && fs_watch_has_native();
    if (backend == FsWatchBackend::Native && !native) { if (err) *err = ENOTSUP; return false; }
    FsWatchState* s = new (mem_alloc(sizeof(FsWatchState))) FsWatchState{};
    s->native   = native;
    s->asked    = backend;
    s->interval = poll_interval;
    s->remote_interval = remote_interval;
#if KV_FS_NATIVE
    if (native) {
        s->n = fs_native_create();
        if (!s->n) {
            if (backend == FsWatchBackend::Native) { mem_free(s); if (err) *err = ENOTSUP; return false; }
            KV_WARN("platform", "fs_watch: no native watcher (the OS refused one); polling");
            s->native = false;
        }
    }
#endif
    state = s;
    return true;
}

void FsWatcher::close() {
    if (!state) return;
#if KV_FS_NATIVE
    if (state->n) fs_native_destroy(state->n);
#endif
    for (FsPollRoot& r : state->roots) {
        if (r.dir) mem_free(r.dir);
        r.entries.free();
        r.text.free();
    }
    state->roots.free();
    state->native_roots.free();
    state->sink.free();
    state->out.free();
    mem_free(state);
    state = nullptr;
}

f64 FsWatcher::lateness() const {
#if KV_FS_NATIVE
    if (state && state->native) return KV_FS_NATIVE_LATENESS;
#endif
    return 0.0; // the polling backend walks when poll(true) asks
}

const char* FsWatcher::backend_name() const {
    if (!state) return "none";
#if KV_FS_NATIVE
    if (state->native) return fs_native_name();
#endif
    return "poll";
}

u32 FsWatcher::add_root(const char* dir, FsWatchBackend how, i32* err) {
    if (!state || !dir) { if (err) *err = EINVAL; return ~0u; }
    if (os_path_kind(dir) != OsPathKind::Dir) {
        if (err) { OsStat st; i32 why = 0; *err = os_path_stat(dir, &st, &why) ? ENOTDIR : why ? why : ENOENT; }
        return ~0u;
    }
    if (how == FsWatchBackend::Native && !state->native) { if (err) *err = ENOTSUP; return ~0u; }
    bool native = state->native && how != FsWatchBackend::Poll;
    f64 interval = state->interval;
    if (how == FsWatchBackend::Auto && state->asked == FsWatchBackend::Auto && os_path_is_remote(dir)) {
        native   = false;
        interval = state->remote_interval;
        KV_INFO("platform", "fs_watch: %s is on a share; polled every %.1f s", dir, interval);
    }
    u32 id = state->next_id++;
#if KV_FS_NATIVE
    if (native) {
        if (!fs_native_add(state->n, id, dir)) { if (err) *err = EIO; return ~0u; } // the OS refused the watch
        state->native_roots.push(id);
        return id;
    }
#else
    (void)native; // no native backend here (iOS): every root is polled
#endif
    FsPollRoot r{};
    r.id  = id;
    r.dir = fsw_dup_str(dir);
    r.interval = interval;
    Snapshot s{};
    snapshot(dir, s);
    r.entries = s.entries;
    r.text    = s.text;
    r.last_walk = time_now();
    state->roots.push(r);
    return id;
}

void FsWatcher::remove_root(u32 id) {
    if (!state) return;
    for (u32 i = 0; i < state->roots.len; ++i) {
        FsPollRoot& r = state->roots.data[i];
        if (r.id != id) continue;
        mem_free(r.dir);
        r.entries.free();
        r.text.free();
        state->roots.data[i] = state->roots.data[state->roots.len - 1];
        --state->roots.len;
        return;
    }
    for (u32 i = 0; i < state->native_roots.len; ++i) {
        if (state->native_roots.data[i] != id) continue;
#if KV_FS_NATIVE
        fs_native_remove(state->n, id);
#endif
        state->native_roots.data[i] = state->native_roots.data[--state->native_roots.len];
        return;
    }
}

FsWatchBackend FsWatcher::root_backend(u32 id) const {
    if (!state) return FsWatchBackend::Auto;
    for (const FsPollRoot& r : state->roots) if (r.id == id) return FsWatchBackend::Poll;
    for (u32 n : state->native_roots) if (n == id) return FsWatchBackend::Native;
    return FsWatchBackend::Auto;
}

KHandle FsWatcher::wake_handle() const {
#if KV_FS_NATIVE
    if (state && state->native) return khandle_make(KHandleKind::Notify, fs_native_fd(state->n));
#endif
    return KHandle{};
}

f64 FsWatcher::due() const {
    if (!state) return 0.0;
    f64 due = 0.0;
#if KV_FS_NATIVE
    if (state->native && fs_native_fd(state->n) < 0 && !state->wakes) due = time_now() + kPlatformSoon; // found only by asking
#endif
    for (const FsPollRoot& r : state->roots)
        if (due <= 0.0 || r.last_walk + r.interval < due) due = r.last_walk + r.interval;
    return due;
}

bool FsWatcher::set_wake(void (*fn)(void* user), void* user) {
    if (!state) return false;
#if KV_FS_NATIVE
    if (state->native) {
        if (fn == state->wake_fn && user == state->wake_user) return state->wakes; // set already (a watch restated each pass)
        state->wake_fn = fn;
        state->wake_user = user;
        state->wakes = fs_native_set_wake(state->n, fn, user) && fn;
        return state->wakes;
    }
#endif
    (void)fn;
    (void)user;
    return false;
}

Span<const FsEvent> FsWatcher::poll(bool now) {
    if (!state) return {};
    FsWatchState& s = *state;
    s.sink.clear();
    s.out.len = 0;
#if KV_FS_NATIVE
    if (s.native) fs_native_poll(s.n, s.sink);
#endif
    f64 t = time_now();
    for (FsPollRoot& r : s.roots) {
        if (!now && t - r.last_walk < r.interval) continue;
        poll_root(r, s.sink);
        r.last_walk = time_now();
    }
    for (const FsSink::Ev& e : s.sink.evs) s.out.push(FsEvent{e.kind, e.root, s.sink.text.data + e.text});
    return Span<const FsEvent>(s.out.data, s.out.len);
}

} // namespace kv
