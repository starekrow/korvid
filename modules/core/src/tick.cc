#include "korvid/core/tick.h"

namespace kv {

u32 TickClock::advance(f64 frame_dt) {
    if (frame_dt < 0.0) frame_dt = 0.0;
    bool stepping = paused && step_ticks > 0;
    f64 speed = (rate > 0.0 ? rate : 0.0) * (scale > 0.0 ? scale : 0.0);
    accumulator += paused && !stepping ? frame_dt : frame_dt * speed;

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
    return ticks;
}

bool TickClock::begin_tick() {
    ++tick_total;
    if (paused && step_ticks == 0) return true;
    if (paused) --step_ticks;
    ++tick_count;
    return false;
}

} // namespace kv
