// Korvid platform: non-blocking TCP. A listener accepts; a socket reads what has arrived
// and queues what is to be sent, draining it when the pump flushes. Nothing here blocks
// and nothing spawns a thread: whoever owns a socket polls it once per frame. The CLI's
// transport, and a game's if it wants one. IPv4 loopback by default; a listener may open
// to every interface, and should think before it does.
#pragma once
#include "korvid/core/array.h"
#include "korvid/platform/khandle.h"

namespace kv {

struct NetListener {
    KHandle fd;      // KHandleKind::Socket
    u16   port = 0; // the bound port; what the OS chose when asked for 0

    // False (*err: EADDRINUSE, EACCES, the OS's errno) with a log line if it cannot listen.
    bool open(u16 port, bool local_only = true, i32* err = nullptr);
    void close();
    bool is_open() const { return fd.is_open(); }
    // A connected socket, or none (not is_open) when nobody is waiting. Non-blocking.
    KHandle accept();
};

struct NetSocket {
    KHandle   fd;       // KHandleKind::Socket
    Array<u8> out;        // queued for sending; flush() drains what the kernel takes
    u32       out_limit = 64 * 1024; // past this, writes are dropped and counted, never blocked on
    bool      closed = false;        // the peer went away (read saw end of stream, or a send failed)
    u64       bytes_in = 0, bytes_out = 0, dropped = 0;

    // Takes ownership of a socket (from accept) and makes it non-blocking.
    void adopt(KHandle fd);
    bool is_open() const { return fd.is_open() && !closed; }
    // Bytes read into buf, 0 when nothing has arrived, -1 when the peer closed.
    i32  read(void* buf, u32 cap);
    // Queues bytes; false if the queue would pass out_limit (then nothing is queued).
    bool write(const void* data, u32 len);
    bool write_str(const char* s);
    // Sends what the kernel will take now.
    void flush();
    void close();
};

// Non-blocking connect to 127.0.0.1:port. The socket is usable at once: writes queue,
// and flush() sends once the connection is up. For tools and tests.
bool net_connect_local(u16 port, NetSocket& out, i32* err = nullptr);

// Closes a socket nobody adopted (one accept returned that is being turned away).
void net_close(KHandle fd);

} // namespace kv
