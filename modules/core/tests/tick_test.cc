#include "korvid/core/tick.h"
#include "korvid/core/test.h"

// Runs one simulated second of frames at fps and returns the ticks produced.
static kv::u32 ticks_in_one_second(kv::f64 fps) {
    kv::TickClock clock;
    kv::u32 frames = (kv::u32)fps;
    kv::u32 ticks  = 0;
    for (kv::u32 i = 0; i < frames; ++i) {
        kv::u32 n = clock.advance(1.0 / fps);
        for (kv::u32 t = 0; t < n; ++t) clock.begin_tick();
        ticks += n;
    }
    return ticks;
}

void test_tick(kv::TestCtx& c) {
    using namespace kv;
    c.begin("tick");

    // Same distance per second at 30, 60 and 144 fps: tick count is the same.
    u32 t30 = ticks_in_one_second(30.0);
    u32 t60 = ticks_in_one_second(60.0);
    u32 t144 = ticks_in_one_second(144.0);
    KV_CHECK(c, t30 >= 59 && t30 <= 60);
    KV_CHECK(c, t60 >= 59 && t60 <= 60);
    KV_CHECK(c, t144 >= 59 && t144 <= 60);

    // alpha is the fraction of a tick left over, in [0, 1).
    TickClock clock;
    KV_CHECK(c, clock.advance(1.0 / 120.0) == 0);
    KV_CHECK(c, clock.alpha() > 0.49f && clock.alpha() < 0.51f);
    KV_CHECK(c, clock.advance(1.0 / 120.0) == 1);
    KV_CHECK(c, clock.alpha() < 0.01f);
    KV_CHECK(c, !clock.begin_tick());
    KV_CHECK(c, clock.tick_count == 1 && clock.tick_total == 1);

    // A huge frame hits the cap: max ticks run, the surplus is dropped, alpha stays in range.
    TickClock slow;
    slow.max_ticks_per_frame = 4;
    KV_CHECK(c, slow.advance(1.0) == 4);
    KV_CHECK(c, slow.alpha() >= 0.0f && slow.alpha() < 1.0f);
    KV_CHECK(c, slow.dropped_time_ticks >= 55);

    // Negative dt is ignored.
    TickClock neg;
    KV_CHECK(c, neg.advance(-1.0) == 0);
    KV_CHECK(c, neg.alpha() == 0.0f);

    // Paused: ticks keep coming at real-time cadence, but they are paused ticks; the sim
    // tick holds and the total counts them. A step lets exactly one sim tick through.
    TickClock p;
    p.paused = true;
    KV_CHECK(c, p.advance(2.0 / 60.0) == 2);
    KV_CHECK(c, p.begin_tick() && p.begin_tick());
    KV_CHECK(c, p.tick_count == 0 && p.tick_total == 2);
    p.step_ticks = 1;
    KV_CHECK(c, p.advance(2.0 / 60.0) == 2);
    KV_CHECK(c, !p.begin_tick());  // the step
    KV_CHECK(c, p.begin_tick());   // paused again
    KV_CHECK(c, p.tick_count == 1 && p.tick_total == 4 && p.step_ticks == 0);

    // Rate scales time while unpaused and is ignored while paused.
    TickClock half;
    half.rate = 0.5;
    KV_CHECK(c, half.advance(4.0 / 60.0) == 2);
    half.paused = true;
    KV_CHECK(c, half.advance(4.0 / 60.0) == 4);
    TickClock twice;
    twice.rate = 2.0;
    twice.max_ticks_per_frame = 8;
    KV_CHECK(c, twice.advance(2.0 / 60.0) == 4);

    // Time scale: the game's factor, times the rate. Ten seconds at a quarter run 150
    // ticks of the same length; paused, it is ignored like the rate; 0 runs none.
    TickClock slow_mo;
    slow_mo.scale = 0.25;
    u32 slow_ticks = 0;
    for (u32 i = 0; i < 600; ++i) slow_ticks += slow_mo.advance(1.0 / 60.0);
    KV_CHECK(c, slow_ticks == 150 || slow_ticks == 149);
    KV_CHECK(c, slow_mo.tick_dt == 1.0 / 60.0);
    TickClock both;
    both.rate = 2.0;
    both.scale = 0.25;
    KV_CHECK(c, both.advance(4.0 / 60.0) == 2);
    both.paused = true;
    KV_CHECK(c, both.advance(2.0 / 60.0) == 2);
    TickClock frozen;
    frozen.scale = 0.0;
    KV_CHECK(c, frozen.advance(1.0) == 0 && frozen.accumulator == 0.0);
}
