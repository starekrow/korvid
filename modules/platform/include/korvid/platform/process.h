// Korvid platform: child processes. Spawn one with an argument vector, poll it, read what
// it writes, kill it. Nothing blocks: whoever owns a process polls it once a tick, as
// with a socket. For tools (goldberg's converters); a game has no business here.
//
// The child's stdin is empty. Its stdout and stderr are pipes the owner must drain: a
// child that fills one waits until it is read. It runs in a process group of its own, so
// kill() takes what it started with it (a shell script's `sleep`).
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct ProcessDesc {
    Span<const char* const> argv;          // argv[0] is the program: a path, or a name found on PATH
    const char*             cwd = nullptr; // null: the parent's
    Span<const char* const> env;           // "NAME=value", added to (and overriding) the parent's
};

struct Process {
    i64  pid = -1;               // the OS's id while it is known; -1 when there is no child
    i64  out_fd = -1, err_fd = -1;
    bool exited = false;
    i32  code = 0;               // once exited: its status, or 128 + the signal that ended it
    char error[160] = {};        // why spawn failed

    // False with `error` set if the program cannot be started (not found, not executable,
    // no such directory). Nothing is left to free then.
    bool spawn(const ProcessDesc& desc);
    bool is_open() const { return pid >= 0; }
    // Asks the OS once; true until the child has ended. Output may remain to be read
    // after it has: read until both streams return -1.
    bool running();
    // Bytes read, 0 when nothing has arrived, -1 at the end of the stream.
    i32  read_out(void* buf, u32 cap);
    i32  read_err(void* buf, u32 cap);
    // Ends the child and everything in its group, now. running() turns false soon after.
    void kill();
    // Closes the pipes and forgets the child. One still running is killed first and
    // waited for.
    void free();
};

// True where spawn is implemented.
bool process_supported();

} // namespace kv
