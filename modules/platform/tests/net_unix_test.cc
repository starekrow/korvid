// KR49: Unix-domain sockets, descriptors across them, and a wait another thread ends.
// The listening end is made here with the OS's own calls: net.h connects, it does not
// listen on a name (nothing has asked to).
#include "korvid/platform/net.h"
#include "korvid/core/atomic.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <errno.h>
#include <string.h>
#if !defined(_WIN32)
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

struct Waking {
    kv::NetWaker* w = nullptr;
    kv::f64       after = 0.0;
};

void wake_later(void* arg) {
    Waking& k = *(Waking*)arg;
    kv::time_sleep(k.after);
    k.w->wake();
}

#if !defined(_WIN32)
// A listener on a name, and its one accepted connection, with the OS's calls.
int listen_on(const char* name, bool abstract) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    size_t n = strlen(name);
    socklen_t len;
    if (abstract) { memcpy(a.sun_path + 1, name, n); len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + n); }
    else { memcpy(a.sun_path, name, n); len = (socklen_t)(offsetof(sockaddr_un, sun_path) + n + 1); }
    if (bind(s, (sockaddr*)&a, len) != 0 || listen(s, 4) != 0) { close(s); return -1; }
    return s;
}
#endif

} // namespace

void test_net_unix(kv::TestCtx& c) {
    using namespace kv;
    c.begin("net, Unix-domain sockets");
#if defined(_WIN32)
    NetSocket a, b;
    i32 err = 0;
    KV_CHECK(c, !net_socket_pair(&a, &b, &err) && err == EAFNOSUPPORT);
    KV_CHECK(c, !net_connect_unix_now("x", NetUnixName::Path, &a, 0.0, &err) && err == EAFNOSUPPORT);
    NetWaker w;
    KV_CHECK(c, !w.open(&err) && err == EAFNOSUPPORT);
#else
    // A pair: bytes each way, as any socket.
    NetSocket a, b;
    i32 err = 0;
    KV_CHECK(c, net_socket_pair(&a, &b, &err) && a.is_open() && b.is_open() && err == 0);
    char buf[64];
    KV_CHECK(c, a.write_now("ping", 4) && b.read(buf, sizeof buf, 2.0) == 4 && memcmp(buf, "ping", 4) == 0);

    // A descriptor across: one end of a second pair goes over the first, and the copy that
    // arrives is a socket of this process's that talks to the second pair's other end.
    NetSocket x, y;
    KV_CHECK(c, net_socket_pair(&x, &y));
    KV_CHECK(c, a.write_now_with("F", 1, &x.fd, 1, 2.0));
    Handle got[kNetMaxHandles];
    u32 n = 0;
    KV_CHECK(c, net_wait(b, false, nullptr, 2.0) == kNetReadable);
    KV_CHECK(c, b.read_with(buf, sizeof buf, got, kNetMaxHandles, &n) == 1 && buf[0] == 'F' && n == 1);
    KV_CHECK(c, os_handle_kind(got[0]) == OsKind::Socket && got[0] != x.fd);
    NetSocket copy;
    copy.adopt(got[0]);
    KV_CHECK(c, copy.write_now("via", 3) && y.read(buf, sizeof buf, 2.0) == 3 && memcmp(buf, "via", 3) == 0);
    KV_CHECK(c, x.write_now("own", 3) && y.read(buf, sizeof buf, 2.0) == 3); // the sender kept its own
    copy.close();

    // More than there is room for: those past the room are closed and counted.
    Handle three[3] = {x.fd, y.fd, a.fd};
    KV_CHECK(c, a.write_now_with("G", 1, three, 3, 2.0));
    n = 0;
    KV_CHECK(c, b.read_with(buf, sizeof buf, got, 1, &n) == 1 && n == 1 && b.handles_dropped == 2);
    net_close(got[0]);
    // Refused before anything goes: no bytes to carry them, a handle not open, too many.
    Handle closed_one;
    KV_CHECK(c, !a.write_now_with("", 0, &x.fd, 1) && a.error == EINVAL);
    KV_CHECK(c, !a.write_now_with("H", 1, &closed_one, 1) && a.error == EINVAL);
    a.error = 0;
    KV_CHECK(c, a.is_open());

    // net_wait: the time, exactly; a wake from another thread while waiting for ever; a
    // wake before the wait is one wake; room to write when asked.
    NetWaker w;
    KV_CHECK(c, w.open(&err) && w.is_open());
    f64 t0 = time_now();
    KV_CHECK(c, net_wait(b, false, &w, 0.05) == 0);
    f64 took = time_now() - t0;
    KV_CHECK(c, took >= 0.045 && took < 0.5);
    Waking k{&w, 0.05};
    OsThread t;
    t0 = time_now();
    KV_CHECK(c, os_thread_start(&t, wake_later, &k));
    KV_CHECK(c, net_wait(b, false, &w, -1.0) == kNetWoken);
    took = time_now() - t0;
    os_thread_join(&t);
    KV_CHECK(c, took >= 0.045 && took < 1.0);
    w.wake(); w.wake();
    KV_CHECK(c, net_wait(b, false, &w, 1.0) == kNetWoken);
    KV_CHECK(c, net_wait(b, false, &w, 0.0) == 0); // taken: nothing left over
    KV_CHECK(c, net_wait(b, true, &w, 1.0) == kNetWritable);
    KV_CHECK(c, a.write_now("z", 1) && net_wait(b, false, &w, 1.0) == kNetReadable && b.read(buf, sizeof buf) == 1);

    // The peer gone: readable, and the read says it is over.
    a.close();
    KV_CHECK(c, net_wait(b, false, &w, 2.0) == kNetReadable && b.read(buf, sizeof buf) == -1 && b.closed);
    KV_CHECK(c, net_wait(b, false, &w, -1.0) == kNetReadable); // over already: at once
    b.close(); x.close(); y.close();
    w.close();
    KV_CHECK(c, !w.is_open());

    // By name: a path, and on Linux the abstract namespace; ENOENT where nothing is, and a
    // name too long for the OS.
    char path[96];
    snprintf(path, sizeof path, "/tmp/kv-net-unix-%d", (int)getpid());
    unlink(path);
    int l = listen_on(path, false);
    KV_CHECK(c, l >= 0);
    NetSocket s;
    KV_CHECK(c, net_connect_unix_now(path, NetUnixName::Path, &s, 2.0, &err) && s.is_open());
    int peer = accept(l, nullptr, nullptr);
    KV_CHECK(c, peer >= 0 && write(peer, "hi", 2) == 2);
    KV_CHECK(c, s.read(buf, sizeof buf, 2.0) == 2 && memcmp(buf, "hi", 2) == 0);
    s.close();
    close(peer);
    close(l);
    unlink(path);
    KV_CHECK(c, !net_connect_unix_now(path, NetUnixName::Path, &s, 1.0, &err) && err == ENOENT && !s.is_open());
    char longname[200];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    KV_CHECK(c, !net_connect_unix_now(longname, NetUnixName::Path, &s, 1.0, &err) && err == ENAMETOOLONG);
#if defined(__linux__)
    char abs_name[64];
    snprintf(abs_name, sizeof abs_name, "kv-net-unix-%d", (int)getpid());
    l = listen_on(abs_name, true);
    KV_CHECK(c, l >= 0);
    KV_CHECK(c, net_connect_unix_now(abs_name, NetUnixName::Abstract, &s, 2.0, &err) && s.is_open());
    s.close();
    close(l);
    KV_CHECK(c, !net_connect_unix_now(abs_name, NetUnixName::Abstract, &s, 1.0, &err) && err == ECONNREFUSED);
#else
    KV_CHECK(c, !net_connect_unix_now("x", NetUnixName::Abstract, &s, 1.0, &err) && err == EAFNOSUPPORT);
#endif
#endif
}
