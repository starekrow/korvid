// The process's environment and standard streams on Windows: the C runtime's copy of the
// environment (_putenv_s, which getenv reads and a child inherits; "" removes a variable),
// and its text mode turned off.
#include "korvid/core/os_process.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

namespace kv {

bool os_env_set(const char* name, const char* value) {
    if (!name || !name[0]) return false;
    return _putenv_s(name, value ? value : "") == 0;
}

void os_stdio_binary() {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

} // namespace kv
