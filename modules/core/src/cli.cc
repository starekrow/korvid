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

namespace kv {

bool cli_resolve(CliContext& ctx, const char* arg, char* out, usize cap) {
    if (cap < 2) return false;
    const char* base = arg && arg[0] == '/' ? "" : (ctx.cwd ? ctx.cwd : "/");
    char joined[2 * KV_CLI_PATH_MAX];
    int n = snprintf(joined, sizeof joined, "%s/%s", base, arg ? arg : "");
    if (n < 0 || (usize)n >= sizeof joined) { ctx.fail("path too long"); return false; }
    // Walk the components, applying "." and "..", building `out` in place.
    usize len = 0;
    out[len++] = '/';
    const char* p = joined;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') ++p;
        usize clen = (usize)(p - start);
        if (clen == 1 && start[0] == '.') continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 1 && out[len - 1] != '/') --len; // drop the last component
            if (len > 1) --len;                           // and its slash
            if (len == 0) len = 1;
            continue;
        }
        if (len > 1) { if (len + 1 >= cap) { ctx.fail("path too long"); return false; } out[len++] = '/'; }
        if (len + clen >= cap) { ctx.fail("path too long"); return false; }
        memcpy(out + len, start, clen);
        len += clen;
    }
    out[len] = 0;
    return true;
}

bool CliTree::add(const char* full_name, bool container, const char* summary) {
    if (!more) return false;
    usize dlen = strlen(dir);
    if (strncmp(full_name, dir, dlen) != 0) return true;
    const char* rest = full_name + dlen;
    if (!*rest) return true;
    const char* slash = strchr(rest, '/');
    if (!slash && !container) { more = emit(user, rest, summary ? summary : ""); return more; }
    // A directory: the first component, once.
    char name[48];
    usize clen = slash ? (usize)(slash - rest) : strlen(rest);
    if (clen >= sizeof name - 1) clen = sizeof name - 2;
    memcpy(name, rest, clen);
    name[clen] = '/';
    name[clen + 1] = 0;
    for (u32 i = 0; i < seen_count; ++i) if (strcmp(seen[i], name) == 0) return true;
    if (seen_count < 64) strcpy(seen[seen_count++], name);
    more = emit(user, name, slash ? "" : (summary ? summary : ""));
    return more;
}

void cli_dots_to_slashes(const char* name, char* out, usize cap) {
    usize i = 0;
    for (; name[i] && i + 1 < cap; ++i) out[i] = name[i] == '.' ? '/' : name[i];
    out[i] = 0;
}

void cli_slashes_to_dots(const char* name, char* out, usize cap) {
    usize i = 0;
    for (; name[i] && i + 1 < cap; ++i) out[i] = name[i] == '/' ? '.' : name[i];
    out[i] = 0;
}

} // namespace kv
