// Korvid core: CLI commands and mounts, as a module declares them.
//
// A running game answers text commands, typed on stdin, sent to a TCP port (--cli-port N)
// or run from code (cli_run, korvid/cli/cli.h). The cli module owns the transports and
// the dispatch; this header is what any module needs to add commands of its own, and
// pieces of the CLI's filesystem.
//
// Commands. A module lists CliCommandDesc entries in its ModuleDesc::commands. A request
// goes to the command whose name matches the most of its first words, so "level load
// arena" runs "level load" with the one argument "arena". The command answers through
// its CliContext: ctx.print for each line of the answer, ctx.fail to say it failed and
// why; a session sees the answer end with "ok" or "err: <message>". Commands run in
// "cli.pump" (phase Input, on the main thread, paused ticks included) and must not
// block. After modules/level/src/level.cc:
//
//   void cmd_load(CliContext& ctx, Span<const char* const> args) {
//       if (args.len != 1) { ctx.fail("level load <name>"); return; }
//       if (!level_request(*ctx.engine, args[0])) {
//           ctx.fail("no level named '%s'", args[0]);
//           return;
//       }
//       ctx.print("loading '%s'", args[0]);
//   }
//   constinit const CliCommandDesc k_commands[] = {
//       {.name = "level load", .help = "<name>: go there", .fn = cmd_load},
//   };
//   // and in KV_MODULE_DEFINE: .commands = k_commands,
//
// Output is line-oriented and never blocks the game: a session that cannot drain loses
// lines and gets a drop marker. The second half of this header is the filesystem: what a
// module declares to show its state under `ls` and `print`, and to let `patch` change it.
// docs/design/cli.md has the line protocol and the transports.
#pragma once
#include "korvid/core/types.h"
#include <stdarg.h>

namespace kv {

struct Engine;

// The longest path, its terminator included: the size of a session's working directory
// and of the buffers the filesystem verbs resolve into.
constexpr u32 KV_CLI_PATH_MAX = 256;

// ---- commands -------------------------------------------------------------------------
// What a command or a mount's callback is handed: the engine, and the way to answer. The
// transport fills it in for one request.
struct CliContext {
    Engine* engine = nullptr; // the running engine
    void  (*sink)(void* user, const char* line) = nullptr; // one response line, without the newline
    void*   user   = nullptr; // the sink's
    // The session's working directory, a KV_CLI_PATH_MAX buffer; null when the request has
    // no session (cli_run), which paths read as "/".
    char*   cwd    = nullptr;
    bool    failed = false;   // set by fail()
    char    error[200] = {};  // fail()'s message

