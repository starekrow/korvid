#include "korvid/platform/fs_watch.h"
#include "korvid/platform/io.h"
#include "korvid/core/atomic.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h> // getuid, symlink
#endif

namespace {

using namespace kv;

// Everything a watcher has said, as "kind path" lines, so a test can ask whether a thing
// was said and in what order.
struct Heard {
    Array<char> text;
    Array<u32>  at;

    void take(Span<const FsEvent> evs) {
        for (const FsEvent& e : evs) {
            char line[1100];
            int n = snprintf(line, sizeof line, "%s %s", fs_event_kind_name(e.kind), e.path);
            at.push(text.len);
            memcpy(text.push_n((u32)n + 1), line, (usize)n + 1);
        }
    }
    i32 find(const char* line) const {
        for (u32 i = 0; i < at.len; ++i) if (strcmp(text.data + at.data[i], line) == 0) return (i32)i;
        return -1;
    }
    bool has(const char* line) const { return find(line) >= 0; }
    bool mentions(const char* part) const {
        for (u32 i = 0; i < at.len; ++i) if (strstr(text.data + at.data[i], part)) return true;
        return false;
    }
    void clear() { text.len = 0; at.len = 0; }
    void free() { text.free(); at.free(); }
};

bool wait_for(FsWatcher& w, Heard& h, const char* line) {
    for (u32 i = 0; i < 2000; ++i) {
        h.take(w.poll(true));
        if (h.has(line)) return true;
        time_sleep(0.001);
    }
    return false;
}

void drain(FsWatcher& w, Heard& h) {
    for (u32 i = 0; i < 20; ++i) { h.take(w.poll(true)); time_sleep(0.001); }
}

bool write_text(const char* dir, const char* rel, const char* text) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    return io_write_file(path, text, strlen(text));
}

