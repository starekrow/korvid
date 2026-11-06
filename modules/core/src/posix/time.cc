#include "korvid/core/time.h"
#include <time.h>

namespace kv {

f64 time_now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

void time_sleep(f64 seconds) {
    if (seconds <= 0.0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (f64)ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
}

} // namespace kv
