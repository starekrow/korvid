// The process's environment and standard streams on POSIX: setenv, and streams that are
// bytes already.
#include "korvid/core/os_process.h"
#include <stdlib.h>

namespace kv {

bool os_env_set(const char* name, const char* value) {
    if (!name || !name[0]) return false;
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

void os_stdio_binary() {}

} // namespace kv
