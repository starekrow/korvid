// Korvid platform: TCP. A listener accepts; a socket reads what has arrived and queues what
// is to be sent, draining it when the pump flushes. None of that blocks, and nothing spawns
// a thread: whoever owns a socket polls it once per frame. For straight-line code (a
// tool_task) each has a twin that waits, given a time: accept(seconds), read(buf, cap,
// seconds), write_now, net_connect_local_now. The CLI's transport, and a game's if it wants
// one. IPv4 loopback by default; a listener may open to every interface, and should think
// before it does.
//
// A socket or listener used after it is over or closed (a write to a connection gone, a
// read after close(), an accept on a listener whose open failed) says false, 0 or -1 as
// below, and a dev or debug build also logs a warning, once per socket, naming what
// happened: code that does not check gets told rather than spinning in silence.
#pragma once
#include "korvid/core/array.h"
#include "korvid/platform/os_handle.h"

namespace kv {

// Who may connect to a listener: this machine only, or anyone who can reach it.
enum class NetScope : u8 { Loopback, Any };

struct NetListener {
    Handle fd;      // OsKind::Socket
    u16   port = 0; // the bound port; what the OS chose when asked for 0
    i32   error = 0; // why the last open failed, as an errno (EADDRINUSE); 0 once one succeeds
    bool  warned = false; // a dev or debug build has warned of its use while not open

    // False (*err and `error`: EADDRINUSE, EACCES, the OS's errno) if it cannot listen.
    // Nothing is logged: the failure is the caller's to report (os_error_text). A port in
    // use is EADDRINUSE on every OS (Windows' exclusive-use bind included).
    bool open(u16 port, NetScope scope = NetScope::Loopback, i32* err = nullptr);
    void close();
    bool is_open() const { return os_handle_open(fd); }
    // A connected socket, or none (not is_open) when nobody is waiting. Non-blocking. On a
    // listener that is not open, none, and a dev or debug build's warning saying why (its
    // open's error): a loop waiting on it would wait for ever.
    Handle accept();
    // The same, waiting up to `seconds` for one (less than 0: as long as it takes): for
    // straight-line code, which waits here instead of looping on accept() with a sleep. The
    // null handle when the time ran out. On a listener that is not open, the null handle
    // too, after the time (a dev or debug build warns): a loop of these waits, not spins.
    Handle accept(f64 seconds);
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
    u64       bytes_in = 0, bytes_out = 0, dropped = 0; // read; taken by the kernel (flush, write_now); refused by write past out_limit
    bool      warned = false;        // a dev or debug build has warned of its use after it was over or closed

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
    // peer closed in order, else why. After close() (or on a socket never adopted) it is 0,
    // as if nothing had arrived yet, and a dev or debug build warns.
    i32  read(void* buf, u32 cap);
    // The same, waiting up to `seconds` (less than 0: as long as it takes) for bytes or the
    // end: 0 then means the time ran out with nothing arrived. A connect still under way is
    // waited out first. For straight-line code, which waits here instead of looping on
    // read() with a sleep. On a socket not open (after close()), 0 after the time, as above.
    i32  read(void* buf, u32 cap, f64 seconds);
    // Queues bytes; false if the queue would pass out_limit (then nothing is queued), and
    // false for good once the socket is over or closed: a loop that retries until write
    // takes its bytes must also stop when is_open() turns false, or it never ends (a dev or
    // debug build warns at the first such write). write_now is that loop, done right.
    bool write(const void* data, u32 size);
    bool write_str(const char* s);
    // Sends `data` now, after whatever is still queued, waiting while the kernel will take
    // no more: true once the kernel has all of it, false if the connection is over, with
    // `error` saying why (or was over before the call: then a dev or debug build warns).
    // Nothing is dropped and out_limit does not apply; nothing is left for flush(). It
    // blocks the calling thread while the peer is not reading, and waits out a connect
    // still under way. For a tool that does one thing at a time; code that must not block
    // uses write, room() and flush.
    //
    // With `seconds` 0 or more it waits no longer than that, and false may also mean the
    // time ran out: then is_open() is still true, and what the kernel had not taken is
    // queued in `out` (past out_limit if need be), so a later write_now or flush goes on
    // where this one stopped. 0 sends what the kernel takes now and queues the rest. So:
    //     if (!s.write_now(buf, n, 5.0)) { if (s.is_open()) /* a peer not reading */; else /* gone: s.error */; }
    bool write_now(const void* data, u32 size, f64 seconds = -1.0);
    // Backpressure, for a caller that would rather hold its data than have write drop it:
    // what is queued and not yet sent, and how much more write will take. Two reads of
    // fields: nothing is asked of the OS.
    u32  pending() const { return out.len; }
    u32  room() const { return out.len < out_limit ? out_limit - out.len : 0; }
    // Sends what the kernel will take now, without waiting: what it will not take stays
    // queued for the next flush.
    void flush();
    // Closes the socket and gives the queue's memory back: nothing else to free. What was
    // still pending() is not sent: flush until it is 0 first, to send everything.
    void close();

