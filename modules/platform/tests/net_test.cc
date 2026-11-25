#include "korvid/platform/net.h"
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
    {   // A port already listened on: refused, with the reason (and a log line).
        NetListener second;
        i32 err = 0;
        KV_CHECK(c, !second.open(l.port, NetScope::Loopback, &err) && err == EADDRINUSE && !second.is_open());
    }
    KV_CHECK(c, !l.accept().is_open());                       // nobody yet, and no blocking

    NetSocket client;
    KV_CHECK(c, net_connect_local(l.port, &client));
    KHandle fd;
    for (u32 i = 0; i < 2000 && !fd.is_open(); ++i) { fd = l.accept(); if (!fd.is_open()) time_sleep(0.001); }
    KV_CHECK(c, fd.is_open() && fd.kind() == KHandleKind::Socket);
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

    // close() gives the queue's memory back: nothing is left for the owner to free.
    server.write_str("x");
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
