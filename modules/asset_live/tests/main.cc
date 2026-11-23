// asset_live against a stand-in for goldberg: a listener in the test, named by KV_GOLDBERG. The
// program connects and says hello; `changed PATH` reloads that asset, heard through the waker the
// on-demand loop asks (engine_wake) as well as the Input phase's pump; the stand-in going away is
// noticed, and a new one is found again. KV_GOLDBERG=off connects to nothing. A ship build
// connects to nothing whatever KV_GOLDBERG says: asset_live is off there.
#include "korvid/asset/blob.h"
#include "korvid/asset/module.h"
#include "korvid/asset_live/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "korvid/core/name.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include "korvid/platform/io.h"
#include "korvid/platform/module.h"
#include "korvid/platform/net.h"
#include <stdlib.h>
#include <string.h>

namespace {

using namespace kv;

void frame(Engine& e) { platform_pump(e); engine_frame(e, 1.0 / 60.0); }

template <class F> bool until(Engine& e, F done, f64 seconds = 5.0) {
    f64 t0 = time_now();
    while (time_now() - t0 < seconds) {
        frame(e);
        if (done()) return true;
        time_sleep(0.002);
    }
    return false;
}

// The stand-in: one connection, what it said, lines to send.
struct Goldberg {
    NetListener l;
    NetSocket   sock;
    Array<char> heard{};
    bool open(u16 port = 0) { return l.open(port, true); }
    bool take() {
        if (sock.fd < 0) { NetFd fd = l.accept(); if (fd < 0) return false; sock.adopt(fd); }
        char buf[512];
        for (i32 n; (n = sock.read(buf, sizeof buf)) > 0;) { if (heard.len) --heard.len; memcpy(heard.push_n((u32)n), buf, (usize)n); heard.push(0); }
        sock.flush();
        return true;
    }
    void say(const char* line) { sock.write_str(line); sock.flush(); }
    void close() { sock.close(); sock.out.free(); sock = NetSocket{}; l.close(); heard.len = 0; }
    void free() { close(); heard.free(); }
};

} // namespace

int main() {
    TestCtx c;
    c.begin("asset_live");
    char dir[400], path[600];
    KV_CHECK(c, io_make_temp_dir("kv-asset-live", dir, sizeof dir));
    format(path, sizeof path, "%s/doc.txt", dir);
    KV_CHECK(c, io_write_now(path, "one", 3));

    Goldberg g;
    KV_CHECK(c, g.open());
    char port[16];
    format(port, sizeof port, "%u", (u32)g.l.port);
    setenv("KV_GOLDBERG", port, 1);
    {
        const ModuleDesc* const list[] = {&platform_module, &asset_module, &asset_live_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        KV_CHECK(c, platform_io(e)->mount_dir(dir));
        Handle doc = asset_load_blob(e, "doc.txt");
        KV_CHECK(c, until(e, [&] { const BlobAsset* b = asset_blob(e, doc); return b && b->state == AssetState::Ready; }));

#if KV_DEV
        // It connects and says who it is; told welcome, it counts itself connected.
        KV_CHECK(c, until(e, [&] { return g.take() && g.heard.len && strstr(g.heard.data, "hello "); }));
        g.say("welcome\n");
        KV_CHECK(c, until(e, [&] { return asset_live_stats(e).connected; }));
        KV_CHECK(c, asset_live_stats(e).port == g.l.port && asset_live_stats(e).connects == 1);

        // `changed`, heard through the waker alone: the reload asked for with no tick between.
        KV_CHECK(c, io_write_now(path, "two!", 4));
        g.say("changed doc.txt\n");
        bool woke = false;
        for (u32 i = 0; i < 2000 && !woke; ++i) { woke = engine_wake(e); time_sleep(0.001); }
        KV_CHECK(c, woke && asset_live_stats(e).changed == 1 && asset_live_stats(e).reloaded == 1);
        KV_CHECK(c, until(e, [&] { const BlobAsset* b = asset_blob(e, doc); return b->generation == 1 && b->size == 4; }));
        // A path nothing loaded: heard, nothing asked.
        g.say("changed nothing/here.png\nremoved doc.txt\n");
        KV_CHECK(c, until(e, [&] { return asset_live_stats(e).changed == 2 && asset_live_stats(e).removed == 1; }));
        KV_CHECK(c, asset_live_stats(e).reloaded == 1);
        KV_CHECK(c, asset_blob(e, doc)->state == AssetState::Ready); // what was loaded from a removed file stays

        // The stand-in goes, and comes back on the same port: noticed, and found again.
        u16 was = g.l.port;
        g.close();
        KV_CHECK(c, until(e, [&] { return !asset_live_stats(e).connected; }));
        KV_CHECK(c, g.open(was));
        KV_CHECK(c, until(e, [&] { return g.take() && g.heard.len && strstr(g.heard.data, "hello "); }, 8.0));
        g.say("welcome\n");
        KV_CHECK(c, until(e, [&] { return asset_live_stats(e).connected && asset_live_stats(e).connects == 2; }));
#else
        // Ship: a listener named by KV_GOLDBERG is never called, and the asset loaded as ever.
        for (u32 i = 0; i < 50; ++i) { frame(e); time_sleep(0.002); }
        KV_CHECK(c, g.l.accept() < 0 && !asset_live_stats(e).connected && asset_live_stats(e).connects == 0);
#endif
        engine_shutdown(e);
    }

    // Off: no connection, however long it runs.
    setenv("KV_GOLDBERG", "off", 1);
    {
        const ModuleDesc* const list[] = {&platform_module, &asset_module, &asset_live_module};
        Engine e;
        KV_CHECK(c, engine_init(e, list));
        for (u32 i = 0; i < 50; ++i) { frame(e); time_sleep(0.002); }
        KV_CHECK(c, g.l.accept() < 0 && !asset_live_stats(e).connected && asset_live_stats(e).connects == 0);
        engine_shutdown(e);
    }
    unsetenv("KV_GOLDBERG");
    g.free();
    io_remove_tree(dir);
    int rc = test_report(c, "asset_live_tests");
    names_shutdown();
    mem_shutdown();
    return rc;
}
