// Korvid core: the page layer. The only part of the memory system that is ported.
// Reserve address space, commit pages into it on demand, give them back. Implemented
// per platform under src/<platform>/os_memory.cc.
#pragma once
#include "korvid/core/types.h"

namespace kv {

usize os_page_size();
// Reserves address space with no backing; null on failure. size rounds up to pages.
void* os_reserve(usize size);
// Makes [p, p + size) readable and writable; p and size are page aligned.
bool  os_commit(void* p, usize size);
// Returns the pages' contents to the OS; the range stays reserved, and a later os_commit of
// it reads as zeros on every platform.
void  os_decommit(void* p, usize size);
// Releases a reservation made by os_reserve, in full.
void  os_release(void* p, usize size);

inline usize os_page_align(usize n) {
    usize ps = os_page_size();
    return (n + ps - 1) & ~(ps - 1);
}

} // namespace kv
