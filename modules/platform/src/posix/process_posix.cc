#include "korvid/platform/process.h"
#include "korvid/core/os_error.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include "../os_err.h"
#include "../misuse.h"
#include "../os_handle_table.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/event.h> // kqueue: a child's exit as a descriptor
#elif defined(__linux__)
#include <sys/syscall.h> // pidfd_open
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434 // the same on every architecture; older headers lack the name
#endif
#endif

// Linux (glibc) and macOS start a child with posix_spawn: a vfork underneath, so the
// parent's page tables are not copied and its pages do not turn copy-on-write. With fork,
// a tool that starts thousands of children (goldberg's converters) paid for its own size
// on every one, on its main thread, and again in faults afterwards. iOS and Android keep
// fork: posix_spawn's chdir action is not there at the API levels the engine targets.
#if (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && TARGET_OS_OSX)
#define KV_PROC_SPAWN 1
#include <spawn.h>
#else
#define KV_PROC_SPAWN 0
#endif

extern char** environ;

namespace kv {

namespace {

// Both ends close on exec (the child gets its ends by dup2, which drops the flag).
bool proc_pipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0; // one call, and no window for another thread's exec
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

void proc_close(int& fd) { if (fd >= 0) close(fd); fd = -1; }

// A descriptor that turns readable when `child` exits, for a waiting loop's watch
// (PlatformWatch::add(Process)): a pidfd on Linux and Android (kernel 5.3 or later), a
// kqueue holding the child's NOTE_EXIT on Apple. Nothing ever reads it: once the child has
// gone it stays readable, and the watch hears it once. -1 where there is none (an older
// kernel), and the exit then wakes nothing; the end of the child's pipes, which nearly
// always comes with it, still does.
int proc_exit_fd(pid_t child) {
#if defined(__APPLE__)
    int kq = kqueue();
    if (kq < 0) return -1;
    struct kevent ev;
    EV_SET(&ev, child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(kq, &ev, 1, nullptr, 0, nullptr) != 0) { close(kq); return -1; } // ESRCH: gone already, and its pipes have said so
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    return kq;
#elif defined(__linux__)
    long fd = syscall(SYS_pidfd_open, child, 0); // close-on-exec from the start
    return fd < 0 ? -1 : (int)fd;
#else
    (void)child;
    return -1;
#endif
}

i32 proc_read(Handle& fd, void* buf, u32 cap) {
    if (!os_handle_open(fd)) return -1;
    for (;;) {
        ssize_t n = read((int)os_handle_value(fd), buf, cap);
        if (n > 0) { os_handle_count(fd, (u64)n, 0); return (i32)n; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        i64 v = os_handle_release(fd); // the end, or an error we can do nothing about: stale, then closed
        if (v >= 0) close((int)v);
        fd = Handle{};
        return -1;
    }
}

// The parent's environment with `extra` laid over it, as one allocation: the pointer
// table, then the new strings. Built before the fork, since the child may not allocate.
char** proc_env(Span<const char* const> extra) {
    u32 base = 0;
    while (environ && environ[base]) ++base;
    char** out = (char**)mem_alloc(sizeof(char*) * (base + extra.len + 1));
    u32 n = 0;
    for (u32 i = 0; i < base; ++i) {
        const char* eq = strchr(environ[i], '=');
        usize kl = eq ? (usize)(eq - environ[i]) + 1 : strlen(environ[i]);
        bool overridden = false;
        for (u32 j = 0; j < extra.len && !overridden; ++j) overridden = strncmp(extra.data[j], environ[i], kl) == 0;
        if (!overridden) out[n++] = environ[i];
    }
    for (u32 j = 0; j < extra.len; ++j) out[n++] = (char*)extra.data[j];
    out[n] = nullptr;
    return out;
}

} // namespace

// An iOS app is sandboxed: it may not start another program (there is no shell to start,
// and fork is refused on a device). Said, rather than tried.
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define KV_PROC_NONE 1
#else
#define KV_PROC_NONE 0
#endif

bool process_supported() { return !KV_PROC_NONE; }

bool Process::spawn(const ProcessDesc& desc, i32* err) {
    close();
    error[0] = 0;
    if (KV_PROC_NONE) { format(error, sizeof error, "an iOS app cannot start another program"); return os_fail(err, ENOTSUP); }
    bool has_argv = desc.argv.len != 0 && desc.argv.data[0];
    if (!desc.shell && !has_argv) { format(error, sizeof error, "nothing to run"); return os_fail(err, EINVAL); }
    // A line for the shell is sh's to read: /bin/sh -c <line>, the line one argument, as
    // written. Else argv is argv.
    const char* const shell_argv[] = {"/bin/sh", "-c", desc.shell};
    Span<const char* const> words = desc.shell ? Span<const char* const>(shell_argv, 3) : desc.argv;
    const char* what = desc.shell ? desc.shell : desc.argv.data[0]; // what a failure names
    int out[2] = {-1, -1}, err[2] = {-1, -1}, fail[2] = {-1, -1};
    // With fork, a third pipe carries the child's errno if its exec fails; posix_spawn
    // returns that itself.
    if (!proc_pipe(out) || !proc_pipe(err) || (!KV_PROC_SPAWN && !proc_pipe(fail))) {
        int e = errno;
        format(error, sizeof error, "pipe: %s", os_error_text(e));
        proc_close(out[0]); proc_close(out[1]); proc_close(err[0]); proc_close(err[1]);
        return os_fail(err, e);
    }
    char** argv = (char**)mem_alloc(sizeof(char*) * (words.len + 1));
    for (u32 i = 0; i < words.len; ++i) argv[i] = (char*)words.data[i];
    argv[words.len] = nullptr;
    char** envp = proc_env(desc.env);

#if KV_PROC_SPAWN
    posix_spawn_file_actions_t acts;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&acts);
    posix_spawnattr_init(&attr);
    // Its own process group, so kill() takes what it starts with it; stdin empty; the
    // pipes as stdout and stderr (dup2 drops their close-on-exec); then its directory.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawn_file_actions_addopen(&acts, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&acts, out[1], 1);
    posix_spawn_file_actions_adddup2(&acts, err[1], 2);
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (desc.cwd) posix_spawn_file_actions_addchdir_np(&acts, desc.cwd);
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
    pid_t child = -1;
    int why = posix_spawnp(&child, argv[0], &acts, &attr, argv, envp);
    posix_spawn_file_actions_destroy(&acts);
    posix_spawnattr_destroy(&attr);
    mem_free(argv);
    mem_free(envp);
    proc_close(out[1]); proc_close(err[1]);
    if (why != 0) {
        proc_close(out[0]); proc_close(err[0]);
        format(error, sizeof error, "%s: %s", desc.cwd && why == ENOENT && access(desc.cwd, F_OK) != 0 ? desc.cwd : what, os_error_text(why));
        return os_fail(err, why);
    }
#else
    pid_t child = fork();
    if (child == 0) {
        // Only what is safe between fork and exec in a threaded program: no allocation,
        // no locks, nothing of the engine's.
        setpgid(0, 0);
        int null_in = open("/dev/null", O_RDONLY);
        if (null_in >= 0) dup2(null_in, 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        int why = 0;
        if (desc.cwd && chdir(desc.cwd) != 0) why = errno;
        if (!why) {
            environ = envp;
            execvp(argv[0], argv);
            why = errno;
        }
        ssize_t ignored = write(fail[1], &why, sizeof why);
        (void)ignored;
        _exit(127);
    }
    int fork_errno = errno;
    mem_free(argv);
    mem_free(envp);
    proc_close(out[1]); proc_close(err[1]); proc_close(fail[1]);
    if (child < 0) {
        format(error, sizeof error, "fork: %s", os_error_text(fork_errno));
        proc_close(out[0]); proc_close(err[0]); proc_close(fail[0]);
        return os_fail(err, fork_errno);
    }
    setpgid(child, child); // both sides set it, so it is set whichever runs first
    // The exec closes the failure pipe; a read that returns bytes is the child saying why
    // it could not. This waits only for the exec, which is as long as a spawn takes.
    int why = 0;
    ssize_t got;
    do got = read(fail[0], &why, sizeof why); while (got < 0 && errno == EINTR);
    proc_close(fail[0]);
    if (got == (ssize_t)sizeof why) {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        proc_close(out[0]); proc_close(err[0]);
        format(error, sizeof error, "%s: %s", desc.cwd && why == ENOENT && access(desc.cwd, F_OK) != 0 ? desc.cwd : what, os_error_text(why));
        return os_fail(err, why);
    }
#endif
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL, 0) | O_NONBLOCK);
    pid    = child;
    char label[160];
    format(label, sizeof label, "stdout of %s", what);
    out_fd = os_handle_make(OsKind::Pipe, out[0], label);
    format(label, sizeof label, "stderr of %s", what);
    err_fd = os_handle_make(OsKind::Pipe, err[0], label);
    format(label, sizeof label, "the exit of %s", what);
    process = os_handle_make(OsKind::Process, proc_exit_fd(child), label); // null where the OS gives none
    exited = false;
    code   = 0;
    return true;
}

