#include "korvid/platform/net.h"
#include "korvid/core/memory.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <errno.h>
#include <string.h>

namespace {

// Pumps both ends until `pred` holds or the budget runs out.
template <class F>
bool pump_until(kv::NetSocket& a, kv::NetSocket& b, F pred) {
    for (kv::u32 i = 0; i < 2000; ++i) {
        a.flush(); b.flush();
        if (pred()) return true;
        kv::time_sleep(0.001);
    }
    return pred();
}

} // namespace

void test_net(kv::TestCtx& c) {
    using namespace kv;
    c.begin("net");

    NetListener l;
    KV_CHECK(c, l.open(0) && l.is_open() && l.port != 0); // 0: the OS picks
    {   // A port already listened on: refused, with the reason (and no log line: it is the caller's to report).
        NetListener second;
        i32 err = 0;
        KV_CHECK(c, !second.open(l.port, NetScope::Loopback, &err) && err == EADDRINUSE && !second.is_open());
        // It keeps the reason; an accept on it anyway finds nobody, and a dev or debug
        // build warns, with the reason, rather than leave a loop waiting in silence.
        KV_CHECK(c, second.error == EADDRINUSE && !os_handle_open(second.accept()) && second.warned == (KV_DEV != 0));
    }
    KV_CHECK(c, !os_handle_open(l.accept()));                       // nobody yet, and no blocking

    NetSocket client;
    KV_CHECK(c, net_connect_local(l.port, &client));
    Handle fd;
    for (u32 i = 0; i < 2000 && !os_handle_open(fd); ++i) { fd = l.accept(); if (!os_handle_open(fd)) time_sleep(0.001); }
    KV_CHECK(c, os_handle_open(fd) && os_handle_kind(fd) == OsKind::Socket);
    NetSocket server;
    server.adopt(fd);

    // A line each way; reads never block, writes queue until a flush.
    char buf[64];
    KV_CHECK(c, server.read(buf, sizeof buf) == 0);
    KV_CHECK(c, client.write_str("ls materials\n"));
    u32 got = 0;
    KV_CHECK(c, pump_until(client, server, [&] { i32 n = server.read(buf + got, (u32)sizeof buf - got); if (n > 0) got += (u32)n; return got >= 13; }));
    KV_CHECK(c, got == 13 && memcmp(buf, "ls materials\n", 13) == 0);
    KV_CHECK(c, server.write_str("ok\n"));
    got = 0;
    KV_CHECK(c, pump_until(client, server, [&] { i32 n = client.read(buf + got, (u32)sizeof buf - got); if (n > 0) got += (u32)n; return got >= 3; }));
    KV_CHECK(c, got == 3 && memcmp(buf, "ok\n", 3) == 0);
    KV_CHECK(c, client.bytes_out == 13 && server.bytes_in == 13 && server.bytes_out == 3 && client.bytes_in == 3);

    // The connection's state: an accepted socket is up from the start, a connecting one
    // once the OS says so.
    KV_CHECK(c, server.link() == NetLink::Up && server.error == 0);
    KV_CHECK(c, client.link() == NetLink::Up && client.link_state == NetLink::Up && client.error == 0);

    // The queue has a limit; past it, a write is dropped and counted, never blocked on.
    // pending() and room() say where it stands before a write is tried.
    server.out_limit = 8;
    KV_CHECK(c, server.pending() == 0 && server.room() == 8);
    KV_CHECK(c, server.write_str("12345") && server.pending() == 5 && server.room() == 3);
    KV_CHECK(c, server.write_str("678") && server.room() == 0 && !server.write_str("9") && server.dropped == 1);
    server.flush();
    KV_CHECK(c, server.pending() == 0 && server.room() == 8); // loopback took it at once

    // Closing one end shows at the other as -1, with no error: the peer closed in order
    // (having read what was sent it: a close on unread bytes is a reset, and says so).
    got = 0;
    KV_CHECK(c, pump_until(client, server, [&] { i32 n = client.read(buf, sizeof buf); if (n > 0) got += (u32)n; return got >= 8; }));
    client.close();
    KV_CHECK(c, pump_until(client, server, [&] { return server.read(buf, sizeof buf) < 0; }));
    KV_CHECK(c, server.closed && !server.is_open() && server.error == 0);

    // A write to a connection that is over is refused for good, and a dev or debug build
    // says so once. close() gives the queue's memory back: nothing is left to free.
    KV_CHECK(c, !server.write_str("x") && server.warned == (KV_DEV != 0));
    server.close();
    KV_CHECK(c, server.out.data == nullptr && server.link_state == NetLink::None);
    u16 dead = l.port;
    l.close();
    KV_CHECK(c, !l.is_open());

    // Nobody listening: the connect is refused, and that is told apart from a server that
    // closed. link() says so; a caller that only reads sees -1 with the reason.
    NetSocket lost;
    i32 err = 0;
    KV_CHECK(c, net_connect_local(dead, &lost, &err) && err == 0); // a socket was made: the answer comes later
    NetLink state = NetLink::Connecting;
    for (u32 i = 0; i < 5000 && state == NetLink::Connecting; ++i) { state = lost.link(); if (state == NetLink::Connecting) time_sleep(0.001); }
    KV_CHECK(c, state == NetLink::Failed && lost.error == ECONNREFUSED && lost.closed && !lost.is_open());
    KV_CHECK(c, lost.read(buf, sizeof buf) == -1);
    lost.close();
    NetSocket lost2; // the same, found by reading alone
    KV_CHECK(c, net_connect_local(dead, &lost2));
    i32 r = 0;
    for (u32 i = 0; i < 5000 && r == 0; ++i) { r = lost2.read(buf, sizeof buf); if (r == 0) time_sleep(0.001); }
    KV_CHECK(c, r == -1 && lost2.error == ECONNREFUSED && lost2.link_state == NetLink::Failed);
    lost2.close();
}

