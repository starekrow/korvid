#include "korvid/core/log.h"
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
    FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    fprintf(out, "[%s] %s: %s\n", level_tag(level), tag, buf);
    fflush(out);
}

void assert_fail(const char* expr, const char* file, int line) {
    fprintf(stderr, "[ASSERT] %s (%s:%d)\n", expr, file, line);
    fflush(stderr);
    abort();
}

} // namespace kv
