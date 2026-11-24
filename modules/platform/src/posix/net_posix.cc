#include "korvid/platform/net.h"
#include "korvid/core/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

bool NetListener::open(u16 want_port, bool local_only) {
    close();
    int s = tcp_socket();
    if (s < 0) { KV_ERROR("net", "socket: %s", strerror(errno)); return false; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(want_port);
    addr.sin_addr.s_addr = htonl(local_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&addr, sizeof addr) != 0 || listen(s, 8) != 0 || !set_nonblocking(s)) {
        KV_ERROR("net", "listen on port %u: %s", want_port, strerror(errno));
        ::close(s);
        return false;
    }
    socklen_t len = sizeof addr;
    if (getsockname(s, (sockaddr*)&addr, &len) == 0) port = ntohs(addr.sin_port);
    fd = khandle_make(KHandleKind::Socket, s);
    return true;
}

void NetListener::close() {
    if (fd.is_open()) ::close((int)fd.os());
    fd = KHandle{};
    port = 0;
}

KHandle NetListener::accept() {
    if (!fd.is_open()) return KHandle{};
#if KV_NET_APPLE
    // A socket accepted from a non-blocking listener does not inherit O_NONBLOCK on macOS
    // (it does not on Linux either, which is why accept4 is asked for it there).
    int c = ::accept((int)fd.os(), nullptr, nullptr);
    if (c >= 0) { apple_socket_flags(c); set_nonblocking(c); }
#else
    int c = ::accept4((int)fd.os(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
    if (c < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) KV_WARN("net", "accept: %s", strerror(errno));
        return -1;
    }
    return khandle_make(KHandleKind::Socket, c);
}

void NetSocket::adopt(KHandle new_fd) {
    close();
    if (new_fd.is_open() && new_fd.kind() != KHandleKind::Socket) {
        KV_ERROR("net", "adopt: a %s handle is not a socket", khandle_kind_name(new_fd.kind()));
        return;
    }
    fd = new_fd;
    closed = false;
    if (fd.is_open()) {
        set_nonblocking((int)fd.os());
        int one = 1;
        setsockopt((int)fd.os(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // lines, not bulk
    }
}

i32 NetSocket::read(void* buf, u32 cap) {
    if (!fd.is_open() || closed || cap == 0) return closed ? -1 : 0;
    ssize_t n = ::recv((int)fd.os(), buf, cap, 0);
    if (n > 0) { bytes_in += (u64)n; return (i32)n; }
    if (n == 0) { closed = true; return -1; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    closed = true;
    return -1;
}

bool NetSocket::write(const void* data, u32 len) {
    if (!fd.is_open() || closed) return false;
    if (out.len + len > out_limit) { dropped += len; return false; }
    memcpy(out.push_n(len), data, len);
    return true;
}

bool NetSocket::write_str(const char* s) { return write(s, (u32)strlen(s)); }

void NetSocket::flush() {
    if (!fd.is_open() || closed || out.len == 0) return;
    u32 sent = 0;
    while (sent < out.len) {
        ssize_t n = ::send((int)fd.os(), out.data + sent, out.len - sent, kSendFlags);
        if (n > 0) { sent += (u32)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        if (n < 0 && (errno == ENOTCONN || errno == EINPROGRESS)) break; // still connecting
        closed = true;
        break;
    }
    bytes_out += sent;
    if (sent) {
        memmove(out.data, out.data + sent, out.len - sent);
        out.len -= sent;
    }
}

void NetSocket::close() {
    if (fd.is_open()) ::close((int)fd.os());
    fd = KHandle{};
    out.clear();
    closed = false;
}

bool net_connect_local(u16 port, NetSocket& out) {
    int s = tcp_socket();
    if (s < 0) return false;
    set_nonblocking(s);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&addr, sizeof addr) != 0 && errno != EINPROGRESS) {
        KV_ERROR("net", "connect to port %u: %s", port, strerror(errno));
        ::close(s);
        return false;
    }
    out.adopt(khandle_make(KHandleKind::Socket, s));
    return true;
}

void net_close(KHandle fd) { if (fd.is_open()) ::close((int)fd.os()); }

} // namespace kv
