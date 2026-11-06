#include "korvid/core/tick.h"
#include "korvid/core/test.h"

// Runs one simulated second of frames at fps and returns the ticks produced.
static kv::u32 ticks_in_one_second(kv::f64 fps) {
    kv::TickClock clock;
    kv::u32 frames = (kv::u32)fps;
    kv::u32 ticks  = 0;
    for (kv::u32 i = 0; i < frames; ++i) ticks += clock.advance(1.0 / fps);
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
    KV_CHECK(c, clock.tick_count == 1);

    // A huge frame hits the cap: max ticks run, the surplus is dropped, alpha stays in range.
    TickClock slow;
    slow.max_ticks_per_frame = 4;
    KV_CHECK(c, slow.advance(1.0) == 4);
    KV_CHECK(c, slow.alpha() >= 0.0f && slow.alpha() < 1.0f);
    KV_CHECK(c, slow.dropped_time_ticks >= 55);
    KV_CHECK(c, slow.tick_count == 4);

    // Negative dt is ignored.
    TickClock neg;
    KV_CHECK(c, neg.advance(-1.0) == 0);
    KV_CHECK(c, neg.alpha() == 0.0f);
}
