// The C library behind format.h: the one place outside the log and the profiler that
// includes <stdio.h>.
#include "korvid/core/format.h"
#include <stdio.h>
#include <stdlib.h>

namespace kv {

i32 vformat(char* out, usize cap, const char* fmt, va_list args) { return (i32)vsnprintf(out, cap, fmt, args); }

i32 format(char* out, usize cap, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i32 n = vformat(out, cap, fmt, args);
    va_end(args);
    return n;
}

static OutputSink g_sink = nullptr;

void output_set_sink(OutputSink sink) { g_sink = sink; }

void output_write(bool err, const char* text, usize len) {
    if (g_sink) { g_sink(err, text, len); return; }
    fwrite(text, 1, len, err ? stderr : stdout);
}

// Formatted for the sink: on the stack when it fits, else in a block of its own.
static void sink_vprint(bool err, const char* fmt, va_list args) {
    char buf[1024];
    va_list again;
    va_copy(again, args);
    int n = vsnprintf(buf, sizeof buf, fmt, args);
    if (n >= (int)sizeof buf) {
        char* big = (char*)malloc((usize)n + 1);
        if (big) {
            vsnprintf(big, (usize)n + 1, fmt, again);
            g_sink(err, big, (usize)n);
            free(big);
        } else {
            g_sink(err, buf, sizeof buf - 1);
        }
    } else if (n > 0) {
        g_sink(err, buf, (usize)n);
    }
    va_end(again);
}

void vprint(const char* fmt, va_list args) {
    if (g_sink) { sink_vprint(false, fmt, args); return; }
    vfprintf(stdout, fmt, args);
}
void print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args);
    va_end(args);
}
void vprint_err(const char* fmt, va_list args) {
    if (g_sink) { sink_vprint(true, fmt, args); return; }
    vfprintf(stderr, fmt, args);
}
void print_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint_err(fmt, args);
    va_end(args);
}
void print_flush() { if (!g_sink) fflush(stdout); }

} // namespace kv
