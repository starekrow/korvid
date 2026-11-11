#include "korvid/core/os_memory.h"
#include <sys/mman.h>
#include <unistd.h>

namespace kv {

usize os_page_size() {
    static usize ps = 0;
    if (!ps) ps = (usize)sysconf(_SC_PAGESIZE);
    return ps;
}

void* os_reserve(usize size) {
    size = os_page_align(size);
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool os_commit(void* p, usize size) {
    return mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}

void os_decommit(void* p, usize size) {
#if defined(__APPLE__)
    // MADV_DONTNEED is advice on macOS: the pages may stay resident and keep their bytes.
    // A fresh anonymous mapping over the range releases them and reads as zeros on the next
    // commit, as Linux's MADV_DONTNEED on private anonymous memory and Windows's
    // MEM_DECOMMIT do. The range stays reserved: MAP_FIXED replaces, never unmaps.
    mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#else
    madvise(p, size, MADV_DONTNEED);
    mprotect(p, size, PROT_NONE);
#endif
}

void os_release(void* p, usize size) {
    munmap(p, os_page_align(size));
}

} // namespace kv
