#include "korvid/platform/process.h"
#include "korvid/platform/io.h"
#include "korvid/core/array.h"
#include "korvid/core/test.h"
#include "korvid/core/time.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>

namespace {

using namespace kv;

// Runs a process to its end, collecting both streams. False if it outlives the budget.
bool run_out(Process& p, Array<char>& out, Array<char>& err, f64 budget = 10.0) {
    f64 start = time_now();
    bool out_open = true, err_open = true;
    char buf[4096];
    while (out_open || err_open || p.running()) {
        bool any = false;
        if (out_open) { i32 n = p.read_out(buf, sizeof buf); if (n > 0) { memcpy(out.push_n((u32)n), buf, (usize)n); any = true; } else if (n < 0) out_open = false; }
        if (err_open) { i32 n = p.read_err(buf, sizeof buf); if (n > 0) { memcpy(err.push_n((u32)n), buf, (usize)n); any = true; } else if (n < 0) err_open = false; }
        if (time_now() - start > budget) return false;
        if (!any) time_sleep(0.001);
    }
    out.push(0);
    err.push(0);
    return true;
}

bool sh(Process& p, const char* script, const char* cwd = nullptr, Span<const char* const> env = {}) {
    const char* argv[] = {"sh", "-c", script};
    ProcessDesc d;
    d.argv = Span<const char* const>(argv, 3);
    d.cwd  = cwd;
    d.env  = env;
    return p.spawn(d);
}

} // namespace

