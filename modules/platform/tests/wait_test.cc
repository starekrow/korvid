// platform_wait and platform_wake with no window (docs/PRDs/Engine-Slice-12-PRD.md, KR26):
// a wake before the wait ends it at once, a wait with nothing to end it lasts its time,
// and a wake from another thread ends it about when it is sent. Each counted.
#include "korvid/platform/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/os_thread.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"

namespace {

using namespace kv;

struct Waker { Engine* e; f64 after; f64 sent; };

void wake_later(void* arg) {
    Waker& w = *(Waker*)arg;
    time_sleep(w.after);
    w.sent = time_now();
    platform_wake(*w.e);
}

} // namespace

void test_wait(kv::TestCtx& c) {
    using namespace kv;
    c.begin("platform wait");
    const ModuleDesc* const list[] = {&platform_module};
    Engine e;
    KV_CHECK(c, engine_init(e, list));

    // A wake that came first: the wait ends without waiting, and takes it.
    platform_wake(e);
    f64 t0 = time_now();
    KV_CHECK(c, platform_wait(e, 1.0) == PlatformWoken::Wake && time_now() - t0 < 0.1);
    // Taken: the next wait lasts its time.
    t0 = time_now();
    KV_CHECK(c, platform_wait(e, 0.05) == PlatformWoken::Time);
    f64 took = time_now() - t0;
    KV_CHECK(c, took >= 0.049 && took < 0.5);
    // No time: no wait, and a wake stays for the next.
    platform_wake(e);
    KV_CHECK(c, platform_wait(e, 0.0) == PlatformWoken::Time);
    KV_CHECK(c, platform_wait(e, 1.0) == PlatformWoken::Wake);

    // From another thread, mid-wait.
    Waker w{&e, 0.05, 0.0};
    OsThread th;
    KV_CHECK(c, os_thread_start(&th, wake_later, &w));
    t0 = time_now();
    PlatformWoken why = platform_wait(e, 5.0);
    f64 woke = time_now();
    os_thread_join(&th);
    KV_CHECK(c, why == PlatformWoken::Wake && woke - t0 < 2.0 && woke >= w.sent);
    KV_INFO("test", "a wake from another thread ended the wait %.3f ms after it was sent", (woke - w.sent) * 1e3);

    PlatformWaits n = platform_waits(e);
    KV_CHECK(c, n.waits == 4 && n.woken == 3 && n.timed == 1 && n.window == 0);
    engine_shutdown(e);
}
