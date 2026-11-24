// The watcher on Linux: inotify. One watch per directory, since inotify does not look
// below the directory it is given; a directory that appears is given a watch and then
// read, because files can land in it before the watch does.
#include "../fs_watch_internal.h"
#include "../os_file.h"
#include "korvid/core/log.h"
#include "korvid/core/map.h"
#include "korvid/platform/module.h"
#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace kv {

struct FsInoDir { i32 wd; u32 root; char* rel; }; // rel: "" for the root itself
struct FsInoRoot { u32 id; char* dir; };

namespace {

constexpr u32 kMask = IN_CREATE | IN_CLOSE_WRITE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                      IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;


char* ino_dup_str(const char* s) {
    usize n = strlen(s) + 1;
    char* p = (char*)mem_alloc(n);
    memcpy(p, s, n);
    return p;
}

} // namespace

struct FsNative {
    int              fd = -1;
    Array<FsInoDir>  dirs;
    Map<i32, u32>    by_wd;  // wd -> index in dirs
    Array<FsInoRoot> roots;
};

namespace {

FsInoRoot* root_of(FsNative* n, u32 id) {
    for (FsInoRoot& r : n->roots) if (r.id == id) return &r;
    return nullptr;
}

void drop_dir(FsNative* n, u32 index) {
    FsInoDir& d = n->dirs.data[index];
    n->by_wd.remove(d.wd);
    mem_free(d.rel);
    u32 last = n->dirs.len - 1;
    if (index != last) {
        n->dirs.data[index] = n->dirs.data[last];
        n->by_wd.set(n->dirs.data[index].wd, index);
    }
    --n->dirs.len;
}

// A directory's entries and what each is. A link to a directory is left out: it is not
// watched, and not reported (KR16).
struct Listing { Array<char> text; Array<u32> at; Array<OsPathKind> kind; };
void list_name(void* user, const OsDirEntry& e) {
    if (e.is_link && e.st.kind == OsPathKind::Dir) return;
    Listing* l = (Listing*)user;
    u32 len = (u32)strlen(e.name);
    l->at.push(l->text.len);
    l->kind.push(e.st.kind);
    memcpy(l->text.push_n(len + 1), e.name, len + 1);
}

// Watches root/rel and everything under it. With a sink, says what it finds: the
// directory itself is the caller's to announce.
bool add_tree(FsNative* n, const FsInoRoot& root, const char* rel, FsSink* sink, u32 depth) {
    if (depth > 64) return false;
    char abs[KV_PLATFORM_PATH_MAX * 2];
    int len = snprintf(abs, sizeof abs, rel[0] ? "%s/%s" : "%s", root.dir, rel);
    if (len < 0 || (usize)len >= sizeof abs) return false;
    // A root may be a link, which is followed; nothing under one is (KR16), so a link that
    // took a directory's place after it was listed is not watched either.
    int wd = inotify_add_watch(n->fd, abs, rel[0] ? kMask | IN_DONT_FOLLOW : kMask);
    if (wd < 0) {
        if (errno == ENOSPC) KV_WARN("platform", "fs_watch: out of inotify watches at '%s' (fs.inotify.max_user_watches)", abs);
        return false;
    }
    if (u32* at = n->by_wd.find(wd)) { // known already (a rescan, or the same directory by another name)
        FsInoDir& d = n->dirs.data[*at];
        if (strcmp(d.rel, rel) != 0 || d.root != root.id) { mem_free(d.rel); d.rel = ino_dup_str(rel); d.root = root.id; }
    } else {
        n->by_wd.set(wd, n->dirs.len);
        n->dirs.push(FsInoDir{wd, root.id, ino_dup_str(rel)});
    }
    Listing l{};
    os_dir_list(abs, false, list_name, &l, nullptr);
    for (u32 i = 0; i < l.at.len; ++i) {
        const char* name = l.text.data + l.at.data[i];
        OsPathKind k = l.kind.data[i];
        if (k == OsPathKind::Dir) {
            char crel[KV_PLATFORM_PATH_MAX * 2];
            snprintf(crel, sizeof crel, rel[0] ? "%s/%s" : "%s%s", rel, name);
            if (sink) sink->emit(FsEventKind::DirAdded, root.id, rel, name);
            add_tree(n, root, crel, sink, depth + 1);
        } else if (k == OsPathKind::File && sink) {
            sink->emit(FsEventKind::Added, root.id, rel, name);
        }
    }
    l.text.free();
    l.at.free();
    l.kind.free();
    return true;
}

// Whether d's entry `name` is a link to a directory, which a watch neither reports nor
// goes down.
bool is_dir_link(FsNative* n, const FsInoDir& d, const char* name) {
    FsInoRoot* r = root_of(n, d.root);
    if (!r) return false;
    char abs[KV_PLATFORM_PATH_MAX * 2];
    int len = snprintf(abs, sizeof abs, d.rel[0] ? "%s/%s/%s" : "%s%s/%s", r->dir, d.rel, name);
    return len > 0 && (usize)len < sizeof abs && os_path_is_dir_link(abs);
}

// Forgets the watches on root's `rel` and everything under it.
void drop_tree(FsNative* n, u32 root, const char* rel) {
    usize rl = strlen(rel);
    for (u32 i = n->dirs.len; i-- > 0;) {
        FsInoDir& d = n->dirs.data[i];
        if (d.root != root) continue;
        bool in = rl == 0 || (strncmp(d.rel, rel, rl) == 0 && (d.rel[rl] == 0 || d.rel[rl] == '/'));
        if (!in) continue;
        inotify_rm_watch(n->fd, d.wd); // fails for one the kernel already dropped: fine
        drop_dir(n, i);
    }
}

} // namespace

