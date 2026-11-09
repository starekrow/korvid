// Korvid core: sorting with a context, the same on every platform. qsort_r is not portable
// (glibc's, the BSDs' and MSVC's qsort_s each take their arguments in a different order),
// and any library's sort leaves equal elements in an order of its own; this one is the
// engine's, so even a comparator with ties sorts identically everywhere. The engine sorts
// with this, never qsort. In place, not stable: insertion sort for short runs, heapsort
// otherwise, O(n log n) at worst.
#pragma once
#include "korvid/core/types.h"

namespace kv {

typedef int (*SortCompare)(const void* a, const void* b, void* ctx);
typedef int (*SortCompareNoCtx)(const void* a, const void* b);

void sort(void* base, usize count, usize size, SortCompare compare, void* ctx);
// The same with qsort's comparator.
void sort(void* base, usize count, usize size, SortCompareNoCtx compare);

} // namespace kv
