// cpukiller's brawl: the problem that tangles threaded game code, four ways.
//
// Every tick every fighter hits one other for a few points. One hit in eight goes to one
// of sixteen fighters everybody wants (kBrawlBosses), so several threads land on the same
// victim at once. A fighter at zero is dead: its killer is credited, loot drops, and it is
// replaced at full health. That is three writes to somebody else's state (the victim's
// health, the death, the loot) from wherever the attacker happens to be running.
//
// Three versions over bare arrays, each the way it is commonly written, on the engine's
// job pool:
//   Naive    `hp[victim] -= damage`, no synchronization. A data race, and meant to be.
//   Atomic   the subtraction is one atomic add; whoever takes the health through zero
//            has the kill.
//   Locked   a lock for the victim (4,096 of them, by index), the naive code inside it.
// And the engine's: a system whose chunks emit a Hit for each swing, and one handler that
// applies them (brawl.cc).
//
// Each is held to the same books, kept apart from the code under test:
//   damage lost    the damage the attackers say they dealt, less the health that is
//                  actually missing (now, or made good when the dead were replaced)
//   extra deaths   the deaths the attackers counted, less the fighters actually replaced
//   the hash       of the whole state after the last tick: the same from run to run, and
//                  on one thread as on many, or the fight cannot be replayed
#pragma once
#include "korvid/core/engine.h"
#include "korvid/core/hash.h"
#include "korvid/core/module.h"

namespace kv {

constexpr i32 kBrawlFullHealth = 100;
constexpr u32 kBrawlBosses     = 16;

// Fighter i's swing on `tick`: whom, of `count`, and how hard (1 to 8). The same for
// every version.
struct BrawlSwing { u32 target; u32 damage; };
inline BrawlSwing brawl_swing(u32 i, u32 count, u64 tick) {
    u64 h = hash_mix(((u64)i << 32) ^ (tick * 0x9e3779b97f4a7c15ull));
    u32 target = (h & 7) == 0 && count > kBrawlBosses ? (u32)((h >> 8) % kBrawlBosses) : (u32)((h >> 8) % count);
    if (target == i) target = (target + 1) % count;
    return BrawlSwing{target, 1 + (u32)((h >> 44) & 7)};
}

// What a run comes to, by the books above.
struct BrawlBooks {
    u64 dealt = 0, deaths = 0;
    i64 damage_lost = 0, extra_deaths = 0;
    u64 hash = 0;
};

// ---- the bare versions -------------------------------------------------------------------
enum class BareBrawl : u8 { Naive, Atomic, Locked };
struct BareArena;
BareArena* bare_arena_create(u32 fighters); // null with no memory
void       bare_arena_destroy(BareArena* a);
// One tick: every fighter swings, then the dead are replaced. On the job pool when
// `parallel` (jobs_run), else a loop on the caller.
void       bare_arena_tick(BareArena* a, BareBrawl how, bool parallel);
BrawlBooks bare_arena_books(const BareArena* a);

// ---- the engine's ------------------------------------------------------------------------
KV_MODULE_DECLARE(brawl_module);
// The module list for an engine that runs the brawl and nothing else.
Span<const ModuleDesc* const> brawl_modules();
// Fighters to spawn, set before engine_init.
void       brawl_set_fighters(u32 n);
BrawlBooks brawl_books(Engine& e);

} // namespace kv