    // A Unix-domain socket's (KR49, below): read() that also takes the descriptors sent
    // with the bytes, each made a handle of this process's (a socket, a pipe, or anything
    // else as a file), close-on-exec. *got says how many went into `handles`; any past
    // `handle_cap` are closed and counted in handles_dropped. The caller owns those it got.
    i32  read_with(void* buf, u32 cap, Handle* handles, u32 handle_cap, u32* got);
    // write_now, with descriptors sent alongside the first byte of `data` (size at least 1;
    // at most kNetMaxHandles of them, each an open handle: the peer gets its own copy, and
    // the caller keeps its). What was queued goes first. False as write_now, or with
    // `error` EINVAL for a handle that is not open, too many, or no bytes to carry them.
    bool write_now_with(const void* data, u32 size, const Handle* handles, u32 count, f64 seconds = -1.0);
    u64  handles_dropped = 0; // received past what read_with was given room for, and closed
};

// Non-blocking connect to 127.0.0.1:port. The socket is usable at once: writes queue,
// and flush() sends once the connection is up; link() says when it is, or that it was
// refused. False (*err) only when no socket could be made at all. For tools and tests.
// A refusal comes at once on POSIX; on Windows the OS retries the connect first, and a port
// nobody listens on is Failed (ECONNREFUSED) about 2 seconds later.
// Nothing here logs: a failure is the caller's to report, with err.
bool net_connect_local(u16 port, NetSocket* out, i32* err = nullptr);
// The same, waiting up to `seconds` (less than 0: as long as it takes) for the connect to
// go one way or the other: true once it is Up; false, with *err and the socket's `error`
// saying why (ECONNREFUSED: nobody listens there; ETIMEDOUT: the time ran out), and the
// socket closed. A refusal is an answer here, where a read would see it only as an end.
// On Windows a refusal takes about 2 seconds to come (above). For straight-line code.
bool net_connect_local_now(u16 port, NetSocket* out, f64 seconds, i32* err = nullptr);

// Closes a socket nobody adopted (one accept returned that is being turned away).
void net_close(Handle fd);

// ---- Unix-domain sockets, for a thread of its own (KR49) ------------------------------
// A stream socket to a name on this machine, for code that speaks a local protocol (D-Bus,
// a desktop portal) from a thread of its own rather than once per frame: its wait is exact
// and ends when another thread wakes it, and descriptors go across with the bytes. The
// socket is a NetSocket like any other: read, write, flush, write_now and close are the
// same, and so is the frame loop's watch of it.
//
//   NetSocket s; i32 err = 0;
//   if (!net_connect_unix_now("/run/user/1000/bus", NetUnixName::Path, &s, 2.0, &err)) ... os_error_text(err)
//   NetWaker wake; wake.open();
//   for (;;) {                                               // the socket's own thread
//       u8 r = net_wait(s, s.pending() != 0, &wake, -1.0);  // asleep until one of them
//       if (r & kNetWoken) ... another thread called wake.wake()
//       if (r & kNetReadable) { Handle got[kNetMaxHandles]; u32 n = 0; i32 k = s.read_with(buf, cap, got, kNetMaxHandles, &n); ... }
//       if (r & kNetWritable) s.flush();
//   }
//
// POSIX (Linux and macOS); the abstract namespace is Linux's. On Windows each call fails
// with EAFNOSUPPORT, a waker does not open, and net_wait returns 0 at once: Windows' AF_UNIX
// takes paths but passes no descriptors, and it is taken up when something there needs it.

// What a Unix-domain socket's name is: a path in the file system, or (Linux) a name in the
// abstract namespace, given without its leading NUL.
enum class NetUnixName : u8 { Path, Abstract };

// Connects to a listening Unix-domain socket, waiting up to `seconds` (less than 0: as
// long as it takes). False, with *err: ENOENT (no such socket), ECONNREFUSED (nobody
// listens there), ENAMETOOLONG (longer than the OS takes: 107 bytes on Linux, 103 on
// macOS), EAFNOSUPPORT (Abstract off Linux), ETIMEDOUT.
bool net_connect_unix_now(const char* name, NetUnixName kind, NetSocket* out, f64 seconds, i32* err = nullptr);

// Two Unix-domain stream sockets connected to each other (socketpair): a peer in the same
// process, for tests and for a thread that plays the other end.
bool net_socket_pair(NetSocket* a, NetSocket* b, i32* err = nullptr);

// The most descriptors one read_with takes or one write_now_with sends.
constexpr u32 kNetMaxHandles = 16;

// Wakes a net_wait from another thread. Opened and closed by hand; not moved while open.
struct NetWaker {
    i64 read_end  = -1; // the OS's: an eventfd on Linux (both ends the same), a pipe on macOS
    i64 write_end = -1;
    bool open(i32* err = nullptr);
    void close();
    bool is_open() const { return read_end >= 0; }
    // Any thread, a signal handler too. Wakes the wait under way or the next to begin;
    // wakes before a wait are one wake.
    void wake();
};

// What ended a net_wait: any of them, or 0 when the time ran out.
constexpr u8 kNetReadable = 1; // bytes to read, the end, or an error for the next read to find
constexpr u8 kNetWritable = 2; // only when asked for: room for more
constexpr u8 kNetWoken    = 4; // the waker was woken (and is clear again now)

// Waits until the socket can be read, can be written (when `want_write`), the waker is
// woken (null: none), or `seconds` pass (less than 0: as long as it takes). Exact: no
// slices, so a thread asleep here costs nothing until one of them happens. One thread
// waits on a socket at a time. A socket not open: 0 after the time, as read(seconds).
u8 net_wait(NetSocket& s, bool want_write, NetWaker* waker, f64 seconds);

} // namespace kv
