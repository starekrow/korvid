// The C library behind format.h: the one place outside the log and the profiler that
// includes <stdio.h>.
#include "korvid/core/format.h"
#include <stdio.h>

namespace kv {

i32 vformat(char* out, usize cap, const char* fmt, va_list args) { return (i32)vsnprintf(out, cap, fmt, args); }

i32 format(char* out, usize cap, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i32 n = vformat(out, cap, fmt, args);
    va_end(args);
    return n;
}

void vprint(const char* fmt, va_list args) { vfprintf(stdout, fmt, args); }
void print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args);
    va_end(args);
}
void vprint_err(const char* fmt, va_list args) { vfprintf(stderr, fmt, args); }
void print_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint_err(fmt, args);
    va_end(args);
}
void print_flush() { fflush(stdout); }

} // namespace kv