void run(TestCtx& c, FsWatchBackend backend, const char* expect_name) {
    char dir[512], path[1024], path2[1024];
    KV_CHECK(c, io_make_temp_dir("kv-fswatch", dir, sizeof dir));
    snprintf(path, sizeof path, "%s/ships", dir);
    KV_CHECK(c, io_make_dirs(path) && write_text(dir, "a.png", "a") && write_text(dir, "ships/big.png", "big"));

    FsWatcher w;
    KV_CHECK(c, w.init(backend, 0.01) && w.is_open() && strcmp(w.backend_name(), expect_name) == 0);
    u32 root = w.add_root(dir);
    KV_CHECK(c, root != ~0u && w.add_root("/no/such/directory/anywhere") == ~0u);

    // What was there when the root was added is not news.
    Heard h{};
    drain(w, h);
    KV_CHECK(c, h.at.len == 0);

    // A new file, at the top and below.
    KV_CHECK(c, write_text(dir, "b.png", "b") && wait_for(w, h, "added b.png"));
    KV_CHECK(c, write_text(dir, "ships/small.png", "s") && wait_for(w, h, "added ships/small.png"));

    // A write to an existing file. (A different size, so the polling backend cannot miss
    // it inside one tick of the file system's clock.)
    h.clear();
    KV_CHECK(c, write_text(dir, "a.png", "a longer a") && wait_for(w, h, "changed a.png"));
    KV_CHECK(c, !h.has("added a.png") && !h.has("removed a.png"));

    // A removal.
    snprintf(path, sizeof path, "%s/b.png", dir);
    KV_CHECK(c, io_remove(path) && wait_for(w, h, "removed b.png"));

    // A rename is a removal and an arrival; a rename onto a file that exists is an arrival
    // at least (the editor's write-a-temp-then-rename). io_rename, not the C library's:
    // Windows' rename() will not replace a file that is there.
    snprintf(path, sizeof path, "%s/a.png", dir);
    snprintf(path2, sizeof path2, "%s/c.png", dir);
    KV_CHECK(c, io_rename(path, path2) && wait_for(w, h, "removed a.png") && wait_for(w, h, "added c.png"));
    h.clear();
    KV_CHECK(c, write_text(dir, "c.png.tmp", "the new c, longer"));
    snprintf(path, sizeof path, "%s/c.png.tmp", dir);
    KV_CHECK(c, io_rename(path, path2));
    drain(w, h);
    KV_CHECK(c, h.has("added c.png") || h.has("changed c.png"));

    // A new directory is seen, watched, and what is already in it reported.
    h.clear();
    snprintf(path, sizeof path, "%s/bosses/one", dir);
    KV_CHECK(c, io_make_dirs(path) && write_text(dir, "bosses/one/eye.png", "e"));
    KV_CHECK(c, wait_for(w, h, "added bosses/one/eye.png") && h.has("dir-added bosses") && h.has("dir-added bosses/one"));
    KV_CHECK(c, h.find("dir-added bosses") < h.find("dir-added bosses/one") && h.find("dir-added bosses/one") < h.find("added bosses/one/eye.png"));
    // And a file made in it afterwards.
    KV_CHECK(c, write_text(dir, "bosses/one/jaw.png", "j") && wait_for(w, h, "added bosses/one/jaw.png"));

    // A directory moved away is one event; a file made where it was is not confused by it.
    h.clear();
    char elsewhere[512];
    KV_CHECK(c, io_make_temp_dir("kv-fswatch-out", elsewhere, sizeof elsewhere));
    snprintf(path, sizeof path, "%s/bosses", dir);
    snprintf(path2, sizeof path2, "%s/bosses", elsewhere);
    KV_CHECK(c, rename(path, path2) == 0 && wait_for(w, h, "dir-removed bosses"));
    drain(w, h);
    KV_CHECK(c, !h.has("removed bosses/one/eye.png") && !h.has("dir-removed bosses/one"));
    // What happens in it now is none of this root's business.
    h.clear();
    KV_CHECK(c, write_text(elsewhere, "bosses/one/eye.png", "changed elsewhere"));
    drain(w, h);
    KV_CHECK(c, h.at.len == 0);
    // Moved back: it is new again, with its contents.
    KV_CHECK(c, rename(path2, path) == 0 && wait_for(w, h, "added bosses/one/jaw.png") && h.has("dir-added bosses") && h.has("added bosses/one/eye.png"));

    // A tree removed.
    h.clear();
    snprintf(path, sizeof path, "%s/ships", dir);
    KV_CHECK(c, io_remove_tree(path) && wait_for(w, h, "dir-removed ships"));

    // Many changes at once: none lost, repeats folded within a poll.
    h.clear();
    for (u32 i = 0; i < 200; ++i) {
        char name[32];
        snprintf(name, sizeof name, "many-%03u.txt", i);
        KV_CHECK(c, write_text(dir, name, "x"));
    }
    KV_CHECK(c, wait_for(w, h, "added many-199.txt") && h.has("added many-000.txt") && h.has("added many-100.txt"));
    u32 dup = 0;
    for (u32 i = 0; i < h.at.len; ++i) if (strcmp(h.text.data + h.at.data[i], "added many-050.txt") == 0) ++dup;
    KV_CHECK(c, dup == 1);

    // A second root has its own id, and removing a root silences it.
    u32 second = w.add_root(elsewhere);
    KV_CHECK(c, second != ~0u && second != root);
    w.remove_root(root);
    h.clear();
    KV_CHECK(c, write_text(dir, "after.txt", "x") && write_text(elsewhere, "here.txt", "x") && wait_for(w, h, "added here.txt"));
    drain(w, h);
    KV_CHECK(c, !h.has("added after.txt"));
    bool right_root = true;
    w.poll(true);
    KV_CHECK(c, write_text(elsewhere, "again.txt", "x"));
    for (u32 i = 0; i < 2000; ++i) {
        Span<const FsEvent> evs = w.poll(true);
        if (evs.len) { for (const FsEvent& e : evs) if (e.root != second) right_root = false; break; }
        time_sleep(0.001);
    }
    KV_CHECK(c, right_root);

    h.free();
    w.free();
    KV_CHECK(c, !w.is_open() && w.poll().len == 0);
    io_remove_tree(dir);
    io_remove_tree(elsewhere);
}

