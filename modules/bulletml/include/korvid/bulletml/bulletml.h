// Korvid bulletml: BulletML patterns, compiled and run. A library, with no kinds, systems or
// engine state: a game compiles documents into a BmlLibrary at load, gives each thing
// that runs a pattern a BmlRunner (plain data, a component in the game's kinds), and steps
// the runners every tick, answering where each one is and where the target is and taking
// its fires as calls. The game owns the bullets.
//
// BulletML (Kenta Cho's format) as this module reads it:
//   - Elements: bulletml (type "vertical" or "none"), action, fire, bullet, repeat (times,
//     action or actionRef), wait, vanish, changeDirection (direction, term), changeSpeed
//     (speed, term), accel (horizontal, vertical, term), direction and speed with their
//     types, actionRef, bulletRef and fireRef with param. Labels are a document's own.
//   - Expressions: numbers, $1..$12 (a reference's params), $rand (uniform in [0, 1), the
//     runner's own stream), $rank (the game's), + - * / %, unary minus, parentheses.
//   - Units: world units and ticks. A speed of 2 is two units a tick; a wait of 30 is
//     thirty ticks (the reference's frames).
//   - Directions: degrees, clockwise, 0 up the screen (the reference's convention for
//     type="vertical"): direction d moves along (sin d, cos d) in a +y-up world.
//     aim: toward the target + value. absolute: value. relative: the runner's own
//     direction + value. sequence: the track's last fire + value (aim, before any fire).
//   - Speeds: absolute, relative (the runner's own + value), sequence (the track's last
//     fire + value; 1 before any fire). A bullet without one has speed 1, without a
//     direction it aims.
//   - accel: horizontal positive is right, vertical positive is down the screen (the
//     reference's screen convention), added to the polar motion; see bml_velocity.
//   - changeDirection, changeSpeed and accel run beside the action for their term (one of
//     each at a time; a new one replaces the old); sequence changes by the value a tick,
//     the others reach their value at the term's end (direction by the shorter way round).
//   - The actions labelled "top..." of a document start together; so do a bullet's
//     actions. At most KV_BML_TRACKS of either.
// Limits, checked when a document compiles: KV_BML_DEPTH nested actions on a track
// (repeats and references count), KV_BML_PARAMS parameters live on a track, no recursion.
// A step runs at most KV_BML_STEP_OPS ops and KV_BML_STEP_FIRES fires per track; past
// them the track continues next tick, so a runaway repeat costs a bounded tick.
#pragma once
#include "korvid/core/array.h"
#include "korvid/math/vec2.h"