namespace {

constexpr kv::u32 kNowBytes = 8u << 20; // past out_limit, and past what a kernel buffers on loopback

kv::u8 now_byte(kv::u32 i) { return (kv::u8)(i * 7u + (i >> 13)); }

// The other end of a write_now, on a thread of its own: everything read, checked as it comes.
struct NowReader {
    kv::NetSocket* sock  = nullptr;
    kv::u64        want  = kNowBytes;
    bool           check = true; // each byte against now_byte, for one write_now of kNowBytes
    kv::u64        got   = 0;
    bool           same  = true;
};

void now_read(void* arg) {
    NowReader& r = *(NowReader*)arg;
    kv::u8 buf[64 * 1024];
    for (kv::u32 idle = 0; r.got < r.want && idle < 10000;) {
        kv::i32 n = r.sock->read(buf, sizeof buf);
        if (n < 0) break;
        if (n == 0) { ++idle; kv::time_sleep(0.001); continue; }
        idle = 0;
        if (r.check)
            for (kv::i32 i = 0; i < n; ++i) if (buf[i] != now_byte((kv::u32)r.got + (kv::u32)i)) r.same = false;
        r.got += (kv::u32)n;
    }
}

} // namespace

void test_net_write_now(kv::TestCtx& c) {
    using namespace kv;
    c.begin("net write_now");

    NetListener l;
    KV_CHECK(c, l.open(0));
    NetSocket client;
    KV_CHECK(c, net_connect_local(l.port, &client) && client.link_state == NetLink::Connecting);
    // It waits out the connect: the kernel finishes it before anyone accepts.
    KV_CHECK(c, client.write_now("hi", 2) && client.link_state == NetLink::Up && client.pending() == 0);
    Handle fd;
    for (u32 i = 0; i < 2000 && !os_handle_open(fd); ++i) { fd = l.accept(); if (!os_handle_open(fd)) time_sleep(0.001); }
    NetSocket server;
    server.adopt(fd);
    char buf[8];
    i32 n = 0;
    for (u32 i = 0; i < 2000 && n == 0; ++i) { n = server.read(buf, sizeof buf); if (n == 0) time_sleep(0.001); }
    KV_CHECK(c, n == 2 && memcmp(buf, "hi", 2) == 0);

    // More than out_limit and the kernel take at once: it waits while the reader catches
    // up, and every byte arrives in order. Nothing dropped, nothing left queued.
    u8* big = (u8*)mem_alloc(kNowBytes);
    for (u32 i = 0; i < kNowBytes; ++i) big[i] = now_byte(i);
    NowReader reader;
    reader.sock = &server;
    OsThread t;
    KV_CHECK(c, os_thread_start(&t, now_read, &reader));
    u64 before = client.bytes_out;
    KV_CHECK(c, client.write_now(big, kNowBytes));
    os_thread_join(&t);
    KV_CHECK(c, reader.got == kNowBytes && reader.same);
    KV_CHECK(c, client.dropped == 0 && client.pending() == 0 && client.bytes_out - before == kNowBytes);

    // What was queued goes first.
    KV_CHECK(c, client.write_str("a") && client.write_now("b", 1));
    n = 0;
    for (u32 i = 0; i < 2000 && n < 2; ++i) { i32 m = server.read(buf + n, sizeof buf - (u32)n); if (m > 0) n += m; else time_sleep(0.001); }
    KV_CHECK(c, n == 2 && memcmp(buf, "ab", 2) == 0);

    // A deadline, with a peer that is not reading: false when the time runs out, the
    // connection still up, and what the kernel did not take queued. The kernel may take a
    // good deal first (Windows' loopback grows its buffer), so it is asked until it stops.
    {
        bool took = true;
        u64 given = 0;
        f64 late = 0.0;
        for (u32 i = 0; i < 32 && took; ++i) {
            f64 t0 = time_now();
            took = client.write_now(big, kNowBytes, 0.2);
            late = time_now() - t0;
            given += kNowBytes;
        }
        KV_CHECK(c, !took && client.is_open() && client.pending() > 0);
        KV_CHECK(c, late >= 0.19 && late < 1.0);
        // A reader comes: the queue goes on where it stopped, and every byte arrives.
        NowReader drain;
        drain.sock = &server;
        drain.want = given;
        drain.check = false;
        OsThread dt;
        KV_CHECK(c, os_thread_start(&dt, now_read, &drain));
        KV_CHECK(c, client.write_now(nullptr, 0) && client.pending() == 0);
        os_thread_join(&dt);
        KV_CHECK(c, drain.got == given);
    }

    // The peer gone: false, with the reason, rather than waiting for ever. True only says
    // the kernel took the bytes, and it may take a buffer's worth before the peer's reset
    // comes back, so a write or two can still succeed. Then the next call on the dead
    // connection is refused at once, and warned of in a dev or debug build.
    server.close();
    bool sent = true;
    for (u32 i = 0; i < 200 && sent; ++i) { sent = client.write_now(big, 64 * 1024); if (sent) time_sleep(0.005); }
    KV_CHECK(c, !sent && client.closed && client.error != 0 && !client.warned);
    KV_CHECK(c, !client.write_now("x", 1) && client.warned == (KV_DEV != 0));
    mem_free(big);

    // After close(), a read looks like nothing arrived yet, and is warned of.
    client.close();
    KV_CHECK(c, !client.warned && client.read(buf, sizeof buf) == 0 && client.warned == (KV_DEV != 0));
    l.close();
}