// KR12: what a share is, and a root polled under a native watcher.
void run_shares(TestCtx& c) {
    char dir[512], other[512];
    KV_CHECK(c, io_make_temp_dir("kv-fswatch-share", dir, sizeof dir) && io_make_temp_dir("kv-fswatch-local", other, sizeof other));
    KV_CHECK(c, !io_path_is_remote(dir) && !io_path_is_remote("") && !io_path_is_remote("/no/such/place/at/all"));
#if defined(__linux__)
    // The desktop's document portal, where there is one, is a FUSE mount: remote, and
    // polled by an Auto watcher without being told.
    char portal[64];
    snprintf(portal, sizeof portal, "/run/user/%u/doc", (unsigned)getuid());
    if (io_is_dir(portal)) {
        KV_CHECK(c, io_path_is_remote(portal));
        FsWatcher w;
        KV_CHECK(c, w.init(FsWatchBackend::Auto, 0.25, 60.0));
        u32 id = w.add_root(portal);
        if (id != ~0u) KV_CHECK(c, w.root_backend(id) == FsWatchBackend::Poll);
        FsWatcher p; // a watcher made with Native insists: no polling by itself
        if (fs_watch_has_native() && p.init(FsWatchBackend::Native)) {
            u32 pid = p.add_root(portal);
            if (pid != ~0u) KV_CHECK(c, p.root_backend(pid) == FsWatchBackend::Native);
        }
        p.free();
        w.free();
    }
#endif
    // A root polled by name beside one watched natively: both report, each by its own means.
    FsWatcher w;
    KV_CHECK(c, w.init(FsWatchBackend::Auto, 0.01));
    u32 polled = w.add_root(dir, FsWatchBackend::Poll);
    u32 native = w.add_root(other);
    KV_CHECK(c, polled != ~0u && native != ~0u && polled != native);
    KV_CHECK(c, w.root_backend(polled) == FsWatchBackend::Poll);
    KV_CHECK(c, w.root_backend(native) == (fs_watch_has_native() ? FsWatchBackend::Native : FsWatchBackend::Poll));
    KV_CHECK(c, w.root_backend(9999) == FsWatchBackend::Auto);
    u32 insisted = w.add_root(dir, FsWatchBackend::Native); // fails where there is no native backend
    KV_CHECK(c, (insisted == ~0u) == !fs_watch_has_native());
    if (insisted != ~0u) w.remove_root(insisted);
    Heard h;
    KV_CHECK(c, write_text(dir, "song.flac", "s") && write_text(other, "cover.jpg", "c"));
    KV_CHECK(c, wait_for(w, h, "added song.flac") && wait_for(w, h, "added cover.jpg"));
    w.remove_root(polled);
    w.remove_root(native);
    KV_CHECK(c, w.root_backend(polled) == FsWatchBackend::Auto && w.root_backend(native) == FsWatchBackend::Auto);
    h.clear();
    KV_CHECK(c, write_text(dir, "late.flac", "l"));
    drain(w, h);
    KV_CHECK(c, !h.has("added late.flac"));
    h.free();
    w.free();
    io_remove_tree(dir);
    io_remove_tree(other);
}

#if !defined(_WIN32)
// Links inside a root (KR16): one to the root itself and one to a directory outside, there
// before the watch, and another to the root made while it runs. A file written in the root
// is reported once, by its own path; one written outside is not reported; no link to a
// directory is reported or gone down.
void run_links(TestCtx& c, FsWatchBackend backend) {
    char dir[512], outside[512], path[1024], target[1024];
    KV_CHECK(c, io_make_temp_dir("kv-fswatch-links", dir, sizeof dir) && io_make_temp_dir("kv-fswatch-out", outside, sizeof outside));
    snprintf(path, sizeof path, "%s/sub", dir);
    KV_CHECK(c, io_make_dirs(path));
    snprintf(path, sizeof path, "%s/loop", dir);
    KV_CHECK(c, symlink(dir, path) == 0);
    snprintf(path, sizeof path, "%s/sub/out", dir);
    KV_CHECK(c, symlink(outside, path) == 0);
    KV_CHECK(c, write_text(outside, "kept.txt", "k"));
    snprintf(target, sizeof target, "%s/kept.txt", outside);
    snprintf(path, sizeof path, "%s/file-link", dir);
    KV_CHECK(c, symlink(target, path) == 0);

    FsWatcher w;
    KV_CHECK(c, w.init(backend, 0.01));
    KV_CHECK(c, w.add_root(dir) != ~0u);
    Heard h;
    drain(w, h);
    snprintf(path, sizeof path, "%s/sub/late-loop", dir);
    KV_CHECK(c, symlink(dir, path) == 0);
    KV_CHECK(c, write_text(dir, "growing.flac", "g") && write_text(outside, "far.flac", "f"));
    KV_CHECK(c, wait_for(w, h, "added growing.flac"));
    KV_CHECK(c, write_text(dir, "sub/marker.txt", "m") && wait_for(w, h, "added sub/marker.txt"));
    drain(w, h);
    KV_CHECK(c, !h.mentions("loop") && !h.mentions("out") && !h.mentions("far.flac"));
    KV_CHECK(c, h.find("added growing.flac") >= 0 && !h.mentions("/growing.flac"));
    h.free();
    w.free();
    io_remove_tree(dir);
    io_remove_tree(outside);
}
#endif

