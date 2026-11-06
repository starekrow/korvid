// Korvid core: monotonic clock and sleep. Implemented per platform under src/<platform>/.
#pragma once
#include "korvid/core/types.h"

namespace kv {

f64  time_now();              // seconds, monotonic, arbitrary origin
void time_sleep(f64 seconds); // coarse; for pacing a loop that has nothing to present

} // namespace kv
