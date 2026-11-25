// Korvid platform: child processes. Spawn one with an argument vector, poll it, read what
// it writes, kill it. Nothing blocks: whoever owns a process polls it once a tick, as
// with a socket. For tools (goldberg's converters); a game has no business here.
//
// The child's stdin is empty. Its stdout and stderr are pipes the owner must drain: a
// child that fills one waits until it is read. It runs in a process group of its own (a
// job object on Windows), so kill() takes what it started with it (a shell script's
// `sleep`), and reports 128 + 9 as a SIGKILL does.
//
// Two ways to say what to run, and no flags to juggle between them:
// - spawn: an argument vector. argv is argv on every OS: each word reaches the program as
//   it is given (on Windows, quoted as the C runtime reads a command line back). Nothing
//   is a shell's to interpret, so a shell's builtins and operators mean nothing here.
// - shell: one line for the platform's non-interactive shell, by that shell's own rules:
//   `/bin/sh -c <line>` on POSIX, `%ComSpec% /d /s /c "<line>"` (cmd.exe) on Windows.
//   Quoting, builtins (`echo`, `dir`), `&&` and redirection are the shell's, so the line is
//   that OS's to write: a line that must run everywhere is a script, started with spawn.
#pragma once
#include "korvid/core/types.h"
#include "korvid/platform/os_handle.h"

namespace kv {

struct ProcessDesc {
    Span<const char* const> argv;          // argv[0] is the program: a path, or a name found on PATH
    const char*             cwd = nullptr; // null: the parent's
    Span<const char* const> env;           // "NAME=value", added to (and overriding) the parent's
    const char*             shell = nullptr; // a line for the platform's shell, run instead of argv: Process::shell sets it
};

struct Process {
    i64  pid = -1;               // the OS's id while it is known; -1 when there is no child
    Handle out_fd, err_fd;     // OsKind::Pipe: the read ends of its stdout and stderr
    bool exited = false;
    i32  code = 0;               // once exited: its status, or 128 + the signal that ended it
    char error[160] = {};        // why spawn failed
    Handle process, group;      // the platform's own hold on the child and on what it started, where the OS gives one beyond the id

    // False with `error` set if the program cannot be started (not found, not executable,
    // no such directory), and *err (if given) its errno: ENOENT, EACCES, EINVAL for nothing
    // to run, ENOTSUP where spawn is not implemented. `error` says what an errno cannot:
    // whose name was not found. Nothing is left to free then.
    bool spawn(const ProcessDesc& desc, i32* err = nullptr);
    // The same for one line of the platform's shell (above): desc gives the directory and
    // the environment, and its argv is not used. The shell starting is all that true
    // means: a line it cannot run is the shell's exit code and its stderr.
    bool shell(const char* line, const ProcessDesc& desc = ProcessDesc{}, i32* err = nullptr) {
        ProcessDesc d = desc;
        d.shell = line;
        return spawn(d, err);
    }
    bool is_open() const { return pid >= 0; }
    // Asks the OS once; true until the child has ended. Output may remain to be read
    // after it has: read until both streams return -1.
    bool running();
    // Blocks until either stream has something (or has ended), or `seconds` pass: for a
    // thread that owns the process and has nothing else to do (an IoQueue task). With both
    // streams ended, it waits a millisecond at most, since only running() can tell the end.
    void wait(f64 seconds);
    // Bytes read, 0 when nothing has arrived, -1 at the end of the stream.
    i32  read_out(void* buf, u32 cap);
    i32  read_err(void* buf, u32 cap);
    // Ends the child and everything in its group, now. running() turns false soon after.
    void kill();
    // Closes the pipes and forgets the child. One still running is killed first and
    // waited for.
    void close();
};

// True where spawn is implemented.
bool process_supported();

} // namespace kv
