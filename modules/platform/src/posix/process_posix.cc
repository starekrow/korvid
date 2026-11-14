#include "korvid/platform/process.h"
#include "korvid/core/format.h"
#include "korvid/core/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace kv {

namespace {

bool proc_pipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void proc_close(int& fd) { if (fd >= 0) close(fd); fd = -1; }

i32 proc_read(i64& fd, void* buf, u32 cap) {
    if (fd < 0) return -1;
    for (;;) {
        ssize_t n = read((int)fd, buf, cap);
        if (n > 0) return (i32)n;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        close((int)fd); // the end, or an error we can do nothing about
        fd = -1;
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

bool process_supported() { return true; }

bool Process::spawn(const ProcessDesc& desc) {
    free();
    error[0] = 0;
    if (desc.argv.len == 0 || !desc.argv.data[0]) { format(error, sizeof error, "nothing to run"); return false; }
    int out[2] = {-1, -1}, err[2] = {-1, -1}, fail[2] = {-1, -1};
    if (!proc_pipe(out) || !proc_pipe(err) || !proc_pipe(fail)) {
        format(error, sizeof error, "pipe: %s", strerror(errno));
        proc_close(out[0]); proc_close(out[1]); proc_close(err[0]); proc_close(err[1]);
        return false;
    }
    char** argv = (char**)mem_alloc(sizeof(char*) * (desc.argv.len + 1));
    for (u32 i = 0; i < desc.argv.len; ++i) argv[i] = (char*)desc.argv.data[i];
    argv[desc.argv.len] = nullptr;
    char** envp = proc_env(desc.env);

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
    fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL, 0) | O_NONBLOCK);
    pid    = child;
    out_fd = out[0];
    err_fd = err[0];
    exited = false;
    code   = 0;
    return true;
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

void Process::free() {
    if (pid >= 0 && !exited) {
        kill();
        int status;
        while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (out_fd >= 0) close((int)out_fd);
    if (err_fd >= 0) close((int)err_fd);
    pid = out_fd = err_fd = -1;
    exited = false;
    code = 0;
}

} // namespace kv
