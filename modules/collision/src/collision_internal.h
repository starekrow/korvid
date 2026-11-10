// Korvid collision: the broadphase's pair finding, apart from the system that feeds it, so
// a test can hold it against a brute-force pass.
#pragma once
#include "korvid/collision/collider.h"
#include "korvid/core/array.h"
#include "korvid/core/handle.h"

namespace kv {

// A collider as the broadphase sees it: where its shape is, how far it reaches, who it meets.
struct CollItem {
    Handle h;
    point2 p;
    f32    r;     // bounding radius: a circle's radius, a box's half diagonal
    u32    layer, mask;
    u8     shape;
    vec2   half;
};
struct CollPair { u64 a, b; }; // canonical: the lower handle bits first

struct CollPairStats {
    u32 tested = 0; // candidate pairs looked at
    u32 groups = 0; // (layer, mask) signatures among the items
};

inline bool collision_meets(u32 la, u32 ma, u32 lb, u32 mb) { return (la & mb) && (lb & ma); }
bool collision_items_overlap(const CollItem& a, const CollItem& b);

// Every overlapping pair among `items` whose layers and masks meet, canonical and sorted,
// into `out` (cleared first). Items are grouped by their (layer, mask); each group has a
// grid of its own sized to its largest member, a group is tested against itself only if
// it meets itself and against another only if their masks meet, and across two groups
// the smaller is walked over the larger's grid. A pair that cannot meet is never looked at.
// Its working arrays are in the thread's scratch: call it inside a MemScope.
void collision_find_pairs(Span<const CollItem> items, Array<CollPair>& out, CollPairStats* stats);

} // namespace kv
