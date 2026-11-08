// Korvid anim: the AnimPlayer component. Put it on any kind; the system "anim.play"
// (phase PostSim, every tick) advances every playing one by dt * rate, applies the loop
// mode, writes each track's channel on the same row and fires the event keys crossed.
//
// The rules, each one a test in the module:
//   - time stays in [0, duration) for Loop and PingPong: a wrap carries the remainder,
//     never resets to zero, so no frame is skipped or doubled at the seam and frame 0 is
//     never shown at t == duration.
//   - a step track with keys at t_i shows key i for [t_i, t_i+1); the last key holds.
//   - Once clamps at the end (or at zero in reverse), shows the last key, sets Done and
//     emits AnimFinished once. Loop counts `loops`; PingPong reverses `rate` at each end
//     and counts a loop per return to the start.
//   - an event key fires when the interval the player moved through contains its time,
//     half-open in the direction of travel ((t0, t1] forward, [t1, t0) backward), and
//     closed at the start of a play and of each loop, so a key at 0 fires at the start
//     of every loop and not again at the wrap, and reverse playback fires keys too.
//   - a player whose clip is stale or not yet Ready does nothing and keeps its time.
//   - clock Sim advances on unpaused ticks only; Always advances on every tick.
#pragma once
#include "korvid/core/handle.h"

namespace kv {

enum class AnimClock : u8 { Sim, Always };
enum class AnimState : u8 { Playing, Paused, Done };

struct AnimPlayer {
    Handle    clip;                       // anim.clip
    f32       time  = 0.0f;               // seconds into the clip
    f32       rate  = 1.0f;               // clip seconds per second; negative plays backward
    AnimClock clock = AnimClock::Sim;
    AnimState state = AnimState::Playing;
    u8        fresh = 1;                  // the next advance is the first: a key at the start fires
    u8        pad   = 0;
    u32       loops = 0;                  // completed loops (Loop) or returns (PingPong)
};

// Starts `clip` from `start` at `rate` (pass the duration to play backward from the end).
inline AnimPlayer anim_player(Handle clip, f32 rate = 1.0f, AnimClock clock = AnimClock::Sim, f32 start = 0.0f) {
    AnimPlayer p;
    p.clip  = clip;
    p.rate  = rate;
    p.clock = clock;
    p.time  = start;
    return p;
}
// Restarts a player on `clip` (or its current one when null).
inline void anim_play(AnimPlayer& p, Handle clip = Handle{}, f32 rate = 1.0f, f32 start = 0.0f) {
    if (!clip.is_null()) p.clip = clip;
    p.rate  = rate;
    p.time  = start;
    p.state = AnimState::Playing;
    p.fresh = 1;
    p.loops = 0;
}

// Emitted by anim.play: an event key was crossed; the clip finished (Once only).
struct AnimEvent    { Handle entity; Handle clip; u64 name; }; // name: Name::hash; name_str prints it
struct AnimFinished { Handle entity; Handle clip; };

} // namespace kv