    // One response line, printf-style, without the newline; cut at 1023 bytes.
    void print(const char* fmt, ...) KV_PRINTF(2, 3);
    void vprint(const char* fmt, va_list args);
    // The request fails with this message; the transport reports it after the lines so far.
    // The first call's message stands; later calls change nothing.
    void fail(const char* fmt, ...) KV_PRINTF(2, 3);
};

// A command. `args` are the request's words after the command's name, double quotes
// grouping words and removed; they are valid during the call only.
typedef void (*CliCommandFn)(CliContext& ctx, Span<const char* const> args);

// One entry in a ModuleDesc::commands table.
struct CliCommandDesc {
    const char*  name = nullptr; // one or more words: "help", "level load"
    // One line, which `help` prints after the name: the arguments, then what it does
    // ("[N] let N sim ticks (1) through the pause").
    const char*  help = nullptr;
    CliCommandFn fn   = nullptr;
};

// Argument parsing for commands: false, with ctx failed and the argument named, on a bad
// value. `what` names it in the message ("count: 'x' is not a count"); a null `arg`, a
// missing argument, fails the same way. u32 takes decimal digits only.
bool cli_arg_f32(CliContext& ctx, const char* arg, const char* what, f32* out);
bool cli_arg_u32(CliContext& ctx, const char* arg, const char* what, u32* out);

// ---- the filesystem -------------------------------------------------------------------
// What the game has, as paths: `ls /` names the top level, `cd /kinds/rocks` and
// `ls` names that module's kinds, `print rock/3` describes an entity, `patch
// rock/3/Health/hp 1` writes a field. The tree is virtual: a module mounts a piece of it
// by declaring a CliMountDesc in its ModuleDesc::mounts, and answers for the names under
// it; the cli module owns the working directory, the path rules and the verbs. Names use
// '/' between components; engine names map their dots to slashes on the way in, so a
// module is a directory (`rocks.rock` is `/kinds/rocks/rock`).
//
// A mount answers three questions. list(dir) emits the children of one directory: `dir`
// is "" for the mount's own root, otherwise the relative directory with a trailing slash
// ("rocks/rock/"); a child whose name ends in '/' is a directory whose contents
// come on request, any other child is a leaf; the summary is one line and may be "".
// describe(rel) prints a node, leaf or directory, through ctx.print, or returns false
// for a name that is not there. patch(rel, values) sets a node from words, or fails
// with a reason; a mount without writable nodes leaves it null. A mount that keeps a flat
// list of names (paths with slashes in them) lists it through CliTree. From
// modules/anim/src/module.cc, `/clips`:
//
//   void clips_list(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) {
//       CliTree tree{dir, emit, user};
//       for (each live clip c)
//           if (!tree.add(c.path, false, summary)) return;
//   }
//   bool clips_describe(CliContext& ctx, const char* rel);  // prints the clip at `rel`
//   constinit const CliMountDesc k_mounts[] = {
//       {.path = "/clips", .list = clips_list, .describe = clips_describe},
//   };
//   // and in KV_MODULE_DEFINE: .mounts = k_mounts,

// Takes one child from list(): its `name`, with a trailing '/' for a directory, and a
// one-line `summary`. Returns false when the listing has enough (`ls` stops at its
// count); list() returns then.
typedef bool (*CliEmitFn)(void* user, const char* name, const char* summary); // false: enough, stop

// One entry in a ModuleDesc::mounts table: a top-level directory and its callbacks.
struct CliMountDesc {
    const char* path = nullptr; // "/materials": one component, with the leading slash
    // Emits the children of `dir`, relative to the mount, through `emit` and `user`.
    void (*list)(CliContext& ctx, const char* dir, CliEmitFn emit, void* user) = nullptr;
    // Prints the node at `rel` through ctx.print; false if there is none.
    bool (*describe)(CliContext& ctx, const char* rel) = nullptr;
    // Sets the node at `rel` from `values`; false, with ctx.fail, if it cannot. Null for a
    // mount with nothing writable.
    bool (*patch)(CliContext& ctx, const char* rel, Span<const char* const> values) = nullptr;
};

// Resolves `arg` against the context's working directory into a normalized absolute path
// ("." and ".." applied, repeated slashes folded, no trailing slash but for the root).
// "@name" and "@name/rest" start at the named entity's row under /kinds. False, with ctx
// failed, if the result would not fit `cap` bytes (pass KV_CLI_PATH_MAX) or no entity has
// the name.
bool cli_resolve(CliContext& ctx, const char* arg, char* out, usize cap);

// Help for a mount that keeps flat names with slashes in them (assets by path, engine
// names with dots turned to slashes): feeds every full name through and emits the unique
// children of `dir`, directories once. `container` says a full name that equals a child
// is itself a directory (a kind, whose rows are under it).
struct CliTree {
    const char* dir;   // as list() received it
    CliEmitFn   emit;  // list()'s, passed through
    void*       user;  // list()'s, passed through
    bool        more = true;      // false once the consumer said stop
    char        seen[64][48] = {}; // directories already emitted: the first 64, names cut at 46 bytes
    u32         seen_count = 0;   // entries of `seen` in use
    // Offers one full name: emits it, or its first directory once, if it is under `dir`.
    // Returns `more`.
    bool add(const char* full_name, bool container, const char* summary);
};

// Engine names in the tree: "rocks.rock" <-> "rocks/rock". Both cut at `cap` - 1 bytes.
void cli_dots_to_slashes(const char* name, char* out, usize cap);
void cli_slashes_to_dots(const char* name, char* out, usize cap);

} // namespace kv
