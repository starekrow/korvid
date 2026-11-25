#include "korvid/core/log.h"
#include "korvid/core/format.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace kv {

static LogLevel g_level = LogLevel::Info;

static const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return " INFO";
        case LogLevel::Warn:  return " WARN";
        case LogLevel::Error: return "ERROR";
        default:              return "  OFF";
    }
}

void     log_set_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level; }

void log_write(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < g_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    // One write a line, so lines from two threads do not interleave, through format.h's
    // output (the streams, or a sink where there are none).
    char line[1200];
    int n = snprintf(line, sizeof line, "[%s] %s: %s\n", level_tag(level), tag, buf);
    if (n >= (int)sizeof line) { n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    // Every level to stderr: stdout is the program's own output (a tool's results, piped
    // to another program), and a log line in the middle of it is a corrupted result.
    output_write(true, line, (usize)n);
    fflush(stderr);
}

void assert_fail(const char* expr, const char* file, int line) {
    char text[1200];
    int n = snprintf(text, sizeof text, "[ASSERT] %s (%s:%d)\n", expr, file, line);
    if (n >= (int)sizeof text) n = (int)sizeof text - 1;
    output_write(true, text, (usize)n);
    fflush(stderr);
    abort();
}

} // namespace kv