const char* fs_native_name() { return "inotify"; }

FsNative* fs_native_create() {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return nullptr;
    FsNative* n = new (mem_alloc(sizeof(FsNative))) FsNative{};
    n->fd = fd;
    return n;
}

void fs_native_destroy(FsNative* n) {
    if (!n) return;
    for (FsInoDir& d : n->dirs) mem_free(d.rel);
    for (FsInoRoot& r : n->roots) mem_free(r.dir);
    n->dirs.free();
    n->by_wd.free();
    n->roots.free();
    close(n->fd);
    mem_free(n);
}

bool fs_native_add(FsNative* n, u32 root, const char* dir) {
    usize len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/') --len;
    char* copy = (char*)mem_alloc(len + 1);
    memcpy(copy, dir, len);
    copy[len] = 0;
    n->roots.push(FsInoRoot{root, copy});
    if (add_tree(n, n->roots.last(), "", nullptr, 0)) return true;
    fs_native_remove(n, root);
    return false;
}

void fs_native_remove(FsNative* n, u32 root) {
    drop_tree(n, root, "");
    for (u32 i = 0; i < n->roots.len; ++i) {
        if (n->roots.data[i].id != root) continue;
        mem_free(n->roots.data[i].dir);
        n->roots.data[i] = n->roots.data[n->roots.len - 1];
        --n->roots.len;
        return;
    }
}

void fs_native_poll(FsNative* n, FsSink& sink) {
    alignas(inotify_event) char buf[16 * 1024];
    for (;;) {
        ssize_t got = read(n->fd, buf, sizeof buf);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            return; // EAGAIN: nothing more
        }
        for (char* p = buf; p < buf + got;) {
            const inotify_event* ev = (const inotify_event*)p;
            p += sizeof(inotify_event) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                // Events were lost. Watch whatever appeared meanwhile, and tell the caller
                // to look at everything again.
                for (u32 i = 0; i < n->roots.len; ++i) {
                    FsInoRoot r = n->roots.data[i];
                    add_tree(n, r, "", nullptr, 0);
                    sink.emit(FsEventKind::Rescan, r.id, nullptr, nullptr);
                }
                continue;
            }
            u32* at = n->by_wd.find(ev->wd);
            if (!at) continue; // a watch already forgotten
            FsInoDir d = n->dirs.data[*at];
            if (ev->mask & IN_IGNORED) { drop_dir(n, *at); continue; }
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                // A directory inside a root is reported by its parent. The root has none.
                if (d.rel[0] == 0) sink.emit(FsEventKind::DirRemoved, d.root, nullptr, nullptr);
                continue;
            }
            if (!ev->len) continue;
            const char* name = ev->name;
            if (ev->mask & IN_ISDIR) {
                char rel[KV_PLATFORM_PATH_MAX * 2];
                int rn = snprintf(rel, sizeof rel, d.rel[0] ? "%s/%s" : "%s%s", d.rel, name);
                if (rn < 0 || (usize)rn >= sizeof rel) continue;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                    sink.emit(FsEventKind::DirAdded, d.root, d.rel, name);
                    if (FsInoRoot* r = root_of(n, d.root)) { FsInoRoot copy = *r; add_tree(n, copy, rel, &sink, 0); }
                } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    drop_tree(n, d.root, rel);
                    sink.emit(FsEventKind::DirRemoved, d.root, d.rel, name);
                }
                continue;
            }
            if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && is_dir_link(n, d, name)) continue; // not reported (KR16)
            if (ev->mask & (IN_CREATE | IN_MOVED_TO))        sink.emit(FsEventKind::Added, d.root, d.rel, name);
            else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) sink.emit(FsEventKind::Changed, d.root, d.rel, name);
            else if (ev->mask & (IN_DELETE | IN_MOVED_FROM))  sink.emit(FsEventKind::Removed, d.root, d.rel, name);
        }
    }
}

} // namespace kv
