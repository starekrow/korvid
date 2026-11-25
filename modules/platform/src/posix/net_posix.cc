#include "korvid/platform/net.h"
#include "korvid/core/os_error.h"
#include "../os_err.h"
#include "../misuse.h"
#include "../net_internal.h"
#include "../os_handle_table.h"
#include "korvid/core/log.h"
#include "korvid/core/format.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Linux sets close-on-exec and non-blocking as a socket is made (SOCK_CLOEXEC, accept4) and
// suppresses SIGPIPE per send (MSG_NOSIGNAL). macOS has neither creation flag nor accept4,
// so it sets the descriptor flags right after, and suppresses SIGPIPE once per socket with
// SO_NOSIGPIPE. Nothing here forks between the two calls, so the gap is not a leak.
#if defined(__APPLE__)
#define KV_NET_APPLE 1
#else
#define KV_NET_APPLE 0
#endif

namespace kv {

namespace {

// The slot first, then the descriptor: every copy of the handle is stale before the OS reuses it.
void close_socket(Handle fd) {
    i64 v = os_handle_release(fd);
    if (v >= 0) ::close((int)v);
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#if KV_NET_APPLE
// What Linux gets from SOCK_CLOEXEC and MSG_NOSIGNAL, for a descriptor made without them.
void apple_socket_flags(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}
constexpr int kSendFlags = 0;
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

// A TCP socket that is not inherited by child processes and never raises SIGPIPE.
int tcp_socket() {
#if KV_NET_APPLE
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) apple_socket_flags(s);
    return s;
#else
    return socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
}

} // namespace

bool NetListener::open(u16 want_port, NetScope scope, i32* err) {
    close();
    auto fail = [&](i32 why) { error = why; return os_fail(err, why); };
    int s = tcp_socket();
    if (s < 0) return fail(errno);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(want_port);
    addr.sin_addr.s_addr = htonl(scope == NetScope::Loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&addr, sizeof addr) != 0 || listen(s, 8) != 0 || !set_nonblocking(s)) {
        int e = errno;
        ::close(s);
        return fail(e);
    }
    socklen_t len = sizeof addr;
    if (getsockname(s, (sockaddr*)&addr, &len) == 0) port = ntohs(addr.sin_port);
    char what[48];
    kv::format(what, sizeof what, "listener %s:%u", scope == NetScope::Loopback ? "127.0.0.1" : "0.0.0.0", port);
    fd = os_handle_make(OsKind::Socket, s, what);
    error = 0;
    return true;
}

void NetListener::close() {
    close_socket(fd);
    fd = Handle{};
    port = 0;
    warned = false;
}

Handle NetListener::accept() {
    if (!os_handle_open(fd)) { misuse_listener(*this); return Handle{}; }
#if KV_NET_APPLE
    // A socket accepted from a non-blocking listener does not inherit O_NONBLOCK on macOS
    // (it does not on Linux either, which is why accept4 is asked for it there).
    int c = ::accept((int)os_handle_value(fd), nullptr, nullptr);
    if (c >= 0) { apple_socket_flags(c); set_nonblocking(c); }
#else
    int c = ::accept4((int)os_handle_value(fd), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
    if (c < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) KV_WARN("net", "accept: %s", os_error_text(errno));
        return Handle{};
    }
    char what[48];
    kv::format(what, sizeof what, "accepted on :%u", port);
    return os_handle_make(OsKind::Socket, c, what);
}

