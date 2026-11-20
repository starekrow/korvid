// Korvid cli: what the module's sources share. The state, the mounts, and the verbs.
#pragma once
#include "korvid/cli/module.h"
#include "korvid/core/array.h"
#include "korvid/core/cli.h"
#include "korvid/platform/net.h"

namespace kv {

constexpr u32 kLineMax     = 1024;
constexpr u32 kMaxArgs     = 32;
constexpr u32 kMaxSessions = 16;

// One connection, or stdin.
struct Session {
    NetSocket sock;          // closed for the stdin session
    bool      is_stdin = false;
    bool      ascii    = false; // escape on the way out, unescape on the way in
    bool      stdin_eof = false;
    char      line[kLineMax];
    u32       line_len = 0;
    bool      overflow = false; // the current line passed kLineMax: dropped at its end
    bool      dropped  = false; // output was dropped: say so once it drains
    char      cwd[KV_CLI_PATH_MAX] = "/";
};

struct CliState {
    CliConfig   config;
    NetListener listener;
    Session     sessions[kMaxSessions];
    u32         session_count = 0;
    Array<const CliCommandDesc*> commands; // every module's, in module order
    Array<const CliMountDesc*>   mounts;   // likewise
    u64         requests = 0, failed = 0, dropped = 0;
};

CliState* cli_state(Engine& e);

// The filesystem verbs (fs.cc).
void cli_fs_ls(CliContext& ctx, Span<const char* const> args);
void cli_fs_cd(CliContext& ctx, Span<const char* const> args);
void cli_fs_pwd(CliContext& ctx, Span<const char* const> args);
void cli_fs_print(CliContext& ctx, Span<const char* const> args);
void cli_fs_patch(CliContext& ctx, Span<const char* const> args);

// The engine's own mounts (mounts.cc): ktables, systems, handlers, resources, events,
// triggers, clock, modules, mem, perf.
extern const CliMountDesc k_engine_mounts[11];

} // namespace kv
