// Non-blocking TCP over Winsock. Where Windows differs from POSIX: Winsock starts once per
// process (here, on first use); a would-block connect reports WSAEWOULDBLOCK, not
// EINPROGRESS; SO_REUSEADDR would let another process take the port, so a listener holds
// it with SO_EXCLUSIVEADDRUSE instead; and there is no SIGPIPE to suppress.
#include "korvid/platform/net.h"
#include "../os_err.h"
#include "../os_handle_table.h"
#include "korvid/core/format.h"
#include "korvid/core/log.h"
#include "wide.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <errno.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")

namespace kv {

namespace {

bool winsock_up() {
    static bool up = false;
    if (!up) {
        WSADATA data;
        up = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!up) KV_ERROR("net", "Winsock would not start");
    }
    return up;
}

const char* net_error(int code) {
    static char text[256];
    wchar_t w[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)code, 0, w, 256, nullptr);
    while (n > 0 && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L'.')) w[--n] = 0;
    if (n == 0 || !narrow(w, text, (int)sizeof text)) kv::format(text, sizeof text, "Winsock error %d", code);
    return text;
}

// Winsock's error as the errno POSIX would have given, for the callers' `i32* err`.
i32 errno_from_wsa(int code) {
    switch (code) {
    case WSAEADDRINUSE:    return EADDRINUSE;
    case WSAEACCES:        return EACCES;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAECONNREFUSED:  return ECONNREFUSED;
    case WSAECONNRESET:    return ECONNRESET;
    case WSAENETUNREACH:   return ENETUNREACH;
    case WSAEHOSTUNREACH:  return EHOSTUNREACH;
    case WSAETIMEDOUT:     return ETIMEDOUT;
    case WSAEMFILE:        return EMFILE;
    case WSAENOBUFS:       return ENOBUFS;
    case WSAEINVAL:        return EINVAL;
    case WSANOTINITIALISED: case WSASYSNOTREADY: case WSAENETDOWN: return ENETDOWN;
    default:               return EIO;
    }
}

SOCKET socket_of(Handle fd) { return (SOCKET)os_handle_value(fd); }
Handle handle_of(SOCKET s, const char* what) { return s == INVALID_SOCKET ? Handle{} : os_handle_make(OsKind::Socket, (i64)s, what); }
// The slot first, then the socket: every copy of the handle is stale before the OS reuses the value.
void close_socket(Handle fd) {
    i64 v = os_handle_release(fd);
    if (v >= 0) closesocket((SOCKET)v);
}

bool set_nonblocking(SOCKET s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}

bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS; }

// Not inherited by child processes, as SOCK_CLOEXEC makes a descriptor.
SOCKET new_socket() {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    return s;
}

} // namespace

bool NetListener::open(u16 want_port, NetScope scope, i32* err) {
    close();
    if (!winsock_up()) return os_fail(err, ENETDOWN);
    SOCKET s = new_socket();
    if (s == INVALID_SOCKET) return os_fail(err, errno_from_wsa(WSAGetLastError()));
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(want_port);
    addr.sin_addr.s_addr = htonl(scope == NetScope::Loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&addr, sizeof addr) != 0 || listen(s, 8) != 0 || !set_nonblocking(s)) {
        int e = WSAGetLastError();
        closesocket(s);
        return os_fail(err, errno_from_wsa(e));
    }
    int len = sizeof addr;
    if (getsockname(s, (sockaddr*)&addr, &len) == 0) port = ntohs(addr.sin_port);
    char what[48];
    kv::format(what, sizeof what, "listener %s:%u", scope == NetScope::Loopback ? "127.0.0.1" : "0.0.0.0", port);
    fd = handle_of(s, what);
    return true;
}

void NetListener::close() {
    close_socket(fd);
    fd = Handle{};
    port = 0;
}

