// Child processes on Windows: CreateProcessW, as the posix fork and exec are on the others.
//
// - The child and everything it starts are one job object, made with the child suspended
//   so nothing escapes before it is in the job: kill() ends the job (posix: the process
//   group), with exit code 128 + 9, what a SIGKILL reports there. The job also ends with
//   its last handle, so a goldberg that dies takes its converters with it.
// - Stdout and stderr are anonymous pipes. They cannot be made non-blocking, so a read
//   asks PeekNamedPipe first and takes only what is there; a broken pipe with nothing left
//   is the end. Stdin is NUL.
// - Only those three handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): a child
//   started on another thread meanwhile does not get this one's pipe, which would keep it
//   open past this child's end.
// - The command line is built by the rules the C runtime parses it with, every argument
//   quoted. Quoting also keeps a Cygwin or MSYS program (Git's sh) from globbing a `*`
//   that arrived as an argument, as posix's execvp would not.
// - The environment is the parent's with desc.env over it, names compared as Windows does,
//   without case.
#include "korvid/platform/process.h"
#include "korvid/core/array.h"
#include "korvid/core/format.h"
#include "wide.h"
#include "../os_err.h"
#include "../os_handle_table.h"
#include <errno.h>
#include <string.h>

namespace kv {

namespace {

constexpr DWORD kKilledCode = 128 + 9;

// Appends s, UTF-8, to out as UTF-16. False if it is not UTF-8.
bool append_wide(Array<wchar_t>& out, const char* s, usize len) {
    if (len == 0) return true;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)len, nullptr, 0);
    if (n <= 0) return false;
    wchar_t* at = out.push_n((u32)n);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)len, at, n);
    return true;
}

// One argument, quoted as CommandLineToArgvW and the C runtime read it back: backslashes
// are literal except before a quote, where each is doubled and the quote escaped.
void append_arg(Array<wchar_t>& out, const wchar_t* a) {
    out.push(L'"');
    for (const wchar_t* p = a;; ++p) {
        u32 slashes = 0;
        while (*p == L'\\') { ++slashes; ++p; }
        if (*p == 0) { for (u32 i = 0; i < slashes * 2; ++i) out.push(L'\\'); break; }
        if (*p == L'"') { for (u32 i = 0; i < slashes * 2 + 1; ++i) out.push(L'\\'); }
        else for (u32 i = 0; i < slashes; ++i) out.push(L'\\');
        out.push(*p);
    }
    out.push(L'"');
}

// The length of a "NAME=value" entry's name, the '=' left out. An entry whose name starts
// with '=' (cmd's per-drive directories, "=C:=C:\...") keeps that '=' in its name.
usize name_len(const wchar_t* e) {
    const wchar_t* eq = wcschr(e + 1, L'=');
    return eq ? (usize)(eq - e) : wcslen(e);
}

bool overridden(const wchar_t* entry, const Array<wchar_t>& extra_names, const Array<u32>& starts) {
    usize n = name_len(entry);
    for (u32 i = 0; i < starts.len; ++i) {
        const wchar_t* name = extra_names.data + starts[i];
        if (wcslen(name) == n && _wcsnicmp(name, entry, n) == 0) return true;
    }
    return false;
}

// The parent's environment block with `extra` laid over it, as CreateProcessW takes it
// (CREATE_UNICODE_ENVIRONMENT): entries, each terminated, then one more terminator.
bool make_env(Span<const char* const> extra, Array<wchar_t>& block) {
    Array<wchar_t> names{};
    Array<u32> starts{};
    Array<wchar_t> values{};
    bool ok = true;
    for (u32 i = 0; i < extra.len && ok; ++i) {
        const char* e = extra.data[i];
        const char* eq = strchr(e, '=');
        if (!eq || eq == e) continue;
        starts.push(names.len);
        ok = append_wide(names, e, (usize)(eq - e));
        names.push(0);
        ok = ok && append_wide(values, e, strlen(e));
        values.push(0);
    }
    if (ok) {
        wchar_t* parent = GetEnvironmentStringsW();
        for (const wchar_t* p = parent; p && *p; p += wcslen(p) + 1) {
            if (overridden(p, names, starts)) continue;
            usize len = wcslen(p) + 1;
            memcpy(block.push_n((u32)len), p, len * sizeof(wchar_t));
        }
        if (parent) FreeEnvironmentStringsW(parent);
        if (values.len) memcpy(block.push_n(values.len), values.data, values.len * sizeof(wchar_t));
        if (block.len == 0) block.push(0); // an empty block is still two terminators
        block.push(0);
    }
    names.free();
    starts.free();
    values.free();
    return ok;
}

