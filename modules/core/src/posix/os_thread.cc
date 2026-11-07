#include "korvid/core/thread.h"
#include <unistd.h>

namespace kv {

u32 os_cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (u32)n : 1;
}

} // namespace kv
