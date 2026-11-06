#include "korvid/core/memory.h"
#include <stdlib.h>

namespace kv {

static u64 g_live = 0;

void* mem_alloc(usize size) {
    void* p = malloc(size ? size : 1);
    KV_ASSERT(p);
    ++g_live;
    return p;
}

void* mem_realloc(void* p, usize size) {
    if (!p) return mem_alloc(size);
    void* q = realloc(p, size ? size : 1);
    KV_ASSERT(q);
    return q;
}

void mem_free(void* p) {
    if (!p) return;
    free(p);
    --g_live;
}

u64 mem_live_allocations() { return g_live; }

} // namespace kv