void close_handle(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

HANDLE as_handle(Handle v) { return os_handle_open(v) ? (HANDLE)(intptr_t)os_handle_value(v) : nullptr; }
Handle pipe_of(HANDLE h, const char* what) { return os_handle_make(OsKind::Pipe, (i64)(intptr_t)h, what); }
// The slot first, then the handle: every copy is stale before the OS reuses the value.
void close_os(Handle& v) {
    i64 os = os_handle_release(v);
    if (os >= 0) CloseHandle((HANDLE)(intptr_t)os);
    v = Handle{};
}

i32 pipe_read(Handle& fd, void* buf, u32 cap) {
    HANDLE h = as_handle(fd);
    if (!h) return -1;
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        close_os(fd); // broken: the child has closed its end and everything is read
        return -1;
    }
    if (avail == 0) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, avail < cap ? avail : cap, &got, nullptr)) {
        close_os(fd);
        return -1;
    }
    os_handle_count(fd, got, 0);
    return (i32)got;
}

void win32_message(char* out, usize cap, const char* what, DWORD err) {
    wchar_t w[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, w, KV_COUNTOF(w), nullptr);
    char msg[256] = "";
    if (n) {
        narrow(w, msg, sizeof msg);
        for (usize i = strlen(msg); i > 0 && (msg[i - 1] == '\n' || msg[i - 1] == '\r' || msg[i - 1] == '.'); --i) msg[i - 1] = 0;
    }
    format(out, cap, "%s: %s", what, msg[0] ? msg : "error");
    if (!msg[0]) format(out, cap, "%s: error %lu", what, (unsigned long)err);
}

} // namespace

bool process_supported() { return true; }

bool Process::spawn(const ProcessDesc& desc, i32* err) {
    close();
    error[0] = 0;
    bool has_argv = desc.argv.len != 0 && desc.argv.data[0] && desc.argv.data[0][0];
    if (!desc.shell && !has_argv) { format(error, sizeof error, "nothing to run"); return os_fail(err, EINVAL); }
    const char* what = desc.shell ? desc.shell : desc.argv.data[0]; // what a failure names

    Array<wchar_t> cmd{}, env{}, arg{};
    bool ok = true;
    if (desc.shell) {
        // The shell's line, as written: cmd.exe does not read a command line by the C
        // runtime's rules, and `<cmd> /d /s /c "<line>"` is the one form in which it takes
        // exactly the outer pair of quotes off and runs what is inside as it stands, quotes,
        // builtins, `&&` and all (/d: no AutoRun). %ComSpec% names the shell; cmd.exe if not.
        wchar_t comspec[KV_WIDE_PATH_MAX];
        DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, KV_WIDE_PATH_MAX);
        if (n == 0 || n >= (DWORD)KV_WIDE_PATH_MAX) wcscpy_s(comspec, L"cmd.exe");
        append_arg(cmd, comspec);
        const wchar_t flags[] = L" /d /s /c \"";
        memcpy(cmd.push_n(KV_COUNTOF(flags) - 1), flags, sizeof flags - sizeof(wchar_t));
        ok = append_wide(cmd, desc.shell, strlen(desc.shell));
        cmd.push(L'"');
    } else {
        // argv is argv: each word quoted as CommandLineToArgvW and the C runtime read it
        // back, whatever the program is.
        for (u32 i = 0; i < desc.argv.len && ok; ++i) {
            arg.len = 0;
            ok = append_wide(arg, desc.argv.data[i], strlen(desc.argv.data[i]));
            arg.push(0);
            if (i > 0) cmd.push(L' ');
            if (ok) append_arg(cmd, arg.data);
        }
    }
    cmd.push(0);
    ok = ok && make_env(desc.env, env);
    wchar_t wcwd[KV_WIDE_PATH_MAX];
    if (ok && desc.cwd && !widen(desc.cwd, wcwd, KV_WIDE_PATH_MAX)) ok = false;
    if (!ok) {
        format(error, sizeof error, "%s: not UTF-8, or too long", what);
        cmd.free(); env.free(); arg.free();
        return os_fail(err, EILSEQ);
    }
    if (desc.cwd) {
        DWORD a = GetFileAttributesW(wcwd);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            format(error, sizeof error, "%s: No such directory", desc.cwd);
            cmd.free(); env.free(); arg.free();
            return os_fail(err, ENOENT);
        }
    }

    // The three ends the child gets, inheritable; the parent's ends are not.
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr, null_in = INVALID_HANDLE_VALUE;
    bool pipes = CreatePipe(&out_r, &out_w, &sa, 0) && CreatePipe(&err_r, &err_w, &sa, 0) &&
                 SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0) && SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    if (pipes) null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE job = pipes && null_in != INVALID_HANDLE_VALUE ? CreateJobObjectW(nullptr, nullptr) : nullptr;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    HANDLE inherit[3] = {null_in, out_w, err_w};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)mem_alloc(attr_size);
    bool attrs_ok = job && attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    PROCESS_INFORMATION pi{};
    DWORD spawn_err = 0;
    bool started = false;
    if (attrs_ok) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb         = sizeof si;
        si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = null_in;
        si.StartupInfo.hStdOutput = out_w;
        si.StartupInfo.hStdError  = err_w;
        si.lpAttributeList        = attrs;
        // A console program gets a console. With one here (a terminal, Ninja), the child
        // shares it; CREATE_NO_WINDOW would give every child a console of its own, and every
        // console a conhost.exe: a second process started for each one asked for. With
        // none (a GUI parent), CREATE_NO_WINDOW, so no child flashes a window.
        DWORD console = GetConsoleCP() ? 0 : CREATE_NO_WINDOW; // a console attached, shown or not
        started = CreateProcessW(nullptr, cmd.data, nullptr, nullptr, TRUE,
                                 CREATE_SUSPENDED | console | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                 env.data, desc.cwd ? wcwd : nullptr, &si.StartupInfo, &pi) != 0;
        if (!started) spawn_err = GetLastError();
    } else {
        spawn_err = GetLastError();
    }
    if (attrs) { if (attrs_ok) DeleteProcThreadAttributeList(attrs); mem_free(attrs); }
    cmd.free(); env.free(); arg.free();
    // The child has its copies now (or there is no child): the parent's go.
    close_handle(out_w);
    close_handle(err_w);
    close_handle(null_in);

    if (started && !AssignProcessToJobObject(job, pi.hProcess)) {
        spawn_err = GetLastError();
        TerminateProcess(pi.hProcess, kKilledCode);
        WaitForSingleObject(pi.hProcess, INFINITE);
        close_handle(pi.hThread);
        close_handle(pi.hProcess);
        started = false;
    }
    if (!started) {
        close_handle(out_r);
        close_handle(err_r);
        close_handle(job);
        if (spawn_err == ERROR_FILE_NOT_FOUND || spawn_err == ERROR_PATH_NOT_FOUND) format(error, sizeof error, "%s: not found", what);
        else win32_message(error, sizeof error, what, spawn_err);
        return os_fail(err, errno_from_win32(spawn_err));
    }
    ResumeThread(pi.hThread);
    close_handle(pi.hThread);
    pid    = (i64)pi.dwProcessId;
    char label[160];
    format(label, sizeof label, "stdout of %s", what);
    out_fd = pipe_of(out_r, label);
    format(label, sizeof label, "stderr of %s", what);
    err_fd = pipe_of(err_r, label);
    process = os_handle_make(OsKind::Process, (i64)(intptr_t)pi.hProcess, what);
    format(label, sizeof label, "the job of %s", what);
    group   = job ? os_handle_make(OsKind::Process, (i64)(intptr_t)job, label) : Handle{};
    exited = false;
    code   = 0;
    return true;
}

