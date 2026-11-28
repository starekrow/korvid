// Korvid platform: KR49's Unix-domain sockets on Windows: not yet (net.h). Windows' AF_UNIX
// takes paths but passes no descriptors and has no socketpair; this is filled when
// something on Windows needs it.
#include "korvid/platform/net.h"
#include "../os_err.h"
#include <errno.h>

namespace kv {

bool net_connect_unix_now(const char*, NetUnixName, NetSocket* out, f64, i32* err) {
    out->close();
    return os_fail(err, EAFNOSUPPORT);
}

bool net_socket_pair(NetSocket* a, NetSocket* b, i32* err) {
    a->close();
    b->close();
    return os_fail(err, EAFNOSUPPORT);
}

i32 NetSocket::read_with(void* buf, u32 cap, Handle*, u32, u32* got) {
    *got = 0;
    return read(buf, cap);
}

bool NetSocket::write_now_with(const void* data, u32 size, const Handle*, u32 count, f64 seconds) {
    if (count == 0) return write_now(data, size, seconds);
    error = EAFNOSUPPORT;
    return false;
}

bool NetWaker::open(i32* err) { return os_fail(err, EAFNOSUPPORT); }
void NetWaker::close() { read_end = write_end = -1; }
void NetWaker::wake() {}

u8 net_wait(NetSocket&, bool, NetWaker*, f64) { return 0; }

} // namespace kv
