#include "korvid/core/tick.h"

namespace kv {

u32 TickClock::advance(f64 frame_dt) {
    if (frame_dt < 0.0) frame_dt = 0.0;
    accumulator += frame_dt;

    f64 cap = (f64)max_ticks_per_frame * tick_dt;
    if (accumulator > cap) {
        // Past the cap the game slows: the surplus is dropped, not caught up later.
        dropped_time_ticks += (u64)((accumulator - cap) / tick_dt);
        accumulator = cap;
    }

    u32 ticks = 0;
    while (accumulator >= tick_dt && ticks < max_ticks_per_frame) {
        accumulator -= tick_dt;
        ++ticks;
    }
    if (accumulator < 0.0) accumulator = 0.0;
    tick_count += ticks;
    return ticks;
}

} // namespace kv
