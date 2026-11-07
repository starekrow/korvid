#include "korvid/core/cli.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kv {

void CliContext::vprint(const char* fmt, va_list args) {
    char line[1024];
    vsnprintf(line, sizeof line, fmt, args);
    if (sink) sink(user, line);
}

void CliContext::print(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprint(fmt, args);
    va_end(args);
}

void CliContext::fail(const char* fmt, ...) {
    if (failed) return; // the first reason stands
    failed = true;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, sizeof error, fmt, args);
    va_end(args);
}

bool cli_arg_f32(CliContext& ctx, const char* arg, const char* what, f32* out) {
    char* end = nullptr;
    errno = 0;
    double v = strtod(arg ? arg : "", &end);
    if (!arg || end == arg || *end || errno) { ctx.fail("%s: '%s' is not a number", what, arg ? arg : ""); return false; }
    *out = (f32)v;
    return true;
}

bool cli_arg_u32(CliContext& ctx, const char* arg, const char* what, u32* out) {
    char* end = nullptr;
    errno = 0;
    unsigned long v = strtoul(arg ? arg : "", &end, 10);
    if (!arg || end == arg || *end || errno || v > 0xFFFFFFFFul) { ctx.fail("%s: '%s' is not a count", what, arg ? arg : ""); return false; }
    *out = (u32)v;
    return true;
}

} // namespace kv