Handle NetListener::accept() {
    if (!os_handle_open(fd)) return Handle{};
    SOCKET c = ::accept(socket_of(fd), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        int e = WSAGetLastError();
        if (!would_block(e)) KV_WARN("net", "accept: %s", net_error(e));
        return Handle{};
    }
    // An accepted socket takes the listener's non-blocking mode; set it anyway, and keep
    // it out of child processes.
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    char what[48];
    kv::format(what, sizeof what, "accepted on :%u", port);
    return handle_of(c, what);
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
    link_state = os_handle_open(fd) ? NetLink::Up : NetLink::None; // accepted: connected already. net_connect_local says Connecting
    if (os_handle_open(fd)) {
        set_nonblocking(socket_of(fd));
        BOOL one = TRUE;
        setsockopt(socket_of(fd), IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one); // lines, not bulk
    }
}

NetLink NetSocket::link() {
    if (link_state != NetLink::Connecting || !os_handle_open(fd)) return link_state;
    // A connect that has finished makes the socket writable; one that failed is in the
    // exception set (and, on later Windows, writable too). Asked without waiting.
    SOCKET s = socket_of(fd);
    fd_set can_write, failed;
    FD_ZERO(&can_write); FD_ZERO(&failed);
    FD_SET(s, &can_write); FD_SET(s, &failed);
    timeval none{0, 0};
    if (select(0, nullptr, &can_write, &failed, &none) <= 0) return link_state;
    int e = 0, len = sizeof e;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &len) != 0) e = WSAGetLastError();
    if (e == 0 && FD_ISSET(s, &failed)) e = WSAECONNREFUSED;
    if (e) { error = errno_from_wsa(e); closed = true; link_state = NetLink::Failed; }
    else link_state = NetLink::Up;
    return link_state;
}

i32 NetSocket::read(void* buf, u32 cap) {
    if (link_state == NetLink::Connecting && link() == NetLink::Connecting) return 0; // nothing can have arrived yet
    if (!os_handle_open(fd) || closed || cap == 0) return closed ? -1 : 0;
    int n = ::recv(socket_of(fd), (char*)buf, (int)(cap > 0x7FFFFFFFu ? 0x7FFFFFFFu : cap), 0);
    if (n > 0) { bytes_in += (u64)n; os_handle_count(fd, (u64)n, 0); return (i32)n; }
    if (n == 0) { closed = true; return -1; }
    int e = WSAGetLastError();
    if (would_block(e)) return 0;
    error = errno_from_wsa(e);
    closed = true;
    return -1;
}

bool NetSocket::write(const void* data, u32 size) {
    if (!os_handle_open(fd) || closed) return false;
    if (out.len + size > out_limit) { dropped += size; return false; }
    memcpy(out.push_n(size), data, size);
    return true;
}

bool NetSocket::write_str(const char* s) { return write(s, (u32)strlen(s)); }

void NetSocket::flush() {
    if (!os_handle_open(fd) || closed || out.len == 0) return;
    u32 sent = 0;
    while (sent < out.len) {
        int n = ::send(socket_of(fd), (const char*)out.data + sent, (int)(out.len - sent), 0);
        if (n > 0) { sent += (u32)n; continue; }
        int e = WSAGetLastError();
        if (would_block(e)) break;
        if (e == WSAENOTCONN) break; // still connecting
        error = errno_from_wsa(e);
        closed = true;
        break;
    }
    bytes_out += sent;
    if (sent) os_handle_count(fd, 0, sent);
    if (sent) {
        memmove(out.data, out.data + sent, out.len - sent);
        out.len -= sent;
    }
}

void NetSocket::close() {
    close_socket(fd);
    fd = Handle{};
    out.free(); // the queue's memory with it: its allocator stays, for the next use
    closed = false;
    link_state = NetLink::None;
}

bool net_connect_local(u16 port, NetSocket* out, i32* err) {
    if (!winsock_up()) return os_fail(err, ENETDOWN);
    SOCKET s = new_socket();
    if (s == INVALID_SOCKET) return os_fail(err, errno_from_wsa(WSAGetLastError()));
    set_nonblocking(s);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&addr, sizeof addr) != 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) {
            closesocket(s);
            return os_fail(err, errno_from_wsa(e));
        }
    }
    char what[48];
    kv::format(what, sizeof what, "to 127.0.0.1:%u", port);
    out->adopt(handle_of(s, what));
    out->link_state = NetLink::Connecting; // link() finds out which way it went
    return true;
}

void net_close(Handle fd) { close_socket(fd); }

} // namespace kv
