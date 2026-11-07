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

namespace kv {

namespace {

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

bool NetListener::open(u16 want_port, bool local_only) {
    close();
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
    fd = s;
    return true;
}

void NetListener::close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    port = 0;
}

int NetListener::accept() {
    if (fd < 0) return -1;
    int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (c < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) KV_WARN("net", "accept: %s", strerror(errno));
        return -1;
    }
    return c;
}

void NetSocket::adopt(int new_fd) {
    close();
    fd = new_fd;
    closed = false;
    if (fd >= 0) {
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // lines, not bulk
    }
}

i32 NetSocket::read(void* buf, u32 cap) {
    if (fd < 0 || closed || cap == 0) return closed ? -1 : 0;
    ssize_t n = ::recv(fd, buf, cap, 0);
    if (n > 0) { bytes_in += (u64)n; return (i32)n; }
    if (n == 0) { closed = true; return -1; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    closed = true;
    return -1;
}

bool NetSocket::write(const void* data, u32 len) {
    if (fd < 0 || closed) return false;
    if (out.len + len > out_limit) { dropped += len; return false; }
    memcpy(out.push_n(len), data, len);
    return true;
}

bool NetSocket::write_str(const char* s) { return write(s, (u32)strlen(s)); }

void NetSocket::flush() {
    if (fd < 0 || closed || out.len == 0) return;
    u32 sent = 0;
    while (sent < out.len) {
        ssize_t n = ::send(fd, out.data + sent, out.len - sent, MSG_NOSIGNAL);
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
    if (fd >= 0) ::close(fd);
    fd = -1;
    out.clear();
    closed = false;
}

bool net_connect_local(u16 port, NetSocket& out) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
    out.adopt(s);
    return true;
}

} // namespace kv