void NetSocket::adopt(Handle new_fd) {
    close();
    if (os_handle_open(new_fd) && os_handle_kind(new_fd) != OsKind::Socket) {
        KV_ERROR("net", "adopt: a %s handle is not a socket", os_kind_name(os_handle_kind(new_fd)));
        return;
    }
    fd = new_fd;
    closed = false;
    error = 0;
    warned = false;
    link_state = os_handle_open(fd) ? NetLink::Up : NetLink::None; // accepted: connected already. net_connect_local says Connecting
    if (os_handle_open(fd)) {
        set_nonblocking((int)os_handle_value(fd));
        int one = 1;
        setsockopt((int)os_handle_value(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // lines, not bulk
    }
}

NetLink NetSocket::link() {
    if (link_state != NetLink::Connecting || !os_handle_open(fd)) return link_state;
    // A connect that has finished, either way, makes the socket writable; SO_ERROR then
    // says which way. Asked without waiting.
    struct pollfd p = {(int)os_handle_value(fd), POLLOUT, 0};
    if (::poll(&p, 1, 0) <= 0) return link_state;
    int e = 0;
    socklen_t len = sizeof e;
    if (getsockopt((int)os_handle_value(fd), SOL_SOCKET, SO_ERROR, &e, &len) != 0) e = errno;
    if (e) { error = e; closed = true; link_state = NetLink::Failed; }
    else link_state = NetLink::Up;
    return link_state;
}

i32 NetSocket::read(void* buf, u32 cap) {
    if (link_state == NetLink::Connecting && link() == NetLink::Connecting) return 0; // nothing can have arrived yet
    if (!os_handle_open(fd) && !closed) { misuse_socket(*this, "read"); return 0; }
    if (closed || cap == 0) return closed ? -1 : 0;
    ssize_t n = ::recv((int)os_handle_value(fd), buf, cap, 0);
    if (n > 0) { bytes_in += (u64)n; os_handle_count(fd, (u64)n, 0); return (i32)n; }
    if (n == 0) { closed = true; return -1; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    error = errno;
    closed = true;
    return -1;
}

bool NetSocket::write(const void* data, u32 size) {
    if (!os_handle_open(fd) || closed) { misuse_socket(*this, "write"); return false; }
    if (out.len + size > out_limit) { dropped += size; return false; }
    memcpy(out.push_n(size), data, size);
    net_mark_queued(*this);
    return true;
}

// write_now's two parts (net_internal.h, net_now.cc).
NetSend net_send_some(NetSocket& sock, const u8* data, u32 size, u32* sent) {
    *sent = 0;
    for (;;) {
        ssize_t n = ::send((int)os_handle_value(sock.fd), data, size, kSendFlags);
        if (n > 0) {
            *sent = (u32)n;
            sock.bytes_out += (u64)n;
            os_handle_count(sock.fd, 0, (u64)n);
            return NetSend::Some;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return NetSend::Again;
        sock.error = n < 0 ? errno : 0;
        sock.closed = true;
        return NetSend::Over;
    }
}

void net_wait_writable(NetSocket& sock, f64 seconds) {
    struct pollfd p = {(int)os_handle_value(sock.fd), POLLOUT, 0};
    int ms = seconds <= 0.0 ? 0 : (int)(seconds * 1000.0 + 0.999);
    ::poll(&p, 1, ms);
}

void net_wait_readable(Handle fd, f64 seconds) {
    i64 v = os_handle_value(fd, OsKind::Socket);
    if (v < 0) return;
    struct pollfd p = {(int)v, POLLIN, 0};
    int ms = seconds <= 0.0 ? 0 : (int)(seconds * 1000.0 + 0.999);
    ::poll(&p, 1, ms);
}

bool NetSocket::write_str(const char* s) { return write(s, (u32)strlen(s)); }

void NetSocket::flush() {
    if (!os_handle_open(fd) || closed || out.len == 0) return;
    u32 sent = 0;
    while (sent < out.len) {
        ssize_t n = ::send((int)os_handle_value(fd), out.data + sent, out.len - sent, kSendFlags);
        if (n > 0) { sent += (u32)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        if (n < 0 && (errno == ENOTCONN || errno == EINPROGRESS)) break; // still connecting
        error = n < 0 ? errno : 0;
        closed = true;
        break;
    }
    bytes_out += sent;
    if (sent) os_handle_count(fd, 0, sent);
    if (sent) {
        memmove(out.data, out.data + sent, out.len - sent);
        out.len -= sent;
        net_mark_queued(*this);
    }
}

void NetSocket::close() {
    close_socket(fd);
    fd = Handle{};
    out.free(); // the queue's memory with it: its allocator stays, for the next use
    closed = false;
    link_state = NetLink::None;
    warned = false;
}

bool net_connect_local(u16 port, NetSocket* out, i32* err) {
    int s = tcp_socket();
    if (s < 0) return os_fail(err, errno);
    set_nonblocking(s);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&addr, sizeof addr) != 0 && errno != EINPROGRESS) {
        int e = errno;
        ::close(s);
        return os_fail(err, e);
    }
    char what[48];
    kv::format(what, sizeof what, "to 127.0.0.1:%u", port);
    out->adopt(os_handle_make(OsKind::Socket, s, what));
    out->link_state = NetLink::Connecting; // link() finds out which way it went
    return true;
}

void net_close(Handle fd) { close_socket(fd); }

} // namespace kv
