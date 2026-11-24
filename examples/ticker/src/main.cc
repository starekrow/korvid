// ticker: the measure of Pacing::Decoupled (KR20). A module that, with --want-ticks, asks
// for every tick (engine_want_ticks) as a music player does while it plays, and changes
// nothing on screen. Run it for ten seconds:
//
//   ticker --pacing decoupled --want-ticks --run-seconds 10
//
// and the frame loop's last line says it ran about 600 ticks and drew a frame or two (the
// window's first). Without --want-ticks it ticks on the heartbeat only. Under --pacing
// demand, asking for ticks is asking for frames: a frame every tick.
//
// --minimize-at N and --restore-at M minimize the window at sim tick N and restore it at
// M (KR21): the module logs each WindowVisibility it is handed and the tick, and the loop
// draws nothing between. Wayland restores nothing a program asks to; run it under X11
// (SDL_VIDEO_DRIVER=x11) to see both, or restore it by hand.
//
// A still window waits for the OS between passes (KR26): at the end it says how many waits
// there were and what ended them, about 40 in ten seconds when it asks for nothing. Each
// key pressed is logged with how long it waited, from the OS's stamp to the pump, and each
// resize with when it was seen. --frame-every MS asks for a frame at a time, again and
// again (a caret's blink), and says how late they came.
#include "korvid/ticker/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/core/time.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include "korvid/platform/module.h"
#include <stdlib.h>

namespace kv {

namespace {

struct Ticker {
    u64 ticks = 0; bool want = false; u64 minimize_at = 0, restore_at = 0;
    // --frame-every MS: a frame asked for at a time (engine_want_frame_at), and how late each came.
    f64 every = 0.0, asked = 0.0, late_sum = 0.0, late_max = 0.0;
    u64 asked_n = 0, drawn_n = 0;
};

// The Frame cycle: a frame drawn at or after the time asked for is that frame; the next
// is asked for from now.
void ticker_frame(TickContext& ctx) {
    Ticker& t = ctx.state_as<Ticker>();
    if (t.every <= 0.0) return;
    f64 now = time_now();
    if (now < t.asked) return; // drawn for something else, before the time
    f64 late = now - t.asked;
    ++t.drawn_n;
    t.late_sum += late;
    if (late > t.late_max) t.late_max = late;
    t.asked = now + t.every;
    ++t.asked_n;
    engine_want_frame_at(*ctx.engine, t.asked);
}

void ticker_tick(TickContext& ctx) {
    Ticker& t = ctx.state_as<Ticker>();
    ++t.ticks;
    if (t.want) engine_want_ticks(*ctx.engine);
    if (HostWindow* w = platform_window(*ctx.engine)) {
        if (t.minimize_at && ctx.tick == t.minimize_at) { KV_INFO("ticker", "tick %llu: minimizing", (unsigned long long)ctx.tick); w->minimize(true); }
        if (t.restore_at && ctx.tick == t.restore_at) { KV_INFO("ticker", "tick %llu: restoring", (unsigned long long)ctx.tick); w->minimize(false); }
    }
    for (const WindowVisibility& v : ctx.received<WindowVisibility>())
        KV_INFO("ticker", "tick %llu: %s", (unsigned long long)ctx.tick,
                v.visible ? "shown" : v.why == WindowHidden::Minimized ? "hidden, minimized" : "hidden, covered or away");
    for (const KeyDown& k : ctx.received<KeyDown>()) {
        if (k.repeat) continue;
        HostWindow* w = platform_window(*ctx.engine);
        KV_INFO("ticker", "tick %llu: key %u, %.2f ms from the OS's stamp to the pump", (unsigned long long)ctx.tick, (unsigned)k.key,
                w ? w->input_delay() * 1e3 : 0.0);
    }
    // On the monotonic clock, to set against when another program asked for it.
    for (const WindowResized& r : ctx.received<WindowResized>())
        KV_INFO("ticker", "tick %llu: resized to %ux%u, seen at monotonic %.6f", (unsigned long long)ctx.tick, r.width, r.height, time_now());
    for (const ExitRequested& e : ctx.received<ExitRequested>()) {
        KV_UNUSED(e);
        engine_request_quit(*ctx.engine);
    }
}

bool ticker_startup(Engine& e, ModuleInstance& self) {
    Ticker* t = arena_push<Ticker>(self.arena);
    if (!t) return false;
    self.set_state(t);
    t->want = frameloop_flag(e, "--want-ticks");
    if (const char* v = frameloop_option(e, "--minimize-at")) t->minimize_at = (u64)atoll(v);
    if (const char* v = frameloop_option(e, "--restore-at")) t->restore_at = (u64)atoll(v);
    if (const char* v = frameloop_option(e, "--frame-every")) t->every = atof(v) * 1e-3;
    if (t->every > 0.0) {
        t->asked = time_now() + t->every;
        t->asked_n = 1;
        engine_want_frame_at(e, t->asked);
    }
    if (t->want) engine_want_ticks(e);
    return true;
}

void ticker_shutdown(Engine& e, ModuleInstance& self) {
    if (Ticker* t = self.state_as<Ticker>()) KV_INFO("ticker", "%llu ticks run by the sim", (unsigned long long)t->ticks);
    if (Ticker* t = self.state_as<Ticker>(); t && t->every > 0.0)
        KV_INFO("ticker", "%llu frames asked for at a time, %llu drawn: %.2f ms late on average, %.2f ms the latest", (unsigned long long)t->asked_n,
                (unsigned long long)t->drawn_n, t->drawn_n ? t->late_sum / (f64)t->drawn_n * 1e3 : 0.0, t->late_max * 1e3);
    PlatformWaits w = platform_waits(e);
    KV_INFO("ticker", "%llu waits: %llu ended by the window, %llu by a wake, %llu by the time", (unsigned long long)w.waits,
            (unsigned long long)w.window, (unsigned long long)w.woken, (unsigned long long)w.timed);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".frame", .phase = Phase::Frame, .fn = ticker_frame, .main_thread = true},
    {.name = ".tick", .phase = Phase::Sim, .fn = ticker_tick, .reads = "ExitRequested, WindowVisibility, WindowResized, KeyDown, platform_hid.window"},
};

} // namespace

KV_MODULE_DEFINE(ticker_module) = {
    .name     = "ticker",
    .systems  = k_systems,
    .startup  = ticker_startup,
    .shutdown = ticker_shutdown,
    .memory   = {.reserve = 64 * 1024},
};

} // namespace kv

namespace {
constinit const char* const k_flags[] = {"--want-ticks"};
constinit const char* const k_options[] = {"--minimize-at", "--restore-at", "--frame-every"};
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid ticker",
    .width  = 480,
    .height = 270,
    .flags   = k_flags,
    .options = k_options,
    .pacing = kv::Pacing::Decoupled,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
