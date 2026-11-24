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
#include "korvid/ticker/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"
#include "korvid/platform_hid/events.h"
#include "korvid/platform_hid/module.h"
#include <stdlib.h>

namespace kv {

namespace {

struct Ticker { u64 ticks = 0; bool want = false; u64 minimize_at = 0, restore_at = 0; };

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
    if (t->want) engine_want_ticks(e);
    return true;
}

void ticker_shutdown(Engine&, ModuleInstance& self) {
    if (Ticker* t = self.state_as<Ticker>()) KV_INFO("ticker", "%llu ticks run by the sim", (unsigned long long)t->ticks);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::Sim, .fn = ticker_tick, .reads = "ExitRequested, WindowVisibility, platform_hid.window"},
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
constinit const char* const k_options[] = {"--minimize-at", "--restore-at"};
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
