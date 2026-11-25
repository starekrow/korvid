// Korvid platform: alerts (alert.h). One system in the Input phase looks at every entity
// with an Alertable, asks the OS without waiting whether its handle has something to read,
// and emits an Alerted for each that has. The sources it saw are what a waiting loop is
// woken by till it runs again: the same PlatformWatch any module states, stated by the
// platform on the entities' behalf.
#include "platform_parts.h"
#include "korvid/platform/alert.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "wake_watch.h"

namespace kv {

namespace {

// Asked at each idle pass: the sources the last tick saw, for the loop to be woken by.
// What came is for the tick to find (the system emits), so this says nothing came.
bool alerts_waker(Engine& e, void* user) {
    PlatformState& s = *(PlatformState*)user;
    PlatformWatch w(e);
    for (Handle h : s.alert_sources) w.add(h);
    platform_watch(e, &s.alert_sources, w);
    return false;
}

} // namespace

void platform_alerts(TickContext& ctx) {
    PlatformState& s = ctx.state_as<PlatformState>();
    Engine& e = *ctx.engine;
    s.alert_sources.len = 0;
    for (KTableRows k : ctx.view<Alertable>()) {
        Span<Alertable> rows = k.col<Alertable>();
        for (u32 i = 0; i < k.rows(); ++i) {
            if (!k.alive(i)) continue;
            Alertable& a = rows[i];
            if (a.source.is_null() || !os_handle_open(a.source)) continue; // nothing asked, or closed: stale
            s.alert_sources.push(a.source);
            if (!wake_watch_readable(a.source)) continue;
            ++a.alerts;
            ctx.emit(Alerted{k.handle_at(i), a.source});
        }
    }
    // The waker only once something is alertable: a game with none pays for none.
    if (s.alert_sources.len && !s.alert_waker) s.alert_waker = engine_add_watched_waker(e, alerts_waker, &s);
}

void platform_alerts_shutdown(Engine& e, PlatformState& s) {
    if (s.alert_waker) {
        engine_remove_waker(e, alerts_waker, &s);
        platform_unwatch(e, &s.alert_sources);
        s.alert_waker = false;
    }
    s.alert_sources.free();
}

} // namespace kv
