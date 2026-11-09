#include "korvid/core/os_memory.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace kv {

// Windows reserves at 64 KiB granularity and releases only a whole reservation, from its
// base; the memory system reserves each range once and releases it whole, so the page
// layer's contract holds as on POSIX.

usize os_page_size() {
    static usize ps = 0;
    if (!ps) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        ps = (usize)si.dwPageSize;
    }
    return ps;
}

void* os_reserve(usize size) {
    return VirtualAlloc(nullptr, os_page_align(size), MEM_RESERVE, PAGE_NOACCESS);
}

bool os_commit(void* p, usize size) {
    return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void os_decommit(void* p, usize size) {
    VirtualFree(p, size, MEM_DECOMMIT);
}

void os_release(void* p, usize) {
    VirtualFree(p, 0, MEM_RELEASE);
}

} // namespace kv
