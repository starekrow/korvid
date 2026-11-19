// Korvid core: the page layer: address space and pages from the OS.
//
// The only part of the memory system that is ported; arenas and the heaps (memory.h,
// arena.h) are built on it. A range is used in three steps: reserve address space, which
// costs no memory; commit pages inside it as they are needed, which makes them readable,
// writable and zero; decommit them to hand the memory back while keeping the addresses.
// Release gives back the whole reservation. Everything is in whole pages: os_page_size()
// bytes, 4 KiB on x64 and 16 KiB on Apple silicon. Implemented per platform under
// src/<platform>/os_memory.cc (mmap and mprotect on POSIX, VirtualAlloc on Windows).
#pragma once
#include "korvid/core/types.h"

namespace kv {

// The OS's page size in bytes, a power of two; asked once and remembered.
usize os_page_size();
// Reserves address space with no backing; null on failure. size rounds up to pages.
void* os_reserve(usize size);
// Makes [p, p + size) readable and writable; p and size are page aligned and inside a
// reservation. Pages committed for the first time read as zeros. False when the OS
// refuses.
bool  os_commit(void* p, usize size);
// Returns the pages' contents to the OS; the range stays reserved, and a later os_commit of
// it reads as zeros on every platform.
void  os_decommit(void* p, usize size);
// Releases a reservation made by os_reserve, in full: `p` is what os_reserve returned and
// `size` what it was asked for (Windows releases from the base and ignores the size).
void  os_release(void* p, usize size);

// `n` rounded up to a whole number of pages.
inline usize os_page_align(usize n) {
    usize ps = os_page_size();
    return (n + ps - 1) & ~(ps - 1);
}

} // namespace kv
