#include "korvid/platform/net.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
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
    KV_CHECK(c, l.accept() == -1);                         // nobody yet, and no blocking

    NetSocket client;
    KV_CHECK(c, net_connect_local(l.port, client));
    NetFd fd = -1;
    for (u32 i = 0; i < 2000 && fd < 0; ++i) { fd = l.accept(); if (fd < 0) time_sleep(0.001); }
    KV_CHECK(c, fd >= 0);
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

    // The queue has a limit; past it, a write is dropped and counted, never blocked on.
    server.out_limit = 8;
    KV_CHECK(c, server.write_str("12345678") && !server.write_str("9") && server.dropped == 1);
    server.flush();

    // Closing one end shows at the other as -1.
    client.close();
    KV_CHECK(c, pump_until(client, server, [&] { return server.read(buf, sizeof buf) < 0; }));
    KV_CHECK(c, server.closed && !server.is_open());

    server.close();
    l.close();
    KV_CHECK(c, !l.is_open());
    server.out.free();
    client.out.free();
}
