// Korvid platform: the sockets platform_wake_set is given, watched from one thread blocked
// in WSAPoll (KR26), as posix/wake_watch_posix.cc watches them in poll. WSAPoll takes
// sockets only, so the thread's own wake is a socket too, where POSIX has a pipe: a UDP
// socket bound to the loopback and connected to itself, a byte sent to it meaning look
// again. A socket that can be read wakes the loop once and is left out of the poll until the
// loop's next wait arms it again (wake_watch_rearm), by which time the pass it woke has read it.
#include "../wake_watch.h"
#include "../platform_state.h"
#include "wide.h" // windows.h as the module includes it (NOMINMAX, lean), before winsock
#include "korvid/core/atomic.h"
#include "korvid/core/log.h"
#include "korvid/core/memory.h"
#include <new>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace kv {

namespace {

constexpr u32 kMaxWatched = 64;

} // namespace

struct WakeWatch;

// A child's exit, which WSAPoll cannot wait for: a one-shot wait in the thread pool on a
// duplicate of the process handle (the Process may close its own while this one waits).
struct ExitWait {
    WakeWatch* w = nullptr;
    HANDLE     dup = nullptr;
    HANDLE     wait = nullptr;
};

struct WakeWatch {
    PlatformState* s = nullptr;
    OsMutex  lock;
    // `once`: a child's exit, which happens once: heard once, never rearmed.
    // `fired_at`: the platform's wait_returns when it last fired (wake_watch_rearm).
    struct Entry { const void* owner; Handle fd; bool armed; bool once; ExitWait* exit; u64 fired_at; };
    Entry    list[kMaxWatched];
    u32      count = 0;
    bool     fired = false; // something was left out of the poll since the last rearm
    bool     stop  = false;
    SOCKET   ctl = INVALID_SOCKET; // the thread's own: a datagram in it means look again
    OsThread thread;
    bool     running = false;
};

namespace {

void poke(WakeWatch& w) {
    char b = 1;
    send(w.ctl, &b, 1, 0); // the buffer full: the thread has one waiting already
}

void watch_main(void* arg) {
    WakeWatch& w = *(WakeWatch*)arg;
    WSAPOLLFD fds[kMaxWatched + 1];
    for (;;) {
        u32 n = 0;
        fds[n++] = {w.ctl, POLLRDNORM, 0};
        os_mutex_lock(&w.lock);
        if (w.stop) { os_mutex_unlock(&w.lock); return; }
        for (u32 i = 0; i < w.count; ++i)
            if (w.list[i].armed) {
                // What the handle names now: one closed since it was stated is stale, and left out,
                // where its number would have been polled for whatever has it next.
                i64 v = os_handle_value(w.list[i].fd, OsKind::Socket);
                if (v >= 0) fds[n++] = {(SOCKET)v, POLLRDNORM, 0};
            }
        os_mutex_unlock(&w.lock);
        int r = WSAPoll(fds, (ULONG)n, -1);
        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            KV_ERROR("platform", "the socket watch stops: WSAPoll failed (%d)", err);
            return;
        }
        if (fds[0].revents) {
            char buf[64];
            while (recv(w.ctl, buf, sizeof buf, 0) > 0) {}
        }
        bool woke = false;
        u64 at = atomic_load(&w.s->wait_returns, MO_ACQUIRE);
        os_mutex_lock(&w.lock);
        for (u32 k = 1; k < n; ++k) {
            if (!fds[k].revents) continue;
            // Readable, closed or gone: the loop's pass finds out which. Left out till rearmed.
            for (u32 i = 0; i < w.count; ++i)
                if ((SOCKET)os_handle_value(w.list[i].fd) == fds[k].fd && w.list[i].armed) {
                    w.list[i].armed = false;
                    w.list[i].fired_at = at;
                    w.fired = true;
                    woke = true;
                }
        }
        os_mutex_unlock(&w.lock);
        if (woke) platform_wake_state(*w.s);
    }
}

// The thread's own socket: UDP on 127.0.0.1, connected to its own port, so a send reaches
// it and nothing else can. Non-blocking, so the thread drains it and a poke never waits.
SOCKET self_socket() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = (int)sizeof a;
    u_long on = 1;
    if (bind(s, (sockaddr*)&a, len) != 0 || getsockname(s, (sockaddr*)&a, &len) != 0 ||
        connect(s, (sockaddr*)&a, len) != 0 || ioctlsocket(s, FIONBIO, &on) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// The thread pool's call when a watched child exits: its entry heard, the loop woken.
void CALLBACK exit_heard(PVOID arg, BOOLEAN) {
    ExitWait* x = (ExitWait*)arg;
    WakeWatch& w = *x->w;
    bool woke = false;
    u64 at = atomic_load(&w.s->wait_returns, MO_ACQUIRE);
    os_mutex_lock(&w.lock);
    for (u32 i = 0; i < w.count; ++i)
        if (w.list[i].exit == x && w.list[i].armed) { w.list[i].armed = false; w.list[i].fired_at = at; w.fired = true; woke = true; }
    os_mutex_unlock(&w.lock);
    if (woke) platform_wake_state(*w.s);
}

// A wait on the exit of the child `fd` names, or null if it cannot be had (logged).
ExitWait* exit_wait_start(WakeWatch& w, Handle fd) {
    i64 v = os_handle_value(fd, OsKind::Process);
    if (v < 0) return nullptr;
    ExitWait* x = new (mem_alloc(sizeof(ExitWait))) ExitWait{};
    x->w = &w;
    HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, (HANDLE)(intptr_t)v, self, &x->dup, SYNCHRONIZE, FALSE, 0) ||
        !RegisterWaitForSingleObject(&x->wait, x->dup, exit_heard, x, INFINITE, WT_EXECUTEONLYONCE)) {
        KV_ERROR("platform", "a child's exit cannot be watched (%lu); it is asked about again soon", (unsigned long)GetLastError());
        if (x->dup) CloseHandle(x->dup);
        mem_free(x);
        return nullptr;
    }
    return x;
}

