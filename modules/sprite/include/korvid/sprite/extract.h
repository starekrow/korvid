// Korvid sprite: the extract. Once a frame (the Frame cycle, docs/design/cycles.md), what
// changed in every kind that has Transform2 and Sprite becomes a run of deltas for the
// renderer's persistent world: a Tick marker when ticks ran since the last extract,
// a Remove for every entity the renderer knew that is gone, and a Set for every entity
// the renderer has not seen or whose transform or sprite differs from what it was last
// told. Nothing is copied twice. The record of what the renderer knows lives here, per
// table, by slot, so no table needs a destroy hook: a slot whose handle changed or died
// since the last extract is a Remove.
//
// Teleports. A transform that differs from what the renderer was told is a move, and the
// renderer interpolates to it, unless the engine's previous copy already equals the new
// transform: then the entity got there without passing through the space between (a
// wrap, a respawn), the Set carries `snap`, and the renderer shows it there at once. A
// system teleports by writing the previous copy along with the current one.
//
// Several ticks in one frame. The renderer interpolates a sprite from where it was told it
// was to where it is now over the last tick. When more than one tick ran since the last
// extract, a sprite that moved first goes to where it was at the start of the last tick
// (the engine's previous copy) with snap, before the Tick marker, so what is interpolated
// is the last tick's motion and not the whole frame's. A frame with no tick in it has no
// marker, so the renderer keeps interpolating the last tick.
#pragma once
#include "korvid/core/array.h"
#include "korvid/render/sprite.h"
#include "korvid/sprite/layers.h"
#include "korvid/sprite/sprite.h"
#include "korvid/sprite/transform2.h"

namespace kv {

struct Engine;

struct ExtractTable {
    u64               seen_churn = ~0ull; // a still kind's churn at the last extract: unchanged, the table is skipped
    u32               seen_restores = 0;  // its restores then: a restore since means one full compare
    Array<Handle>     sent;            // by slot: the handle the renderer knows there, or null
    Array<Sprite>     sent_sprites;    // by slot: the Sprite it was last told
    Array<Transform2> sent_placements; // by slot: the Transform2 it was last told
};

struct ExtractState {
    Array<ExtractTable> tables; // by engine table index
    // How many sprite rows the game expects (FrameLoopConfig::sprite_rows; 0: none said).
    // A kind's side tables are sized at its first growth to that or its capacity, the
    // smaller, and the delta list to twice it at the first extract, so neither grows (a
    // copy of megabytes in one tick) in the middle of a run.
    u32 rows_hint = 0;
    vec2 sent_layers[256] = {}; // the layers' offsets the renderer was last told
    u64  seen_ticks = 0;        // the clock's tick_total at the last extract (the module's system)
    void free();
};

// Appends the deltas since the last extract to `out`, `ticks` ticks later: with two or more,
// the catch-up Sets; with one or more, the Tick marker; then a Layer delta for each layer
// whose offset changed (clearing its snap), the Removes and the Sets. Updates the record of
// what the renderer knows. `layers` may be null.
void extract_sprites(Engine& e, ExtractState& st, SpriteLayers* layers, Array<SpriteDelta>& out, u32 ticks = 1);

} // namespace kv
