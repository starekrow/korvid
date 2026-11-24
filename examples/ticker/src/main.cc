// ticker: the measure of Pacing::Decoupled (KR20). A module that, with --want-ticks, asks
// for every tick (engine_want_ticks) as a music player does while it plays, and changes
// nothing on screen. Run it for ten seconds:
//
//   ticker --pacing decoupled --want-ticks --run-seconds 10
//
// and the frame loop's last line says it ran about 600 ticks and drew a frame or two (the
// window's first). Without --want-ticks it ticks on the heartbeat only. Under --pacing
// demand, asking for ticks is asking for frames: a frame every tick.
#include "korvid/ticker/module.h"
#include "korvid/core/engine.h"
#include "korvid/core/log.h"
#include "korvid/frameloop/frameloop.h"

namespace kv {

namespace {

struct Ticker { u64 ticks = 0; bool want = false; };

void ticker_tick(TickContext& ctx) {
    Ticker& t = ctx.state_as<Ticker>();
    ++t.ticks;
    if (t.want) engine_want_ticks(*ctx.engine);
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
    if (t->want) engine_want_ticks(e);
    return true;
}

void ticker_shutdown(Engine&, ModuleInstance& self) {
    if (Ticker* t = self.state_as<Ticker>()) KV_INFO("ticker", "%llu ticks run by the sim", (unsigned long long)t->ticks);
}

constinit const SystemDesc k_systems[] = {
    {.name = ".tick", .phase = Phase::Sim, .fn = ticker_tick, .reads = "ExitRequested"},
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
constinit const kv::FrameLoopConfig k_config = {
    .title  = "korvid ticker",
    .width  = 480,
    .height = 270,
    .flags  = k_flags,
    .pacing = kv::Pacing::Decoupled,
};
}

int main(int argc, char** argv) {
    return kv::frameloop_main(argc, argv, k_config);
}
