// Korvid platform: the sockets platform_wake_set is given, watched from one thread blocked
// in WSAPoll (KR26), as posix/wake_watch_posix.cc watches them in poll. WSAPoll takes
// sockets only, so the thread's own wake is a socket too, where POSIX has a pipe: a UDP
// socket bound to the loopback and connected to itself, a byte sent to it meaning look
// again. A socket that can be read wakes the loop once and is left out of the poll until the
// loop's next wait arms it again (wake_watch_rearm), by which time the pass it woke has read it.
#include "../wake_watch.h"
#include "wide.h" // windows.h as the module includes it (NOMINMAX, lean), before winsock
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

struct WakeWatch {
    PlatformState* s = nullptr;
    OsMutex  lock;
    struct Entry { const void* owner; KHandle fd; bool armed; };
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
            if (w.list[i].armed) fds[n++] = {(SOCKET)w.list[i].fd.os(), POLLRDNORM, 0};
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
        os_mutex_lock(&w.lock);
        for (u32 k = 1; k < n; ++k) {
            if (!fds[k].revents) continue;
            // Readable, closed or gone: the loop's pass finds out which. Left out till rearmed.
            for (u32 i = 0; i < w.count; ++i)
                if ((SOCKET)w.list[i].fd.os() == fds[k].fd && w.list[i].armed) { w.list[i].armed = false; w.fired = true; woke = true; }
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

} // namespace

bool wake_watch_supported() { return true; }
// WSAPoll takes sockets and nothing else: a pipe's or a console's handle is not one.
bool wake_watch_takes(KHandleKind k) { return k == KHandleKind::Socket; }

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
    closesocket(w->ctl);
    os_mutex_destroy(&w->lock);
    w->~WakeWatch();
    mem_free(w);
    WSACleanup();
}

bool wake_watch_set(WakeWatch* w, const void* owner, const KHandle* fds, u32 n) {
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
        if (!fds[k].is_open()) continue;
        bool have = false;
        for (u32 i = 0; i < w->count; ++i) if (w->list[i].owner == owner && w->list[i].fd == fds[k]) have = true;
        if (have) continue;
        if (w->count == kMaxWatched) { fits = false; continue; }
        w->list[w->count++] = WakeWatch::Entry{owner, fds[k], true};
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
        for (u32 i = 0; i < w->count; ++i) w->list[i].armed = true;
        w->fired = false;
    }
    os_mutex_unlock(&w->lock);
    if (again) poke(*w);
}

} // namespace kv
