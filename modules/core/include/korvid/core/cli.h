// Korvid core: the CLI as the registry sees it. A module declares commands as data in its
// ModuleDesc; a transport (the cli module) reads lines, finds the command whose name is
// the longest prefix of the line's words, and calls it with the rest as arguments. A
// command answers through the context, one line at a time, and marks a request failed
// with fail(); the transport frames the answer for whoever asked. Output is ASCII and
// line-oriented, never blocking: a session that cannot drain gets a drop marker.
//
//   void ls_materials(CliContext& ctx, Span<const char* const> args);
//   constinit const CliCommandDesc k_commands[] = {
//       {.name = "ls materials", .help = "list the loaded material assets", .fn = ls_materials},
//   };
#pragma once
#include "korvid/core/types.h"
#include <stdarg.h>

namespace kv {

struct Engine;

struct CliContext {
    Engine* engine = nullptr;
    void  (*sink)(void* user, const char* line) = nullptr; // one response line, without the newline
    void*   user   = nullptr;
    bool    failed = false;
    char    error[200] = {};

    // One response line.
    void print(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void vprint(const char* fmt, va_list args);
    // The request fails with this message; the transport reports it after the lines so far.
    void fail(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

typedef void (*CliCommandFn)(CliContext& ctx, Span<const char* const> args);

struct CliCommandDesc {
    const char*  name = nullptr; // one or more words: "help", "ls materials"
    const char*  help = nullptr; // one line: arguments and what it does
    CliCommandFn fn   = nullptr;
};

// Argument parsing for commands: false, with ctx failed and the argument named, on a bad value.
bool cli_arg_f32(CliContext& ctx, const char* arg, const char* what, f32* out);
bool cli_arg_u32(CliContext& ctx, const char* arg, const char* what, u32* out);

} // namespace kv
