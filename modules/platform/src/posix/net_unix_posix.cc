// Korvid platform: Unix-domain sockets, descriptors across them, and a wait another thread
// can end (net.h, KR49). Linux and macOS; the abstract namespace is Linux's.
#include "korvid/platform/net.h"
#include "korvid/core/format.h"
#include "korvid/core/time.h"
#include "../blocking.h"
#include "../misuse.h"
#include "../net_internal.h"
#include "../os_err.h"
#include "../os_handle_table.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

#if defined(__APPLE__)
#define KV_NET_APPLE 1
#else
#define KV_NET_APPLE 0
#endif

namespace kv {

// Named, not anonymous: a unity build puts this beside net_posix.cc, which has its own.
namespace net_unix {

#if KV_NET_APPLE
constexpr int kSendFlags = 0;
void cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }
void no_sigpipe(int fd) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

int unix_socket() {
#if KV_NET_APPLE
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s >= 0) { cloexec(s); no_sigpipe(s); }
    return s;
#else
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
}

int poll_ms(f64 seconds) {
    if (seconds < 0.0) return -1;
    f64 ms = seconds * 1000.0 + 0.999;
    return ms > 2.0e9 ? 2000000000 : (int)ms;
}

// A descriptor that arrived, as a handle of the kind it is.
Handle adopt_received(int fd) {
#if KV_NET_APPLE
    cloexec(fd);
#endif
    struct stat st;
    OsKind kind = OsKind::File;
    if (fstat(fd, &st) == 0) {
        if (S_ISSOCK(st.st_mode)) kind = OsKind::Socket;
        else if (S_ISFIFO(st.st_mode)) kind = OsKind::Pipe;
    }
    return os_handle_make(kind, fd, "received over a Unix-domain socket");
}

} // namespace net_unix

bool net_connect_unix_now(const char* name, NetUnixName kind, NetSocket* out, f64 seconds, i32* err) {
    BlockingCall blocking("net_connect_unix_now", name);
    out->close();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    usize n = strlen(name);
    usize room = sizeof addr.sun_path - 1; // a path keeps its NUL; an abstract name its leading one
    if (n == 0 || n > room) return os_fail(err, n == 0 ? EINVAL : ENAMETOOLONG);
    socklen_t len;
    if (kind == NetUnixName::Abstract) {
#if defined(__linux__)
        memcpy(addr.sun_path + 1, name, n);
        len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + n);
#else
        return os_fail(err, EAFNOSUPPORT);
#endif
    } else {
        memcpy(addr.sun_path, name, n);
        len = (socklen_t)(offsetof(sockaddr_un, sun_path) + n + 1);
    }
    int s = net_unix::unix_socket();
    if (s < 0) return os_fail(err, errno);
    net_unix::set_nonblocking(s);
    // A Unix-domain connect is answered at once, except when the listener's backlog is
    // full (EAGAIN on Linux, where it is not retried for us): then it is asked again.
    f64 deadline = seconds >= 0.0 ? time_now() + seconds : -1.0;
    for (;;) {
        if (connect(s, (sockaddr*)&addr, len) == 0) break;
        int e = errno;
        if (e == EINTR) continue;
        if (e == EINPROGRESS) { // macOS may say so: settle it the way TCP's is
            struct pollfd p = {s, POLLOUT, 0};
            f64 left = deadline < 0.0 ? -1.0 : deadline - time_now();
            if (deadline >= 0.0 && left <= 0.0) { ::close(s); return os_fail(err, ETIMEDOUT); }
            ::poll(&p, 1, net_unix::poll_ms(left));
            int se = 0; socklen_t sl = sizeof se;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &se, &sl) != 0) se = errno;
            if (se == 0 && (p.revents & POLLOUT)) break;
            if (se == 0) { ::close(s); return os_fail(err, ETIMEDOUT); }
            ::close(s);
            return os_fail(err, se);
        }
        if (e == EAGAIN && (deadline < 0.0 || time_now() < deadline)) { time_sleep(0.001); continue; }
        ::close(s);
        return os_fail(err, e == EAGAIN ? ETIMEDOUT : e);
    }
    char what[160];
    kv::format(what, sizeof what, "unix %s%s", kind == NetUnixName::Abstract ? "@" : "", name);
    out->adopt(os_handle_make(OsKind::Socket, s, what));
    return true;
}

bool net_socket_pair(NetSocket* a, NetSocket* b, i32* err) {
    a->close();
    b->close();
    int sv[2];
#if KV_NET_APPLE
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return os_fail(err, errno);
    for (int fd : sv) { net_unix::cloexec(fd); net_unix::no_sigpipe(fd); }
#else
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return os_fail(err, errno);
#endif
    a->adopt(os_handle_make(OsKind::Socket, sv[0], "socket pair, end 0"));
    b->adopt(os_handle_make(OsKind::Socket, sv[1], "socket pair, end 1"));
    return true;
}

i32 NetSocket::read_with(void* buf, u32 cap, Handle* handles, u32 handle_cap, u32* got) {
    *got = 0;
    if (!os_handle_open(fd) && !closed) { misuse_socket(*this, "read_with"); return 0; }
    if (closed || cap == 0) return closed ? -1 : 0;
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * kNetMaxHandles)];
    struct iovec iov = {buf, cap};
    struct msghdr m{};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = sizeof control;
#if KV_NET_APPLE
    int flags = 0;
#else
    int flags = MSG_CMSG_CLOEXEC;