void Process::wait(f64 seconds) {
    if (pid < 0) misuse_process(*this, "wait");
    pollfd fds[3];
    nfds_t n = 0;
    if (os_handle_open(out_fd)) fds[n++] = pollfd{(int)os_handle_value(out_fd), POLLIN, 0};
    if (os_handle_open(err_fd)) fds[n++] = pollfd{(int)os_handle_value(err_fd), POLLIN, 0};
    // The exit too, where the OS gives it as a descriptor: with both streams ended the wait
    // is for it alone. Without one, a millisecond at most, since only running() can tell.
    bool exit_heard = !exited && os_handle_open(process);
    if (exit_heard) fds[n++] = pollfd{(int)os_handle_value(process), POLLIN, 0};
    if (!n || (!exit_heard && !os_handle_open(out_fd) && !os_handle_open(err_fd))) seconds = seconds < 0.001 ? seconds : 0.001;
    int ms = seconds <= 0 ? 0 : (int)(seconds * 1000.0 + 0.999);
    poll(n ? fds : nullptr, n, ms);
}

bool Process::running() {
    if (pid < 0) { misuse_process(*this, "running"); return false; }
    if (exited) return false;
    int status = 0;
    pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r < 0 && errno == EINTR) return true;
    exited = true;
    if (r < 0) code = -1; // not ours to wait for any more: nothing to report
    else code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return false;
}

i32 Process::read_out(void* buf, u32 cap) {
    if (pid < 0) misuse_process(*this, "read_out");
    return proc_read(out_fd, buf, cap);
}
i32 Process::read_err(void* buf, u32 cap) {
    if (pid < 0) misuse_process(*this, "read_err");
    return proc_read(err_fd, buf, cap);
}

void Process::kill() {
    if (pid < 0) { misuse_process(*this, "kill"); return; }
    if (exited) return;
    if (::kill((pid_t)-pid, SIGKILL) != 0) ::kill((pid_t)pid, SIGKILL);
}

void Process::close() {
    if (pid >= 0 && !exited) {
        kill();
        int status;
        while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR) {}
    }
    i64 v = os_handle_release(out_fd); // stale, then closed
    if (v >= 0) ::close((int)v);
    v = os_handle_release(err_fd);
    if (v >= 0) ::close((int)v);
    pid = -1;
    v = os_handle_release(process); // the exit's descriptor, where there was one
    if (v >= 0) ::close((int)v);
    out_fd = err_fd = process = Handle{};
    exited = false;
    code = 0;
    warned = false;
}

} // namespace kv
