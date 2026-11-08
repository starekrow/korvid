// Korvid level: places to be, loaded behind a fade with the sim held. A game hands the
// module its catalog at startup and asks for levels by name; the module runs the
// transition and tells the game what to do at each step through events:
//
//   LevelLeaving  the fade starts; the old level is still there (save what you keep)
//   LevelLoading  a new asset scope exists and the map is loading into it; load more into it
//   LevelBuild    every asset in the scope is settled; spawn the level under `root`
//   LevelEntered  the fade has gone; the level is live
//
// The steps, in "level.run" (PreSim, main thread, every tick):
//
//   1. Leaving: a screen-space quad fades in over `fade_out` seconds. The sim is held
//      (HoldLoading) from the start of it, or, with `freeze` false, from its end.
//   2. Loading: a new scope, the map loaded into it by the catalog's loader, LevelLoading.
//      The module waits until nothing in the scope is pending.
//   3. Building, over two ticks: on the first, the old root entity is destroyed (its
//      children and tile maps with it) and then the old scope released, after the new one
//      holds its references, so assets the two levels share never leave memory; on the
//      second, a new root and LevelBuild. No tick shows both levels' entities.
//   4. Entering: the hold clears, the quad fades out over `fade_in`, LevelEntered.
//
// A request during a transition waits for it to finish (the latest one wins). A request
// for the current level is a reload: the old scope is released before anything loads, so
// every file of the level is read again; LevelBuild's `entry` is then "reload".
//
// Because the build happens on held ticks, a level's first sim tick has the same tick
// number however long the disk took, and a demo that switches levels hashes the same
// run to run. The fade counts every tick (held or not) at the tick's length.
#pragma once
#include "korvid/core/handle.h"
#include "korvid/math/color.h"

namespace kv {

struct Engine;

constexpr u32 KV_LEVEL_NAME_MAX  = 32;  // bytes, NUL included
constexpr u32 KV_LEVEL_ENTRY_MAX = 32;
constexpr u32 KV_LEVELS          = 64;  // catalog entries

// What a level is: a name to ask for, a map to load, a word for the game.
struct LevelDesc {
    const char* name;
    const char* map;      // an asset path; what loads it is the catalog's loader
    u64         user = 0;
};

// Loads a level's map into its scope and returns the asset handle LevelBuild carries.
// The default is asset_load_blob; a game with Tiled maps passes tilemap_load.
typedef Handle (*LevelMapLoader)(Engine& e, const char* map, Handle scope);

struct LevelConfig {
    Span<const LevelDesc> levels;
    LevelMapLoader        load = nullptr; // null: asset_load_blob
};

struct LevelTransition {
    f32         fade_out = 0.25f; // seconds; 0 cuts
    f32         fade_in  = 0.25f;
    color       tint     = color_rgba(0.0f, 0.0f, 0.0f, 1.0f); // the quad's color at full fade
    const char* entry    = nullptr; // copied: which way the player came in, for the game
    bool        freeze   = true;    // hold the sim from the request on; false: it runs until the screen is covered
};

enum class LevelState : u8 { None, Leaving, Loading, Building, Entering, Active };

// Events. `level` is the catalog index; level_name gives the name back.
struct LevelLeaving { u32 level; };              // the level being left (KV_LEVELS when there is none)
struct LevelLoading { u32 level; Handle scope; };
struct LevelBuild   { u32 level; Handle root; Handle map; Handle scope; char entry[KV_LEVEL_ENTRY_MAX]; };
struct LevelEntered { u32 level; };

// The catalog, copied. Call at startup, before any request; a second call replaces it.
bool        level_catalog(Engine& e, const LevelConfig& config);
// Starts (or queues) a transition. False for a name not in the catalog.
bool        level_request(Engine& e, const char* name, const LevelTransition& t = LevelTransition{});
bool        level_reload(Engine& e, const LevelTransition& t = LevelTransition{});
// The level that is live or being entered (KV_LEVELS when none), its name, and where the
// transition is.
u32         level_current(Engine& e);
const char* level_name(Engine& e, u32 level);
u32         level_find(Engine& e, const char* name);
LevelState  level_state(Engine& e);
// The current level's root entity (level.root), scope and map; null before the first build.
Handle      level_root(Engine& e);
Handle      level_scope(Engine& e);
Handle      level_map(Engine& e);
// The catalog's user word.
u64         level_user(Engine& e, u32 level);

} // namespace kv