void test_process(kv::TestCtx& c) {
    using namespace kv;
    c.begin("process");
    if (!process_supported()) {
        Process p;
        KV_CHECK(c, !sh(p, "true") && p.error[0] && !p.is_open());
        return;
    }
    Array<char> out{}, err{};
    Process p;

    // Both streams and the status.
    KV_CHECK(c, sh(p, "echo to-out; echo to-err >&2; exit 3") && p.is_open());
    KV_CHECK(c, run_out(p, out, err) && p.exited && p.code == 3);
    KV_CHECK(c, strcmp(out.data, "to-out\n") == 0 && strcmp(err.data, "to-err\n") == 0);
    p.close();
    KV_CHECK(c, !p.is_open() && !p.running() && p.warned == (KV_DEV != 0)); // used after close: told, in dev and debug

    // A program found on PATH, with arguments passed as they are (no shell between).
    out.len = err.len = 0;
    const char* echo[] = {"echo", "a b", "$HOME", "*"};
    ProcessDesc d;
    d.argv = Span<const char* const>(echo, 4);
    KV_CHECK(c, p.spawn(d) && run_out(p, out, err) && p.code == 0 && strcmp(out.data, "a b $HOME *\n") == 0);
    p.close();

    // The working directory and the environment: added to the parent's, and over it.
    char dir[512];
    KV_CHECK(c, io_make_temp_dir("kv-process", dir, sizeof dir));
    out.len = err.len = 0;
    const char* env[] = {"KV_TEST_ONE=1", "KV_TEST_TWO=two words", "PATH=/usr/bin:/bin"};
    KV_CHECK(c, sh(p, "basename \"$PWD\"; echo \"$KV_TEST_ONE/$KV_TEST_TWO/$PATH\"; test -n \"$HOME\" && echo home", dir, Span<const char* const>(env, 3)));
    KV_CHECK(c, run_out(p, out, err) && p.code == 0);
    const char* base = strrchr(dir, '/') + 1;
    KV_CHECK(c, strncmp(out.data, base, strlen(base)) == 0 && strstr(out.data, "\n1/two words//usr/bin:/bin\n") && strstr(out.data, "home\n"));
    p.close();

    // Stdin is empty, not the parent's.
    out.len = err.len = 0;
    KV_CHECK(c, sh(p, "cat; echo done") && run_out(p, out, err) && strcmp(out.data, "done\n") == 0);
    p.close();

    // What cannot start says why, and leaves nothing behind.
    const char* nope[] = {"kv-no-such-program-anywhere"};
    d = ProcessDesc{};
    d.argv = Span<const char* const>(nope, 1);
    KV_CHECK(c, !p.spawn(d) && !p.is_open() && strstr(p.error, "kv-no-such-program-anywhere"));
    KV_CHECK(c, !sh(p, "true", "/no/such/directory/anywhere") && strstr(p.error, "/no/such/directory/anywhere"));
    d = ProcessDesc{};
    KV_CHECK(c, !p.spawn(d) && p.error[0]);

    // Polling does not block; a long output does not wedge the child.
    KV_CHECK(c, sh(p, "sleep 0.2; echo late"));
    f64 t0 = time_now();
    KV_CHECK(c, p.running() && time_now() - t0 < 0.1);
    char one[8];
    KV_CHECK(c, p.read_out(one, sizeof one) == 0);
    out.len = err.len = 0;
    KV_CHECK(c, run_out(p, out, err) && strcmp(out.data, "late\n") == 0);
    p.close();
    out.len = err.len = 0;
    KV_CHECK(c, sh(p, "i=0; while [ $i -lt 20000 ]; do echo line-$i; i=$((i+1)); done; echo tail >&2"));
    KV_CHECK(c, run_out(p, out, err, 30.0) && p.code == 0 && out.len > 200000 && strstr(out.data, "line-19999\n") && strcmp(err.data, "tail\n") == 0);
    p.close();

    // Killed, with what it started.
    char marker[600];
    snprintf(marker, sizeof marker, "%s/alive", dir);
    char script[900];
    snprintf(script, sizeof script, "(sleep 0.4; echo x > '%s') & sleep 30", marker);
    KV_CHECK(c, sh(p, script) && p.running());
    p.kill();
    bool ended = false;
    for (u32 i = 0; i < 2000 && !ended; ++i) { ended = !p.running(); if (!ended) time_sleep(0.001); }
    KV_CHECK(c, ended && p.code == 128 + 9);
    p.close();
    time_sleep(0.6);
    KV_CHECK(c, !io_is_file(marker)); // the background sleep died with its group

    // free() on one still running ends it.
    KV_CHECK(c, sh(p, "sleep 30") && p.running());
    t0 = time_now();
    p.close();
    KV_CHECK(c, !p.is_open() && time_now() - t0 < 2.0);

#if defined(_WIN32)
    // A batch file is the shell's to run, on a line written by the shell's rules: its
    // arguments arrive as they were given, the last one's closing quote kept (cmd strips
    // the line's last quote unless it is asked with /s, which shell does), an & inside its
    // argument.
    char bat[600], bat_line[900];
    snprintf(bat, sizeof bat, "%s/args.bat", dir);
    const char* args_bat = "@echo off\r\necho 1=%1 2=%2\r\n";
    KV_CHECK(c, io_write_file(bat, args_bat, strlen(args_bat)));
    snprintf(bat_line, sizeof bat_line, "\"%s\" \"salt & pepper\" \"two\"", bat);
    out.len = err.len = 0;
    KV_CHECK(c, p.shell(bat_line) && run_out(p, out, err) && p.code == 0 && strcmp(out.data, "1=\"salt & pepper\" 2=\"two\"\r\n") == 0);
    p.close();
    // The shell's own words work in a line, and mean nothing in an argument vector: cmd's
    // builtin echo, two commands joined, an exit code.
    out.len = err.len = 0;
    KV_CHECK(c, p.shell("echo one&& echo two& exit 7") && run_out(p, out, err) && p.code == 7 && strcmp(out.data, "one\r\ntwo\r\n") == 0);
    p.close();
#else
    out.len = err.len = 0;
    KV_CHECK(c, p.shell("echo one && echo \"two  words\"; exit 7") && run_out(p, out, err) && p.code == 7 && strcmp(out.data, "one\ntwo  words\n") == 0);
    p.close();
#endif
    // The shell is given the directory and the environment as spawn is, and a failure's errno.
    {
        ProcessDesc sd;
        const char* envs[] = {"KV_SHELL_TEST=from the parent"};
        sd.env = Span<const char* const>(envs, 1);
        sd.cwd = dir;
        out.len = err.len = 0;
#if defined(_WIN32)
        KV_CHECK(c, p.shell("echo %KV_SHELL_TEST%", sd) && run_out(p, out, err) && strcmp(out.data, "from the parent\r\n") == 0);
#else
        KV_CHECK(c, p.shell("echo \"$KV_SHELL_TEST\"", sd) && run_out(p, out, err) && strcmp(out.data, "from the parent\n") == 0);
#endif
        p.close();
        i32 why = 0;
        const char* none[] = {"kv-no-such-program-anywhere"};
        ProcessDesc nd;
        nd.argv = Span<const char* const>(none, 1);
        KV_CHECK(c, !p.spawn(nd, &why) && why == ENOENT && p.error[0]);
        // Used anyway, as code that does not look would: an empty child that succeeded,
        // and in a dev or debug build a warning with spawn's reason, once.
        char none_buf[16];
        KV_CHECK(c, !p.running() && p.read_out(none_buf, sizeof none_buf) == -1 && p.code == 0);
        KV_CHECK(c, p.warned == (KV_DEV != 0));
        why = 0;
        KV_CHECK(c, !p.spawn(ProcessDesc{}, &why) && why == EINVAL);
    }
    // wait: returns when there is output, not at its limit; with nothing, at its limit.
    KV_CHECK(c, sh(p, "sleep 0.2; echo late"));
    t0 = time_now();
    p.wait(0.05);
    f64 quiet = time_now() - t0;
    KV_CHECK(c, quiet >= 0.04 && quiet < 0.15);
    t0 = time_now();
    p.wait(5.0);
    f64 woke = time_now() - t0;
    KV_CHECK(c, woke < 1.0);
    char line[16] = {};
    KV_CHECK(c, p.read_out(line, sizeof line - 1) > 0 && strncmp(line, "late", 4) == 0);
    p.close();

    out.free();
    err.free();
    io_remove_tree(dir);
}