#endif
    ssize_t n;
    do n = ::recvmsg((int)os_handle_value(fd), &m, flags); while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        error = errno;
        closed = true;
        return -1;
    }
    // Descriptors first, whatever else happened: each that came is this process's now.
    for (struct cmsghdr* h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
        if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
        u32 count = (u32)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        const unsigned char* data = CMSG_DATA(h);
        for (u32 i = 0; i < count; ++i) {
            int rfd;
            memcpy(&rfd, data + i * sizeof(int), sizeof rfd);
            if (*got < handle_cap) handles[(*got)++] = net_unix::adopt_received(rfd);
            else { ::close(rfd); ++handles_dropped; }
        }
    }
    if (m.msg_flags & MSG_CTRUNC) ++handles_dropped; // more than kNetMaxHandles came: the kernel closed the rest
    if (n == 0) { closed = true; return -1; }
    bytes_in += (u64)n;
    os_handle_count(fd, (u64)n, 0);
    return (i32)n;
}

bool NetSocket::write_now_with(const void* data, u32 size, const Handle* handles, u32 count, f64 seconds) {
    BlockingCall blocking("NetSocket::write_now_with", nullptr);
    if (!os_handle_open(fd) || closed) { misuse_socket(*this, "write_now_with"); return false; }
    if (count == 0) return write_now(data, size, seconds);
    int fds[kNetMaxHandles];
    if (size == 0 || count > kNetMaxHandles) { error = EINVAL; return false; }
    for (u32 i = 0; i < count; ++i) {
        i64 v = os_handle_value(handles[i]);
        if (v < 0) { error = EINVAL; return false; }
        fds[i] = (int)v;
    }
    f64 deadline = seconds >= 0.0 ? time_now() + seconds : -1.0;
    // What was queued goes first, so the descriptors ride the first byte of `data`.
    if (out.len && !write_now(nullptr, 0, seconds)) return false;
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * kNetMaxHandles)];
    memset(control, 0, sizeof control);
    struct iovec iov = {(void*)data, size};
    struct msghdr m{};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = CMSG_SPACE(sizeof(int) * count);
    struct cmsghdr* h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(h), fds, sizeof(int) * count);
    for (;;) {
        ssize_t n = ::sendmsg((int)os_handle_value(fd), &m, net_unix::kSendFlags);
        if (n > 0) {
            bytes_out += (u64)n;
            os_handle_count(fd, 0, (u64)n);
            if ((u32)n == size) return true;
            f64 left = deadline < 0.0 ? -1.0 : deadline - time_now();
            if (deadline >= 0.0 && left < 0.0) left = 0.0;
            return write_now((const u8*)data + n, size - (u32)n, left); // the descriptors have gone with the first
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            f64 left = deadline < 0.0 ? 0.25 : deadline - time_now();
            if (left <= 0.0) { error = ETIMEDOUT; return false; } // nothing went: the caller may try again
            net_wait_writable(*this, left < 0.25 ? left : 0.25);
            continue;
        }
        error = n < 0 ? errno : 0;
        closed = true;
        return false;
    }
}

bool NetWaker::open(i32* err) {
    close();
#if defined(__linux__)
    int e = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (e < 0) return os_fail(err, errno);
    read_end = write_end = e;
#else
    int p[2];
    if (pipe(p) != 0) return os_fail(err, errno);
    for (int fd : p) { net_unix::cloexec(fd); net_unix::set_nonblocking(fd); }
    read_end = p[0];
    write_end = p[1];
#endif
    return true;
}

void NetWaker::close() {
    if (read_end >= 0) ::close((int)read_end);
    if (write_end >= 0 && write_end != read_end) ::close((int)write_end);
    read_end = write_end = -1;
}

void NetWaker::wake() {
    if (write_end < 0) return;
#if defined(__linux__)
    u64 one = 1;
    ssize_t r = ::write((int)write_end, &one, sizeof one);
#else
    u8 one = 1;
    ssize_t r = ::write((int)write_end, &one, 1); // a full pipe is already a wake
#endif
    (void)r;
}

u8 net_wait(NetSocket& s, bool want_write, NetWaker* waker, f64 seconds) {
    BlockingCall blocking("net_wait", nullptr);
    i64 v = os_handle_value(s.fd, OsKind::Socket);
    if (v >= 0 && s.closed) return kNetReadable; // over already: the next read says so
    struct pollfd p[2];
    nfds_t n = 0;
    if (v >= 0) p[n++] = {(int)v, (short)(POLLIN | (want_write ? POLLOUT : 0)), 0};
    else if (v < 0) misuse_socket(s, "net_wait");
    int wi = -1;
    if (waker && waker->read_end >= 0) { wi = (int)n; p[n++] = {(int)waker->read_end, POLLIN, 0}; }
    if (n == 0) { if (seconds > 0.0) time_sleep(seconds); return 0; }
    f64 deadline = seconds >= 0.0 ? time_now() + seconds : -1.0;
    int r;
    for (;;) {
        f64 left = deadline < 0.0 ? -1.0 : deadline - time_now();
        r = ::poll(p, n, deadline < 0.0 ? -1 : net_unix::poll_ms(left > 0.0 ? left : 0.0));
        if (r >= 0 || errno != EINTR) break;
    }
    if (r <= 0) return 0;
    u8 got = 0;
    if (v >= 0) {
        short re = p[0].revents;
        if (re & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) got |= kNetReadable;
        if (re & POLLOUT) got |= kNetWritable;
    }
    if (wi >= 0 && (p[wi].revents & POLLIN)) {
        got |= kNetWoken;
        u8 drain[64];
        while (::read((int)waker->read_end, drain, sizeof drain) > 0) {}
    }
    return got;
}

} // namespace kv
