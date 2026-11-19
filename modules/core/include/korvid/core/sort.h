// Korvid core: sorting with a context, the same on every platform. qsort_r is not portable
// (glibc's, the BSDs' and MSVC's qsort_s each take their arguments in a different order),
// and any library's sort leaves equal elements in an order of its own; this one is the
// engine's, so even a comparator with ties sorts identically everywhere. The engine sorts
// with this, never qsort. In place, not stable: insertion sort for 16 elements or fewer,
// heapsort otherwise, O(n log n) at worst. Nothing here allocates.
//
// The comparator returns below 0, 0 or above 0 for a before, equal to or after b, as
// qsort's does. `ctx` is passed through to it, for a sort that needs more than the two
// elements (from modules/pack/src/zip.cc, entries ordered by names kept elsewhere):
//
//   kv::sort(t.entries, t.count, sizeof(PackEntry), compare_entry, t.names);
#pragma once
#include "korvid/core/types.h"

namespace kv {

// A comparator that gets the sort's `ctx`.
typedef int (*SortCompare)(const void* a, const void* b, void* ctx);
// qsort's comparator.
typedef int (*SortCompareNoCtx)(const void* a, const void* b);

// Sorts `count` elements of `size` bytes at `base`, by `compare`, which gets `ctx`.
void sort(void* base, usize count, usize size, SortCompare compare, void* ctx);
// The same with qsort's comparator.
void sort(void* base, usize count, usize size, SortCompareNoCtx compare);

} // namespace kv