// set_wake (KR26): where the native backend has no descriptor, a write calls the wake once,
// a second before the poll does not call it again, the poll's taking of it lets the next
// call, and none comes once it is cleared; due() asks for no poll while a wake is set.
// Where there is a descriptor (inotify), set_wake is false and wake_handle says.
void wake_bump(void* user) { atomic_fetch_add((u32*)user, 1u, MO_ACQ_REL); }

bool wake_until(u32* calls, u32 want) {
    for (u32 i = 0; i < 2000; ++i) { if (atomic_load(calls, MO_ACQUIRE) >= want) return true; time_sleep(0.001); }
    return false;
}

void run_wake(TestCtx& c) {
    char dir[256];
    KV_CHECK(c, io_make_temp_dir("kv-fswatch-wake", dir, sizeof dir));
    FsWatcher w;
    KV_CHECK(c, w.init(FsWatchBackend::Native) && w.add_root(dir) != ~0u);
    {
        char gone[300];
        format(gone, sizeof gone, "%s/not-here", dir);
        i32 err = 0;
        KV_CHECK(c, w.add_root(gone, FsWatchBackend::Auto, &err) == ~0u && err == ENOENT);
    }
    u32 calls = 0;
    if (w.wake_handle().is_open()) {
        KV_CHECK(c, !w.set_wake(wake_bump, &calls));
    } else {
        KV_CHECK(c, w.due() > 0.0);                 // no wake: a poll soon
        KV_CHECK(c, w.set_wake(wake_bump, &calls));
        KV_CHECK(c, w.due() == 0.0);                // a wake, no polled root: nothing to ask for
        KV_CHECK(c, atomic_load(&calls, MO_ACQUIRE) == 0);
        KV_CHECK(c, write_text(dir, "a.txt", "a"));
        KV_CHECK(c, wake_until(&calls, 1));
        KV_CHECK(c, write_text(dir, "b.txt", "b")); // not polled yet: no second call
        time_sleep(w.lateness() + 0.1);
        KV_CHECK(c, atomic_load(&calls, MO_ACQUIRE) == 1);
        Heard h;
        drain(w, h);
        KV_CHECK(c, h.has("added a.txt") && h.has("added b.txt"));
        KV_CHECK(c, write_text(dir, "c.txt", "c")); // polled: the next news calls again
        KV_CHECK(c, wake_until(&calls, 2));
        drain(w, h);
        KV_CHECK(c, w.set_wake(nullptr, nullptr) == false && w.due() > 0.0);
        u32 had = atomic_load(&calls, MO_ACQUIRE);
        KV_CHECK(c, write_text(dir, "d.txt", "d"));
        time_sleep(w.lateness() + 0.1);
        KV_CHECK(c, atomic_load(&calls, MO_ACQUIRE) == had); // cleared: never called
        h.free();
    }
    w.free();
    io_remove_tree(dir);
}

} // namespace

void test_fs_watch(kv::TestCtx& c) {
    using namespace kv;
    c.begin("fs_watch poll");
    run(c, FsWatchBackend::Poll, "poll");
    FsWatcher w;
    if (fs_watch_has_native()) {
        c.begin("fs_watch native");
        KV_CHECK(c, w.init(FsWatchBackend::Auto) && strcmp(w.backend_name(), "poll") != 0);
        char name[32];
        format(name, sizeof name, "%s", w.backend_name());
        w.free();
        run(c, FsWatchBackend::Native, name);
        c.begin("fs_watch wake");
        run_wake(c);
    } else {
        KV_CHECK(c, !w.init(FsWatchBackend::Native) && w.init(FsWatchBackend::Auto) && strcmp(w.backend_name(), "poll") == 0);
        w.free();
    }
    c.begin("fs_watch shares");
    run_shares(c);
#if !defined(_WIN32)
    c.begin("fs_watch links poll");
    run_links(c, FsWatchBackend::Poll);
    if (fs_watch_has_native()) {
        c.begin("fs_watch links native");
        run_links(c, FsWatchBackend::Native);
    }
#endif
}