void test_net_blocking(kv::TestCtx& c) {
    using namespace kv;
    c.begin("net, waiting");

    // accept(seconds): nobody comes, and it gives up when told; somebody comes, and it
    // returns at once.
    NetListener l;
    KV_CHECK(c, l.open(0));
    f64 t0 = time_now();
    KV_CHECK(c, !os_handle_open(l.accept(0.1)));
    f64 took = time_now() - t0;
    KV_CHECK(c, took >= 0.09 && took < 1.0);

    // net_connect_local_now: Up when somebody listens.
    NetSocket client;
    i32 err = 0;
    KV_CHECK(c, net_connect_local_now(l.port, &client, 5.0, &err) && client.link_state == NetLink::Up && err == 0);
    t0 = time_now();
    Handle fd = l.accept(5.0);
    KV_CHECK(c, os_handle_open(fd) && time_now() - t0 < 1.0);
    NetSocket server;
    server.adopt(fd);

    // read(buf, cap, seconds): nothing sent, 0 when the time runs out; something sent, it
    // returns with it; the peer gone, -1 at once.
    char buf[16];
    t0 = time_now();
    KV_CHECK(c, server.read(buf, sizeof buf, 0.1) == 0);
    took = time_now() - t0;
    KV_CHECK(c, took >= 0.09 && took < 1.0);
    KV_CHECK(c, client.write_now("hello", 5));
    t0 = time_now();
    KV_CHECK(c, server.read(buf, sizeof buf, 5.0) == 5 && memcmp(buf, "hello", 5) == 0 && time_now() - t0 < 1.0);
    client.close();
    t0 = time_now();
    KV_CHECK(c, server.read(buf, sizeof buf, 5.0) == -1 && time_now() - t0 < 1.0);
    server.close();

    // Nobody listening: false, with the reason, and the socket closed. Windows takes about
    // 2 seconds to say so.
    u16 dead = l.port;
    l.close();
    NetSocket lost;
    err = 0;
    KV_CHECK(c, !net_connect_local_now(dead, &lost, 10.0, &err) && err == ECONNREFUSED && lost.error == ECONNREFUSED && !lost.is_open());
    // A deadline shorter than the answer: ETIMEDOUT, the socket closed all the same.
    NetSocket late;
    err = 0;
    bool up = net_connect_local_now(dead, &late, 0.0, &err);
    KV_CHECK(c, !up && (err == ETIMEDOUT || err == ECONNREFUSED) && !late.is_open());
}

void test_net_waiting_on_nothing(kv::TestCtx& c) {
    using namespace kv;
    c.begin("net, waiting on nothing");
    // A listener that never opened, a socket that was never connected: the waits still
    // last their time (and a dev or debug build warns), so a loop of them idles.
    NetListener none;
    f64 t0 = time_now();
    KV_CHECK(c, !os_handle_open(none.accept(0.05)) && time_now() - t0 >= 0.045 && none.warned == (KV_DEV != 0));
    NetSocket nobody;
    char buf[4];
    t0 = time_now();
    KV_CHECK(c, nobody.read(buf, sizeof buf, 0.05) == 0 && time_now() - t0 >= 0.045 && nobody.warned == (KV_DEV != 0));
}
