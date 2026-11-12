// example_arcfire: the run's record and the score's rules (scoring.cc has the system that
// keeps them and the run's shape).
//
//   - The chain: a kill within the chain's window (`chain_window` ticks, 90 at Normal) of
//     the last adds one to it and pays its score times a step of it: x1 below 10, x2 to 24,
//     x3 to 49, x4 to 99, x5 from 100. A graze holds the window open 20 ticks more (up to
//     its length); a gap, a death or a restart ends the chain.
//   - Extends: an egg at 200,000 and at 600,000, then at every million; an egg item too.
//     Past five eggs, 50,000 instead.
//   - The stage bonus, at a stage's end: 20,000 an egg in hand and 5,000 a bomb.
//
// Plain code, no engine: the tests take it whole.
#pragma once
#include "korvid/core/types.h"

namespace kv::arcfire {

struct Run {
    i64 score;
    i64 stage_score;       // the score when this stage (or loop) began: a restart goes back to it
    i64 next_extend;       // the score that earns the next egg
    i32 eggs;
    u32 restarts;
    u32 epoch;             // +1 at every restart: scheduled spawns of an earlier epoch are dropped
    u32 kills, grazes, deaths;
    i32 bombs;             // in hand: 2 at the start and after every death, at most kMaxBombs
    u32 chips;             // picked up
    u32 chain;             // kills in the chain
    u32 chain_left;        // ticks of the chain's window left; 0: no chain
    u32 chain_best, bombs_used;
};
constexpr i32 kBaseBombs = 2, kMaxBombs = 6;
constexpr i32 kStartEggs = 3, kMaxEggs = 5;
constexpr i64 kFirstExtend = 200000, kSecondExtend = 600000, kExtendEvery = 1000000;
constexpr i32 kEggPast = 50000;                     // an extend or an egg item past five
constexpr i32 kEggBonus = 20000, kBombBonus = 5000; // the stage bonus, an egg and a bomb in hand
constexpr u32 kGrazeHold = 20;                      // ticks a graze adds to the chain's window

inline u32 chain_multiplier(u32 chain) { return chain < 10 ? 1 : chain < 25 ? 2 : chain < 50 ? 3 : chain < 100 ? 4 : 5; }

// A kill, in a chain whose window is `window` ticks: what it pays.
inline i64 chain_pay(Run& r, u32 window, i32 score) {
    r.chain = r.chain_left ? r.chain + 1 : 1;
    r.chain_left = window;
    if (r.chain > r.chain_best) r.chain_best = r.chain;
    return (i64)score * chain_multiplier(r.chain);
}

inline void chain_graze(Run& r, u32 window) {
    if (!r.chain_left) return;
    r.chain_left = r.chain_left + kGrazeHold < window ? r.chain_left + kGrazeHold : window;
}

// A tick of the window; true when the chain ended on it.
inline bool chain_tick(Run& r) {
    if (!r.chain_left || --r.chain_left) return false;
    r.chain = 0;
    return true;
}

inline void chain_break(Run& r) { r.chain = r.chain_left = 0; }

// An egg (an extend, an egg item), or past five its 50,000: true for an egg.
inline bool egg_gain(Run& r) {
    if (r.eggs < kMaxEggs) { ++r.eggs; return true; }
    r.score += kEggPast;
    return false;
}

// The extend after the one at `at`: 600,000, then 1,000,000, 2,000,000, ...
inline i64 extend_after(i64 at) { return at < kSecondExtend ? kSecondExtend : (at / kExtendEvery + 1) * kExtendEvery; }

inline i64 stage_bonus(const Run& r) {
    return (i64)kEggBonus * (r.eggs > 0 ? r.eggs : 0) + (i64)kBombBonus * (r.bombs > 0 ? r.bombs : 0);
}

} // namespace kv::arcfire
