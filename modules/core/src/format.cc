// The C library behind format.h: the one place outside the log and the profiler that
// includes <stdio.h>.
#include "korvid/core/format.h"
#include "korvid/core/atomic.h"
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

// A line has ended on stdout since the last flush: print_flush_lines' to push out.
static u32 g_line_ended = 0;

void output_write(bool err, const char* text, usize len) {
    if (g_sink) { g_sink(err, text, len); return; }
    fwrite(text, 1, len, err ? stderr : stdout);
    if (!err && len && text[len - 1] == '\n') atomic_store(&g_line_ended, 1u, MO_RELEASE);
}

// Formatted, then written in one piece (so output_write sees how the text ends): on the
// stack when it fits, else in a block of its own.
static void sink_vprint(bool err, const char* fmt, va_list args) {
    char buf[1024];
    va_list again;
    va_copy(again, args);
    int n = vsnprintf(buf, sizeof buf, fmt, args);
    if (n >= (int)sizeof buf) {
        char* big = (char*)malloc((usize)n + 1);
        if (big) {
            vsnprintf(big, (usize)n + 1, fmt, again);
            output_write(err, big, (usize)n);
            free(big);
        } else {
            output_write(err, buf, sizeof buf - 1);
        }
    } else if (n > 0) {
        output_write(err, buf, (usize)n);
    }
    va_end(again);
}

void vprint(const char* fmt, va_list args) { sink_vprint(false, fmt, args); }
void print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args);
    va_end(args);
}
void vprint_err(const char* fmt, va_list args) { sink_vprint(true, fmt, args); }
void print_err(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint_err(fmt, args);
    va_end(args);
}
void print_flush() {
    atomic_store(&g_line_ended, 0u, MO_RELEASE);
    if (!g_sink) fflush(stdout);
}

void print_flush_lines() {
    if (atomic_load(&g_line_ended, MO_ACQUIRE)) print_flush();
}

} // namespace kv
