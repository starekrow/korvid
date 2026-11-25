// Korvid platform: non-blocking TCP. A listener accepts; a socket reads what has arrived
// and queues what is to be sent, draining it when the pump flushes. Nothing here blocks
// and nothing spawns a thread: whoever owns a socket polls it once per frame. The CLI's
// transport, and a game's if it wants one. IPv4 loopback by default; a listener may open
// to every interface, and should think before it does.
#pragma once
#include "korvid/core/array.h"
#include "korvid/platform/os_handle.h"

namespace kv {

// Who may connect to a listener: this machine only, or anyone who can reach it.
enum class NetScope : u8 { Loopback, Any };

struct NetListener {
    Handle fd;      // OsKind::Socket
    u16   port = 0; // the bound port; what the OS chose when asked for 0

    // False (*err: EADDRINUSE, EACCES, the OS's errno) if it cannot listen. Nothing is
    // logged: the failure is the caller's to report, with err (os_error_text).
    bool open(u16 port, NetScope scope = NetScope::Loopback, i32* err = nullptr);
    void close();
    bool is_open() const { return os_handle_open(fd); }
    // A connected socket, or none (not is_open) when nobody is waiting. Non-blocking.
    Handle accept();
};

// Where a socket's connection stands. One that was accepted is Up from the start; one
// that net_connect_local made is Connecting until the OS says which way it went.
enum class NetLink : u8 { None, Connecting, Up, Failed };

struct NetSocket {
    Handle   fd;       // OsKind::Socket
    Array<u8> out;        // queued for sending; flush() drains what the kernel takes. Set out.alloc before the first write to say where it lives
    u32       out_limit = 64 * 1024; // past this, writes are dropped and counted, never blocked on
    bool      closed = false;        // over: the peer went away (read saw the end, or a send failed), or the connect failed
    NetLink   link_state = NetLink::None; // what link() last found
    i32       error = 0;             // why it is closed, as an errno: ECONNREFUSED, ECONNRESET; 0 for a peer that closed in order
    u64       bytes_in = 0, bytes_out = 0, dropped = 0; // read; taken by the kernel at flush(); refused by write past out_limit

    // Takes ownership of a socket (from accept) and makes it non-blocking.
    void adopt(Handle fd);
    bool is_open() const { return os_handle_open(fd) && !closed; }
    // Where the connection stands. Free for a socket that is Up or over; while Connecting it
    // asks the OS, without waiting, and remembers the answer. Failed is also `closed`, with
    // `error` saying why (ECONNREFUSED: nobody is listening there). read() asks this
    // itself, so a caller that only reads sees a refused connection as -1 with the error
    // set, never as an empty stream.
    NetLink link();
    // Bytes read into buf, 0 when nothing has arrived, -1 when it is over: `error` 0 if the
    // peer closed in order, else why.
    i32  read(void* buf, u32 cap);
    // Queues bytes; false if the queue would pass out_limit (then nothing is queued).
    bool write(const void* data, u32 size);
    bool write_str(const char* s);
    // Backpressure, for a caller that would rather hold its data than have write drop it:
    // what is queued and not yet sent, and how much more write will take. Two reads of
    // fields: nothing is asked of the OS.
    u32  pending() const { return out.len; }
    u32  room() const { return out.len < out_limit ? out_limit - out.len : 0; }
    // Sends what the kernel will take now.
    void flush();
    // Closes the socket and gives the queue's memory back: nothing else to free. What was
    // still pending() is not sent: flush until it is 0 first, to send everything.
    void close();
};

// Non-blocking connect to 127.0.0.1:port. The socket is usable at once: writes queue,
// and flush() sends once the connection is up; link() says when it is, or that it was
// refused. False (*err) only when no socket could be made at all. For tools and tests.
// A refusal comes at once on POSIX; on Windows the OS retries the connect first, and a port
// nobody listens on is Failed (ECONNREFUSED) about 2 seconds later.
// Nothing here logs: a failure is the caller's to report, with err.
bool net_connect_local(u16 port, NetSocket* out, i32* err = nullptr);

// Closes a socket nobody adopted (one accept returned that is being turned away).
void net_close(Handle fd);

} // namespace kv
