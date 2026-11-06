// Korvid core: fixed-tick clock driven by an accumulator.
// Ticks per frame are capped; past the cap the leftover is dropped and the game slows.
// alpha() is the interpolation point for rendering between the previous and current tick.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct TickClock {
    f64 tick_dt             = 1.0 / 60.0;
    u32 max_ticks_per_frame = 4;
    f64 accumulator         = 0.0;
    u64 tick_count          = 0;
    u64 dropped_time_ticks  = 0; // ticks' worth of time discarded at the cap

    // Adds frame_dt and returns how many ticks to run this frame.
    // Callers run exactly that many ticks; tick_count is advanced here.
    u32 advance(f64 frame_dt);

    f32 alpha() const { return (f32)(accumulator / tick_dt); }
};

} // namespace kv
