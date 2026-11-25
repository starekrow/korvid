// Korvid platform: what each OS's sockets (posix/net_posix.cc, win32/net_win32.cc) give
// NetSocket::write_now (net_now.cc), so its waiting and its deadline are written once.
#pragma once
#include "korvid/platform/net.h"
#include "os_handle_table.h"

namespace kv {

enum class NetSend : u8 {
    Some,  // the kernel took *sent bytes (counted in bytes_out), maybe all
    Again, // it will take none now: wait until the socket can be written
    Over,  // the connection is over: `closed` and `error` are set
};
// One send of what the kernel will take now, without waiting. Loop's thread.
NetSend net_send_some(NetSocket& s, const u8* data, u32 size, u32* sent);
// Until the socket can be written, or `seconds` pass: a connect settled either way, room in
// the kernel's buffer, or an error for the next send to find.
void net_wait_writable(NetSocket& s, f64 seconds);
// What a watch of the socket reads (kOsQueued): whether it has output queued. After every
// change to `out`: write, flush, write_now.
inline void net_mark_queued(const NetSocket& s) { os_handle_set_flag(s.fd, kOsQueued, s.out.len != 0); }

} // namespace kv
