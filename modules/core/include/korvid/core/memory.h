// Korvid core: the single allocation choke point. Arenas and pools build on it later.
#pragma once
#include "korvid/core/types.h"

namespace kv {

void* mem_alloc(usize size);
void* mem_realloc(void* p, usize size);
void  mem_free(void* p);

// Live allocation count, for leak checks in tests and at shutdown.
u64 mem_live_allocations();

} // namespace kv