namespace kv {

constexpr u32 KV_BML_TRACKS     = 2;
constexpr u32 KV_BML_DEPTH      = 6;
constexpr u32 KV_BML_PARAMS     = 12;
constexpr u32 KV_BML_STEP_OPS   = 4096;
constexpr u32 KV_BML_STEP_FIRES = 1024;
constexpr u32 KV_BML_NONE       = 0xFFFFFFFFu;

// A runner: where each track is in its program, its parameters, its pending changes, its
// random stream and the motion it drives. Plain data, no pointers, so it can be a
// component of any kind and hash as one. Frames are [track][depth][field].
enum BmlFrame16 : u32 { BML_ACT, BML_PC, BML_LEFT, BML_F16 };    // action id, op offset in it, repeats left
enum BmlFrame8 : u32 { BML_PBASE, BML_PCOUNT, BML_SPB, BML_F8 };  // its params on the track's stack, the stack before it
enum BmlTrack8 : u32 { BML_DEPTH, BML_SP, BML_T8 };
enum BmlTerm : u32 { BML_TERM_DIR, BML_TERM_SPEED, BML_TERM_ACCEL, BML_TERMS };
enum BmlFlags : u32 { BML_VANISH = 1, BML_FIRED = 2 }; // vanished; track t has fired (the bit BML_FIRED << t: sequence has a last)
struct BmlRunner {
    u64 rng;                                       // PCG32 state ($rand)
    u32 wait[KV_BML_TRACKS];                       // ticks left of a wait
    u16 frame16[KV_BML_TRACKS][KV_BML_DEPTH][BML_F16];
    u8  frame8[KV_BML_TRACKS][KV_BML_DEPTH][BML_F8];
    u8  track8[KV_BML_TRACKS][BML_T8];
    f32 params[KV_BML_TRACKS][KV_BML_PARAMS];
    f32 last[KV_BML_TRACKS][2];                    // the track's last fire: direction, speed
    u32 term[BML_TERMS];                           // ticks left of each change
    f32 delta[4];                                  // per tick: direction, speed, accel x, accel y
    f32 dir, speed;                                // the motion it drives: degrees, units a tick
    f32 mx, my;                                    // accel's part: right, down the screen
    u32 flags;
};

// What a fire hands the game: where the new bullet goes and how fast, its definition
// (bml_bullet_actions says whether it runs a program), and its parameters.
struct BmlFire {
    f32 dir, speed;
    u32 bullet;
    u32 nparams;
    f32 params[KV_BML_PARAMS];
};

// What a step needs to know of the world.
struct BmlWorld {
    point2 pos;     // the runner's position
    point2 target;  // what aim aims at
    f32    rank;    // $rank
};

typedef void (*BmlFireFn)(void* user, const BmlFire& fire);

enum class BmlStep : u8 { Running, Done, Vanish };

// ---- the library ------------------------------------------------------------------------------

struct BmlBullet {
    u32  dir_type, dir_expr, speed_type, speed_expr; // BmlDirType / BmlSpeedType, KV_BML_NONE for none
    u32  actions[KV_BML_TRACKS];
    u32  action_count;
    char label[32];
};
struct BmlEntry {
    char name[48];
    u32  actions[KV_BML_TRACKS];
    u32  count;
};
// Compiled patterns: immutable once loaded, shared by every runner on every thread.
struct BmlLibrary {
    Array<u32>       ops;      // every action's ops, back to back
    Array<u32>       actions;  // action id -> its first op
    Array<u32>       code;     // expressions, back to back
    Array<BmlBullet> bullets;
    Array<BmlEntry>  entries;  // one a document: its top actions
};

// Compiles one document into the library under `name`. Returns the entry's id, or
// KV_BML_NONE with `why` saying what and where (the element and its line). The library
// is unchanged by a document that fails.
u32  bml_compile(BmlLibrary& lib, const char* name, const char* text, usize len, char* why, usize why_cap);
u32  bml_find(const BmlLibrary& lib, const char* name);
void bml_free(BmlLibrary& lib);
// Whether a bullet runs a program of its own (has actions), and its label ("" for none).
bool        bml_bullet_actions(const BmlLibrary& lib, u32 bullet);
const char* bml_bullet_label(const BmlLibrary& lib, u32 bullet);

// ---- running ---------------------------------------------------------------------------------

// A runner for an entry's top actions. `dir` and `speed` are the runner's own motion,
// which `relative` reads (an enemy facing down is 180).
void bml_start(const BmlLibrary& lib, BmlRunner& r, u32 entry, u64 seed, f32 dir = 180.0f, f32 speed = 0.0f);
// A runner for a fired bullet: its actions with the fire's parameters, its motion the fire's.
void bml_start_bullet(const BmlLibrary& lib, BmlRunner& r, const BmlFire& fire, u64 seed);
// One tick: the pending changes move the runner's motion, then each track runs until it
// waits or ends. Fires go to `fire` in the order they happen. Done when every track has
// ended and no change is pending; Vanish when the program said so.
BmlStep bml_step(const BmlLibrary& lib, BmlRunner& r, const BmlWorld& w, BmlFireFn fire, void* user);

// The velocity of a motion, world units a tick, +y up.
vec2 bml_velocity(f32 dir, f32 speed, f32 mx = 0.0f, f32 my = 0.0f);
// The direction (degrees, as above) from one point to another.
f32  bml_aim(point2 from, point2 to);

} // namespace kv
