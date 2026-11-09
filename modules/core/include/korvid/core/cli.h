// Korvid core: the CLI as the registry sees it. A module declares commands as data in its
// ModuleDesc; a transport (the cli module) reads lines, finds the command whose name is
// the longest prefix of the line's words, and calls it with the rest as arguments. A
// command answers through the context, one line at a time, and marks a request failed
// with fail(); the transport frames the answer for whoever asked. Output is ASCII and
// line-oriented, never blocking: a session that cannot drain gets a drop marker.
//
//   void spawn_wave(CliContext& ctx, Span<const char* const> args);
//   constinit const CliCommandDesc k_commands[] = {
//       {.name = "spawn wave", .help = "<count>: a wave of rocks now", .fn = spawn_wave},
//   };
#pragma once
#include "korvid/core/types.h"
#include <stdarg.h>

namespace kv {

struct Engine;

constexpr u32 KV_CLI_PATH_MAX = 256;

struct CliContext {
    Engine* engine = nullptr;
    void  (*sink)(void* user, const char* line) = nullptr; // one response line, without the newline
    void*   user   = nullptr;
    char*   cwd    = nullptr; // the session's working directory in the filesystem below; "/" when there is no session
    bool    failed = false;
    char    error[200] = {};

    // One response line.
    void print(const char* fmt, ...) KV_PRINTF(2, 3);
    void vprint(const char* fmt, va_list args);
    // The request fails with this message; the transport reports it after the lines so far.
    void fail(const char* fmt, ...) KV_PRINTF(2, 3);
};

typedef void (*CliCommandFn)(CliContext& ctx, Span<const char* const> args);

struct CliCommandDesc {
    const char*  name = nullptr; // one or more words: "help", "spawn wave"
    const char*  help = nullptr; // one line: arguments and what it does
    CliCommandFn fn   = nullptr;
};

// Argument parsing for commands: false, with ctx failed and the argument named, on a bad value.
bool cli_arg_f32(CliContext& ctx, const char* arg, const char* what, f32* out);
bool cli_arg_u32(CliContext& ctx, const char* arg, const char* what, u32* out);

// ---- the filesystem ---------------------------------------------------------------------
// What the game has, as paths: `ls /` names the top level, `cd /kinds/example_rocks` and
// `ls` names that module's kinds, `print rock/3` describes an entity, `patch
// rock/3/Health/hp 1` writes a field. The tree is virtual: a module mounts a piece of it
// by declaring a CliMountDesc in its ModuleDesc::mounts, and answers for the names under
// it; the cli module owns the working directory, the path rules and the verbs. Names use
// '/' between components; engine names map their dots to slashes on the way in, so a
// module is a directory (`example_rocks.rock` is `/kinds/example_rocks/rock`).
//
// A mount answers three questions. list(dir) emits the children of one directory: `dir`
// is "" for the mount's own root, otherwise the relative directory with a trailing slash
// ("example_rocks/rock/"); a child whose name ends in '/' is a directory whose contents
// come on request, any other child is a leaf; the summary is one line and may be "".
// describe(rel) prints a node, leaf or directory, through ctx.print, or returns false
// for a name that is not there. patch(rel, values) sets a node from words, or fails
// with a reason; a mount without writable nodes leaves it null.
typedef bool (*CliEmitFn)(void* user, const char* name, const char* summary); // false: enough, stop

struct CliMountDesc {
    const char* path = nullptr; // "/materials": one component, with the leading slash
    void (*list)(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) = nullptr;
    bool (*describe)(CliContext& ctx, const char* rel) = nullptr;
    bool (*patch)(CliContext& ctx, const char* rel, Span<const char* const> values) = nullptr;
};

// Resolves `arg` against the context's working directory into a normalized absolute path
// ("." and ".." applied, repeated slashes folded, no trailing slash but for the root).
// False, with ctx failed, if the result would not fit KV_CLI_PATH_MAX.
bool cli_resolve(CliContext& ctx, const char* arg, char* out, usize cap);

// Help for a mount that keeps flat names with slashes in them (assets by path, engine
// names with dots turned to slashes): feeds every full name through and emits the unique
// children of `dir`, directories once. `container` says a full name that equals a child
// is itself a directory (a kind, whose rows are under it).
struct CliTree {
    const char* dir;   // as list() received it
    CliEmitFn   emit;
    void*       user;
    bool        more = true;      // false once the consumer said stop
    char        seen[64][48] = {}; // directories already emitted; enough for a listing
    u32         seen_count = 0;
    // Offers one full name; returns `more`.
    bool add(const char* full_name, bool container, const char* summary);
};

// Engine names in the tree: "example_rocks.rock" <-> "example_rocks/rock".
void cli_dots_to_slashes(const char* name, char* out, usize cap);
void cli_slashes_to_dots(const char* name, char* out, usize cap);

} // namespace kv
