// Korvid sprite: the extract. Each tick, what changed in every kind that has Transform2
// and Sprite becomes a run of deltas for the renderer's persistent world: a Tick marker,
// a Remove for every entity the renderer knew that is gone, and a Set for every entity
// the renderer has not seen or whose transform or sprite differs from what it was last
// told. Nothing is copied twice. The record of what the renderer knows lives here, per
// table, by slot, so no table needs a destroy hook: a slot whose handle changed or died
// since the last extract is a Remove.
#pragma once
#include "korvid/core/array.h"
#include "korvid/render/sprite.h"
#include "korvid/sprite/sprite.h"

namespace kv {

struct Engine;

struct ExtractTable {
    Array<Handle> sent;         // by slot: the handle the renderer knows there, or null
    Array<Sprite> sent_sprites; // by slot: the Sprite it was last told
};

struct ExtractState {
    Array<ExtractTable> tables; // by engine table index
    void free();
};

// Appends one tick's deltas to `out`, starting with the Tick marker, and updates the
// record of what the renderer knows.
void extract_sprites(Engine& e, ExtractState& st, Array<SpriteDelta>& out);

} // namespace kv
