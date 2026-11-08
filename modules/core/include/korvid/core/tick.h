// Korvid core: fixed-tick clock driven by an accumulator.
// Ticks per frame are capped; past the cap the leftover is dropped and the game slows.
// alpha() is the interpolation point for rendering between the previous and current tick.
//
// Sim control: `paused` holds the simulation. The clock still produces ticks at its rate
// while paused (so input, menus, the extract and the CLI keep running) but they are
// paused ticks: tick_count, the sim tick that TickContext::tick and the timer wheel use,
// does not advance; tick_total counts every tick run. `step_ticks` lets that many sim
// ticks through a pause. `rate` scales time while unpaused (0.5 is half speed); a paused
// clock ignores it so the paused ticks keep real-time cadence.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct TickClock {
    f64 tick_dt             = 1.0 / 60.0;
    u32 max_ticks_per_frame = 4;
    f64 accumulator         = 0.0;
    u64 tick_count          = 0;   // sim ticks run; the engine advances it per unpaused tick
    u64 tick_total          = 0;   // every tick run, paused ones included
    u64 dropped_time_ticks  = 0;   // ticks' worth of time discarded at the cap
    f64 rate                = 1.0; // time scale while unpaused; <= 0 means no ticks at all
    u32 step_ticks          = 0;   // sim ticks still to let through a pause
    bool paused             = false;

    // Adds frame_dt (scaled by rate unless paused) and returns how many ticks to run this
    // frame. Callers run exactly that many, deciding per tick with begin_tick.
    u32 advance(f64 frame_dt);

    // Whether the next tick is a paused one, then books it: advances tick_total, and
    // tick_count or step_ticks as the case may be. Returns true for a paused tick.
    bool begin_tick();

    f32 alpha() const { return (f32)(accumulator / tick_dt); }
};

} // namespace kv