bool Process::running() {
    if (pid < 0 || exited) return false;
    if (WaitForSingleObject(as_handle(process), 0) == WAIT_TIMEOUT) return true;
    DWORD status = 0;
    exited = true;
    code = GetExitCodeProcess(as_handle(process), &status) ? (i32)status : -1;
    return false;
}

// Anonymous pipes cannot be waited on: a look at each, then a short wait on the process
// (which ends at once if it exits), until something is there or the time is up.
void Process::wait(f64 seconds) {
    auto has = [](Handle fd) {
        HANDLE h = as_handle(fd);
        if (!h) return false;
        DWORD avail = 0;
        return !PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail > 0; // data, or the end
    };
    if (!os_handle_open(out_fd) && !os_handle_open(err_fd)) seconds = seconds < 0.001 ? seconds : 0.001;
    ULONGLONG until = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
    for (;;) {
        if (has(out_fd) || has(err_fd)) return;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return;
        if (os_handle_open(process) && !exited) WaitForSingleObject(as_handle(process), 1);
        else Sleep(1);
    }
}

i32 Process::read_out(void* buf, u32 cap) { return pipe_read(out_fd, buf, cap); }
i32 Process::read_err(void* buf, u32 cap) { return pipe_read(err_fd, buf, cap); }

void Process::kill() {
    if (pid < 0 || exited) return;
    if (!os_handle_open(group) || !TerminateJobObject(as_handle(group), kKilledCode)) TerminateProcess(as_handle(process), kKilledCode);
}

void Process::close() {
    if (pid >= 0 && !exited) {
        kill();
        WaitForSingleObject(as_handle(process), INFINITE);
    }
    close_os(out_fd);
    close_os(err_fd);
    close_os(process);
    close_os(group); // KILL_ON_JOB_CLOSE: whatever the child left running ends here
    pid = -1;
    exited = false;
    code = 0;
}

} // namespace kv
