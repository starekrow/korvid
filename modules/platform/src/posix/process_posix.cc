#include "korvid/platform/process.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
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

i32 proc_read(KHandle& fd, void* buf, u32 cap) {
    if (!fd.is_open()) return -1;
    for (;;) {
        ssize_t n = read((int)fd.os(), buf, cap);
        if (n > 0) return (i32)n;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        close((int)fd.os()); // the end, or an error we can do nothing about
        fd = KHandle{};
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

bool Process::spawn(const ProcessDesc& desc) {
    close();
    error[0] = 0;
    if (KV_PROC_NONE) { format(error, sizeof error, "an iOS app cannot start another program"); return false; }
    if (desc.argv.len == 0 || !desc.argv.data[0]) { format(error, sizeof error, "nothing to run"); return false; }
    int out[2] = {-1, -1}, err[2] = {-1, -1}, fail[2] = {-1, -1};
    // With fork, a third pipe carries the child's errno if its exec fails; posix_spawn
    // returns that itself.
    if (!proc_pipe(out) || !proc_pipe(err) || (!KV_PROC_SPAWN && !proc_pipe(fail))) {
        format(error, sizeof error, "pipe: %s", strerror(errno));
        proc_close(out[0]); proc_close(out[1]); proc_close(err[0]); proc_close(err[1]);
        return false;
    }
    char** argv = (char**)mem_alloc(sizeof(char*) * (desc.argv.len + 1));
    for (u32 i = 0; i < desc.argv.len; ++i) argv[i] = (char*)desc.argv.data[i];
    argv[desc.argv.len] = nullptr;
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
        format(error, sizeof error, "%s: %s", desc.cwd && why == ENOENT && access(desc.cwd, F_OK) != 0 ? desc.cwd : desc.argv.data[0], strerror(why));
        return false;
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
        format(error, sizeof error, "fork: %s", strerror(fork_errno));
        proc_close(out[0]); proc_close(err[0]); proc_close(fail[0]);
        return false;
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
        format(error, sizeof error, "%s: %s", desc.cwd && why == ENOENT && access(desc.cwd, F_OK) != 0 ? desc.cwd : desc.argv.data[0], strerror(why));
        return false;
    }
#endif
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL, 0) | O_NONBLOCK);
    pid    = child;
    out_fd = khandle_make(KHandleKind::Pipe, out[0]);
    err_fd = khandle_make(KHandleKind::Pipe, err[0]);
    exited = false;
    code   = 0;
    return true;
}

void Process::wait(f64 seconds) {
    pollfd fds[2];
    nfds_t n = 0;
    if (out_fd.is_open()) fds[n++] = pollfd{(int)out_fd.os(), POLLIN, 0};
    if (err_fd.is_open()) fds[n++] = pollfd{(int)err_fd.os(), POLLIN, 0};
    if (!n) seconds = seconds < 0.001 ? seconds : 0.001;
    int ms = seconds <= 0 ? 0 : (int)(seconds * 1000.0 + 0.999);
    poll(n ? fds : nullptr, n, ms);
}

bool Process::running() {
    if (pid < 0 || exited) return false;
    int status = 0;
    pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r < 0 && errno == EINTR) return true;
    exited = true;
    if (r < 0) code = -1; // not ours to wait for any more: nothing to report
    else code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    return false;
}

i32 Process::read_out(void* buf, u32 cap) { return proc_read(out_fd, buf, cap); }
i32 Process::read_err(void* buf, u32 cap) { return proc_read(err_fd, buf, cap); }

void Process::kill() {
    if (pid < 0 || exited) return;
    if (::kill((pid_t)-pid, SIGKILL) != 0) ::kill((pid_t)pid, SIGKILL);
}

void Process::close() {
    if (pid >= 0 && !exited) {
        kill();
        int status;
        while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (out_fd.is_open()) close((int)out_fd.os());
    if (err_fd.is_open()) close((int)err_fd.os());
    pid = -1;
    out_fd = err_fd = KHandle{};
    exited = false;
    code = 0;
}

} // namespace kv
