// Korvid platform: NetSocket::write_now, the blocking send (net.h), over each OS's one
// send and one wait (net_internal.h). The deadline, the queue and what is left on a
// timeout are the same everywhere.
#include "net_internal.h"
#include "misuse.h"
#include "korvid/core/time.h"
#include <string.h>

namespace kv {

namespace {

constexpr f64 kWaitSlice = 0.25; // the longest one wait for room lasts, with no deadline

// The time left before `deadline` (time_now's clock), or a slice when there is none; 0
// or less when it has passed.
f64 time_left(f64 deadline) { return deadline < 0.0 ? kWaitSlice : deadline - time_now(); }

void wait_some(NetSocket& s, f64 deadline) {
    f64 left = time_left(deadline);
    net_wait_writable(s, left < kWaitSlice ? left : kWaitSlice);
}

// Sends `data` until it has all gone, the connection is over, or the deadline passes.
// What went is in *sent.
NetSend send_until(NetSocket& s, const u8* data, u32 size, f64 deadline, u32* sent) {
    *sent = 0;
    while (*sent < size) {
        u32 n = 0;
        NetSend r = net_send_some(s, data + *sent, size - *sent, &n);
        if (r == NetSend::Over) return r;
        *sent += n;
        if (r == NetSend::Some) continue;
        if (time_left(deadline) <= 0.0) return NetSend::Again;
        wait_some(s, deadline);
    }
    return NetSend::Some;
}

// What the kernel did not take, onto the queue for whoever sends next. Past out_limit if
// need be: these bytes were given to write_now, which drops nothing.
void queue_rest(NetSocket& s, const u8* data, u32 size) {
    if (size) memcpy(s.out.push_n(size), data, size);
}

bool send_now(NetSocket& s, const u8* bytes, u32 size, f64 seconds) {
    f64 deadline = seconds >= 0.0 ? time_now() + seconds : -1.0;
    while (s.link() == NetLink::Connecting) {
        if (time_left(deadline) <= 0.0) { queue_rest(s, bytes, size); return false; } // flush sends it once the connect is up
        wait_some(s, deadline);
    }
    if (s.closed) return false; // the connect failed: `error` says why
    u32 sent = 0;
    if (s.out.len) {
        NetSend r = send_until(s, s.out.data, s.out.len, deadline, &sent);
        if (sent) {
            memmove(s.out.data, s.out.data + sent, s.out.len - sent);
            s.out.len -= sent;
        }
        if (r == NetSend::Over) return false;
        if (r == NetSend::Again) { queue_rest(s, bytes, size); return false; }
    }
    NetSend r = send_until(s, bytes, size, deadline, &sent);
    if (r == NetSend::Again) queue_rest(s, bytes + sent, size - sent);
    return r == NetSend::Some;
}

} // namespace

bool NetSocket::write_now(const void* data, u32 size, f64 seconds) {
    if (!os_handle_open(fd) || closed) { misuse_socket(*this, "write_now"); return false; }
    bool all = send_now(*this, (const u8*)data, size, seconds);
    net_mark_queued(*this); // what a watch of it reads, on every way out
    return all;
}

} // namespace kv