// Ends the wait, its call finished if it was under way, and frees it. Not under the
// watch's lock: the call takes it.
void exit_wait_end(ExitWait* x) {
    if (!x) return;
    UnregisterWaitEx(x->wait, INVALID_HANDLE_VALUE);
    CloseHandle(x->dup);
    mem_free(x);
}

} // namespace

bool wake_watch_supported() { return true; }
// WSAPoll takes sockets and nothing else: a pipe's or a console's handle is not one. A
// child's exit is a wait of the thread pool's (exit_wait_start).
bool wake_watch_takes(OsKind k) { return k == OsKind::Socket || k == OsKind::Process; }

bool wake_watch_readable(Handle h) {
    OsKind kind = os_handle_kind(h);
    i64 v = os_handle_value(h);
    if (v < 0) return false;
    if (kind == OsKind::Socket) {
        fd_set can_read;
        FD_ZERO(&can_read);
        FD_SET((SOCKET)v, &can_read);
        timeval none{0, 0};
        return select(0, &can_read, nullptr, nullptr, &none) > 0;
    }
    if (kind == OsKind::Pipe) {
        DWORD avail = 0;
        return !PeekNamedPipe((HANDLE)(intptr_t)v, nullptr, 0, nullptr, &avail, nullptr) || avail > 0; // data, or the end
    }
    if (kind == OsKind::Process) return WaitForSingleObject((HANDLE)(intptr_t)v, 0) == WAIT_OBJECT_0; // it has exited
    return false;
}

WakeWatch* wake_watch_create(PlatformState& s) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { // counted: the destroy's WSACleanup gives it back
        KV_ERROR("platform", "the socket watch has no Winsock");
        return nullptr;
    }
    WakeWatch* w = new (mem_alloc(sizeof(WakeWatch))) WakeWatch{};
    w->s = &s;
    w->ctl = self_socket();
    if (w->ctl == INVALID_SOCKET) {
        KV_ERROR("platform", "the socket watch has no socket of its own (%d)", WSAGetLastError());
        w->~WakeWatch();
        mem_free(w);
        WSACleanup();
        return nullptr;
    }
    os_mutex_init(&w->lock);
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
    for (u32 i = 0; i < w->count; ++i) exit_wait_end(w->list[i].exit); // the pool's calls finished first
    closesocket(w->ctl);
    os_mutex_destroy(&w->lock);
    w->~WakeWatch();
    mem_free(w);
    WSACleanup();
}

bool wake_watch_set(WakeWatch* w, const void* owner, const Handle* fds, u32 n) {
    bool changed = false, fits = true;
    ExitWait* ended[kMaxWatched];
    u32 ended_count = 0;
    os_mutex_lock(&w->lock);
    // The owner's handles no longer given go; those still given keep whether they are armed.
    for (u32 i = 0; i < w->count;) {
        WakeWatch::Entry& e = w->list[i];
        bool kept = false;
        if (e.owner == owner)
            for (u32 k = 0; k < n; ++k) if (fds[k] == e.fd) kept = true;
        if (e.owner != owner || kept) { ++i; continue; }
        if (e.exit) ended[ended_count++] = e.exit;
        e = w->list[--w->count];
        changed = true;
    }
    for (u32 k = 0; k < n; ++k) {
        if (!os_handle_open(fds[k])) continue;
        bool have = false;
        for (u32 i = 0; i < w->count; ++i) if (w->list[i].owner == owner && w->list[i].fd == fds[k]) have = true;
        if (have) continue;
        if (w->count == kMaxWatched) { fits = false; continue; }
        bool exit = os_handle_kind(fds[k]) == OsKind::Process;
        WakeWatch::Entry& e = w->list[w->count++];
        e = WakeWatch::Entry{owner, fds[k], true, exit, nullptr, 0};
        if (exit && !(e.exit = exit_wait_start(*w, fds[k]))) fits = false;
        changed = true;
    }
    bool start = !w->running && w->count;
    os_mutex_unlock(&w->lock);
    for (u32 i = 0; i < ended_count; ++i) exit_wait_end(ended[i]);
    if (start) {
        if (os_thread_start(&w->thread, watch_main, w)) w->running = true;
        else { KV_ERROR("platform", "the socket watch's thread did not start"); return false; }
    } else if (changed && w->running) {
        poke(*w);
    }
    return fits;
}

void wake_watch_rearm(WakeWatch* w, u64 returned) {
    os_mutex_lock(&w->lock);
    bool again = false;
    if (w->fired) {
        w->fired = false;
        for (u32 i = 0; i < w->count; ++i) {
            WakeWatch::Entry& e = w->list[i];
            if (e.armed || e.once) continue;
            if (e.fired_at < returned) { e.armed = true; again = true; }
            else w->fired = true; // fired since the last wait returned: unread yet, the next wait's
        }
    }
    os_mutex_unlock(&w->lock);
    if (again) poke(*w);
}

} // namespace kv
