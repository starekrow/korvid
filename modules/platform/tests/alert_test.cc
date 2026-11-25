// Alerts (alert.h): an entity with an Alertable is told when its handle has something to
// read, a waiting loop is woken for it, and a closed handle asks for nothing. And /dev in
// the CLI's tree, where the open handles are listed.
#include "korvid/platform/alert.h"
#include "korvid/platform/module.h"
#include "korvid/platform/net.h"
#include "korvid/core/cli.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <string.h>

namespace {

using namespace kv;

struct Seen { u32 alerts = 0; Handle entity, source; };
Seen g_seen;

void on_alerted(TickContext&, Span<const Alerted> alerts) {
    for (const Alerted& a : alerts) { ++g_seen.alerts; g_seen.entity = a.entity; g_seen.source = a.source; }
}

constinit const KTableDesc k_ktables[] = {{.name = "at.peer", .components = "Alertable", .capacity = 8}};
constinit const HandlerDesc k_handlers[] = {{.name = "at.heard", KV_ON(Alerted, on_alerted), .phase = Phase::Input}};
KV_MODULE_DEFINE(k_at) = {.name = "at", .ktables = k_ktables, .handlers = k_handlers};

struct Lines { char text[4096]; usize len = 0; };
void take_line(void* user, const char* line) {
    Lines& l = *(Lines*)user;
    l.len += (usize)format(l.text + l.len, sizeof l.text - l.len, "%s\n", line);
}
bool list_line(void* user, const char* name, const char* summary) {
    Lines& l = *(Lines*)user;
    l.len += (usize)format(l.text + l.len, sizeof l.text - l.len, "%s  %s\n", name, summary);
    return true;
}

} // namespace

void test_alert(kv::TestCtx& c) {
    using namespace kv;
    c.begin("platform alerts");
    const ModuleDesc* const list[] = {&platform_module, &k_at};
    Engine e;
    KV_CHECK(c, engine_init(e, list));
    const f64 dt = e.clock.tick_dt;
    auto frame = [&] { platform_pump(e); engine_frame(e, dt); };

    NetListener l;
    NetSocket client, server;
    KV_CHECK(c, l.open(0) && net_connect_local(l.port, &client));
    for (u32 i = 0; i < 400 && server.fd.is_null(); ++i) {
        Handle fd = l.accept();
        if (!fd.is_null()) server.adopt(fd); else time_sleep(0.005);
    }
    KV_CHECK(c, !server.fd.is_null());

    KTable* peers = engine_ktable(e, "at.peer");
    Handle peer = peers->spawn(Alertable{server.fd});
    Handle idle = peers->spawn(Alertable{}); // asks for nothing
    KV_CHECK(c, !peer.is_null() && !idle.is_null());

    // Nothing to read: no alert, however many ticks.
    g_seen = Seen{};
    for (u32 i = 0; i < 5; ++i) frame();
    KV_CHECK(c, g_seen.alerts == 0 && peers->get<Alertable>(peer)->alerts == 0);

    // A loop asleep is woken when something arrives: the platform watches the entity's
    // handle for it, with no waker of the game's.
    KV_CHECK(c, !engine_wake(e)); // the waker states what the last tick saw
    client.write_str("hello");
    client.flush();
    f64 t0 = time_now();
    KV_CHECK(c, platform_wait(e, 5.0) == PlatformWoken::Wake && time_now() - t0 < 2.0);

    // The tick after: the entity is alerted, by name, with the handle that stirred.
    frame();
    KV_CHECK(c, g_seen.alerts == 1 && g_seen.entity == peer && g_seen.source == server.fd);
    // A level, not an edge: unread, it is alerted again; read, no more.
    frame();
    KV_CHECK(c, g_seen.alerts == 2 && peers->get<Alertable>(peer)->alerts == 2);
    char buf[16];
    KV_CHECK(c, server.read(buf, sizeof buf) == 5);
    frame();
    frame();
    KV_CHECK(c, g_seen.alerts == 2);

    // /dev/handles in the CLI's tree: the kinds, the open handles, one described. Asked of
    // the mount itself, as the cli module's `ls` and `print` do (it is not linked here).
    {
        const CliMountDesc* dev = nullptr;
        for (const CliMountDesc& m : platform_module.mounts) if (strcmp(m.path, "/dev") == 0) dev = &m;
        KV_CHECK(c, dev && dev->list && dev->describe);
        Lines out;
        CliContext ctx;
        ctx.engine = &e;
        ctx.sink = take_line;
        ctx.user = &out;
        dev->list(ctx, "", list_line, &out);
        KV_CHECK(c, strstr(out.text, "handles/"));
        out = Lines{};
        dev->list(ctx, "handles/", list_line, &out);
        KV_CHECK(c, strstr(out.text, "socket/"));
        char name[96];
        out = Lines{};
        dev->list(ctx, "handles/socket/", list_line, &out);
        format(name, sizeof name, "%u  ", server.fd.index());
        KV_CHECK(c, strstr(out.text, name));
#if KV_DEV || KV_DEBUG
        KV_CHECK(c, strstr(out.text, "listener 127.0.0.1:") && strstr(out.text, "accepted on :") && strstr(out.text, "5 read"));
#endif
        out = Lines{};
        format(name, sizeof name, "handles/socket/%u", server.fd.index());
        KV_CHECK(c, dev->describe(ctx, name) && strstr(out.text, "os.socket#"));
#if KV_DEV || KV_DEBUG
        // Opened outside any module's code (this test's): the program's main module, the
        // last of the list, is who has it.
        KV_CHECK(c, strstr(out.text, "opened by at") && strstr(out.text, "by       at"));
#endif
        out = Lines{};
        KV_CHECK(c, dev->describe(ctx, "handles") && strstr(out.text, "OS handles open"));
        KV_CHECK(c, dev->describe(ctx, "handles/socket") && !dev->describe(ctx, "handles/socket/99999") && !dev->describe(ctx, "handles/nope"));
    }

    // Closed: the handle is stale, so the entity asks for nothing, with nothing told to
    // anyone. What arrives for the dead socket alerts nobody and wakes nobody.
    client.write_str("more");
    client.flush();
    server.close();
    g_seen = Seen{};
    for (u32 i = 0; i < 3; ++i) frame();
    KV_CHECK(c, g_seen.alerts == 0 && !os_handle_open(peers->get<Alertable>(peer)->source));
    engine_wake(e);
    KV_CHECK(c, platform_wait(e, 0.1) == PlatformWoken::Time);

    // A listener is alertable too: a connection waiting is something to read.
    peers->get<Alertable>(idle)->source = l.fd;
    for (u32 i = 0; i < 3; ++i) frame();
    KV_CHECK(c, g_seen.alerts == 0);
    NetSocket second;
    KV_CHECK(c, net_connect_local(l.port, &second));
    for (u32 i = 0; i < 400 && g_seen.alerts == 0; ++i) { frame(); if (!g_seen.alerts) time_sleep(0.005); }
    KV_CHECK(c, g_seen.alerts >= 1 && g_seen.entity == idle && g_seen.source == l.fd);

    second.close();
    client.close();
    l.close();
    engine_shutdown(e);
}
