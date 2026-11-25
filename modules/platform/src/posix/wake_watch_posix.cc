// Korvid platform: the sockets platform_wake_set is given, watched from one thread blocked
// in poll (KR26). A pipe wakes the thread when the set changes or it is to stop. A socket
// that can be read wakes the loop once and is left out of the poll until the loop's next
// wait arms it again (wake_watch_rearm), by which time the pass it woke has read it.
#include "../wake_watch.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <unistd.h>

namespace kv {

namespace {

constexpr u32 kMaxWatched = 64;

} // namespace

struct WakeWatch {
    PlatformState* s = nullptr;
    OsMutex  lock;
    // `once`: a child's exit, which happens once and stays readable after: heard once, never rearmed.
    struct Entry { const void* owner; Handle fd; bool armed; bool once; };
    Entry    list[kMaxWatched];
    u32      count = 0;
    bool     fired = false; // something was left out of the poll since the last rearm
    bool     stop  = false;
    int      ctl[2] = {-1, -1}; // the thread's own pipe: a byte in it means look again
    OsThread thread;
    bool     running = false;
};

namespace {

void poke(WakeWatch& w) {
    char b = 1;
    ssize_t r = write(w.ctl[1], &b, 1); // full: the thread has one waiting already
    (void)r;
}

void watch_main(void* arg) {
    WakeWatch& w = *(WakeWatch*)arg;
    struct pollfd fds[kMaxWatched + 1];
    for (;;) {
        u32 n = 0;
        fds[n++] = {w.ctl[0], POLLIN, 0};
        os_mutex_lock(&w.lock);
        if (w.stop) { os_mutex_unlock(&w.lock); return; }
        for (u32 i = 0; i < w.count; ++i)
            if (w.list[i].armed) {
                // What the handle names now: one closed since it was stated is stale, and left out,
                // where its number would have been polled for whatever has it next.
                i64 v = os_handle_value(w.list[i].fd);
                if (v >= 0) fds[n++] = {(int)v, POLLIN, 0};
            }
        os_mutex_unlock(&w.lock);
        int r = poll(fds, (nfds_t)n, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            KV_ERROR("platform", "the socket watch stops: poll failed (%d)", errno);
            return;
        }
        if (fds[0].revents) {
            char buf[64];
            while (read(w.ctl[0], buf, sizeof buf) > 0) {}
        }
        bool woke = false;
        os_mutex_lock(&w.lock);
        for (u32 k = 1; k < n; ++k) {
            if (!fds[k].revents) continue;
            // Readable, closed or gone: the loop's pass finds out which. Left out till rearmed.
            for (u32 i = 0; i < w.count; ++i)
                if ((int)os_handle_value(w.list[i].fd) == fds[k].fd && w.list[i].armed) { w.list[i].armed = false; w.fired = true; woke = true; }
        }
        os_mutex_unlock(&w.lock);
        if (woke) platform_wake_state(*w.s);
    }
}

} // namespace

bool wake_watch_supported() { return true; }
// poll takes whatever is a descriptor that becomes readable, a child's exit (a pidfd, a
// kqueue) among them; a file on a disk always is, so never.
bool wake_watch_takes(OsKind k) { return k == OsKind::Socket || k == OsKind::Pipe || k == OsKind::Notify || k == OsKind::Process; }

bool wake_watch_readable(Handle h) {
    if (!wake_watch_takes(os_handle_kind(h))) return false;
    i64 v = os_handle_value(h);
    if (v < 0) return false;
    struct pollfd p = {(int)v, POLLIN, 0};
    return poll(&p, 1, 0) > 0 && p.revents != 0; // data, a connection, or the end
}

WakeWatch* wake_watch_create(PlatformState& s) {
    WakeWatch* w = new (mem_alloc(sizeof(WakeWatch))) WakeWatch{};
    w->s = &s;
    os_mutex_init(&w->lock);
    if (pipe(w->ctl) != 0) {
        KV_ERROR("platform", "the socket watch has no pipe (%d)", errno);
        os_mutex_destroy(&w->lock);
        mem_free(w);
        return nullptr;
    }
    for (int fd : w->ctl) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return w;
}

void wake_watch_destroy(WakeWatch* w) {
    if (!w) return;
    if (w->running) {
        os_mutex_lock(&w->lock);
        w->stop = true;
        os_mutex_unlock(&w->lock);
        poke(*w);
        os_thread_join(&w->thread);
    }
    close(w->ctl[0]);
    close(w->ctl[1]);
    os_mutex_destroy(&w->lock);
    w->~WakeWatch();
    mem_free(w);
}

bool wake_watch_set(WakeWatch* w, const void* owner, const Handle* fds, u32 n) {
    bool changed = false, fits = true;
    os_mutex_lock(&w->lock);
    // The owner's sockets no longer given go; those still given keep whether they are armed.
    for (u32 i = 0; i < w->count;) {
        WakeWatch::Entry& e = w->list[i];
        bool kept = false;
        if (e.owner == owner)
            for (u32 k = 0; k < n; ++k) if (fds[k] == e.fd) kept = true;
        if (e.owner != owner || kept) { ++i; continue; }
        e = w->list[--w->count];
        changed = true;
    }
    for (u32 k = 0; k < n; ++k) {
        if (!os_handle_open(fds[k])) continue;
        bool have = false;
        for (u32 i = 0; i < w->count; ++i) if (w->list[i].owner == owner && w->list[i].fd == fds[k]) have = true;
        if (have) continue;
        if (w->count == kMaxWatched) { fits = false; continue; }
        w->list[w->count++] = WakeWatch::Entry{owner, fds[k], true, os_handle_kind(fds[k]) == OsKind::Process};
        changed = true;
    }
    bool start = !w->running && w->count;
    os_mutex_unlock(&w->lock);
    if (start) {
        if (os_thread_start(&w->thread, watch_main, w)) w->running = true;
        else { KV_ERROR("platform", "the socket watch's thread did not start"); return false; }
    } else if (changed && w->running) {
        poke(*w);
    }
    return fits;
}

void wake_watch_rearm(WakeWatch* w) {
    os_mutex_lock(&w->lock);
    bool again = w->fired;
    if (again) {
        for (u32 i = 0; i < w->count; ++i) if (!w->list[i].once) w->list[i].armed = true;
        w->fired = false;
    }
    os_mutex_unlock(&w->lock);
    if (again) poke(*w);
}

} // namespace kv
