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

HANDLE as_handle(i64 v) { return v < 0 ? nullptr : (HANDLE)(intptr_t)v; }

i32 pipe_read(i64& fd, void* buf, u32 cap) {
    HANDLE h = as_handle(fd);
    if (!h) return -1;
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        CloseHandle(h); // broken: the child has closed its end and everything is read
        fd = -1;
        return -1;
    }
    if (avail == 0) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, avail < cap ? avail : cap, &got, nullptr)) {
        CloseHandle(h);
        fd = -1;
        return -1;
    }
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

bool Process::spawn(const ProcessDesc& desc) {
    free();
    error[0] = 0;
    if (desc.argv.len == 0 || !desc.argv.data[0] || !desc.argv.data[0][0]) { format(error, sizeof error, "nothing to run"); return false; }

    Array<wchar_t> cmd{}, env{}, arg{};
    bool ok = true;
    // cmd.exe does not read its line by the C runtime's rules: given `/c` and more than two
    // quotes it strips the first and the last of the whole rest, which breaks every quoted
    // argument. `cmd /d /s /c "<the rest, quoted as usual>"` is the form it does read: /s
    // takes exactly the outer pair off and runs what is inside as written (/d: no AutoRun).
    u32 first = 0;
    bool via_cmd = desc.argv.len >= 3 && (_stricmp(desc.argv.data[0], "cmd") == 0 || _stricmp(desc.argv.data[0], "cmd.exe") == 0) &&
                   _stricmp(desc.argv.data[1], "/c") == 0;
    if (via_cmd) {
        const wchar_t lead[] = L"cmd /d /s /c \"";
        memcpy(cmd.push_n(KV_COUNTOF(lead) - 1), lead, sizeof lead - sizeof(wchar_t));
        first = 2;
    }
    for (u32 i = first; i < desc.argv.len && ok; ++i) {
        arg.len = 0;
        ok = append_wide(arg, desc.argv.data[i], strlen(desc.argv.data[i]));
        arg.push(0);
        if (i > first) cmd.push(L' ');
        if (ok) append_arg(cmd, arg.data);
    }
    if (via_cmd) cmd.push(L'"');
    cmd.push(0);
    ok = ok && make_env(desc.env, env);
    wchar_t wcwd[KV_WIDE_PATH_MAX];
    if (ok && desc.cwd && !widen(desc.cwd, wcwd, KV_WIDE_PATH_MAX)) ok = false;
    if (!ok) {
        format(error, sizeof error, "%s: not UTF-8, or too long", desc.argv.data[0]);
        cmd.free(); env.free(); arg.free();
        return false;
    }
    if (desc.cwd) {
        DWORD a = GetFileAttributesW(wcwd);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            format(error, sizeof error, "%s: No such directory", desc.cwd);
            cmd.free(); env.free(); arg.free();
            return false;
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
        started = CreateProcessW(nullptr, cmd.data, nullptr, nullptr, TRUE,
                                 CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
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
        if (spawn_err == ERROR_FILE_NOT_FOUND || spawn_err == ERROR_PATH_NOT_FOUND) format(error, sizeof error, "%s: not found", desc.argv.data[0]);
        else win32_message(error, sizeof error, desc.argv.data[0], spawn_err);
        return false;
    }
    ResumeThread(pi.hThread);
    close_handle(pi.hThread);
    pid    = (i64)pi.dwProcessId;
    out_fd = (i64)(intptr_t)out_r;
    err_fd = (i64)(intptr_t)err_r;
    os[0]  = pi.hProcess;
    os[1]  = job;
    exited = false;
    code   = 0;
    return true;
}

bool Process::running() {
    if (pid < 0 || exited) return false;
    if (WaitForSingleObject((HANDLE)os[0], 0) == WAIT_TIMEOUT) return true;
    DWORD status = 0;
    exited = true;
    code = GetExitCodeProcess((HANDLE)os[0], &status) ? (i32)status : -1;
    return false;
}

// Anonymous pipes cannot be waited on: a look at each, then a short wait on the process
// (which ends at once if it exits), until something is there or the time is up.
void Process::wait(f64 seconds) {
    auto has = [](i64 fd) {
        HANDLE h = as_handle(fd);
        if (!h) return false;
        DWORD avail = 0;
        return !PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail > 0; // data, or the end
    };
    if (out_fd < 0 && err_fd < 0) seconds = seconds < 0.001 ? seconds : 0.001;
    ULONGLONG until = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
    for (;;) {
        if (has(out_fd) || has(err_fd)) return;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return;
        if (os[0] && !exited) WaitForSingleObject((HANDLE)os[0], 1);
        else Sleep(1);
    }
}

i32 Process::read_out(void* buf, u32 cap) { return pipe_read(out_fd, buf, cap); }
i32 Process::read_err(void* buf, u32 cap) { return pipe_read(err_fd, buf, cap); }

void Process::kill() {
    if (pid < 0 || exited) return;
    if (!os[1] || !TerminateJobObject((HANDLE)os[1], kKilledCode)) TerminateProcess((HANDLE)os[0], kKilledCode);
}

void Process::free() {
    if (pid >= 0 && !exited) {
        kill();
        WaitForSingleObject((HANDLE)os[0], INFINITE);
    }
    HANDLE h = as_handle(out_fd);
    close_handle(h);
    h = as_handle(err_fd);
    close_handle(h);
    HANDLE proc = (HANDLE)os[0], job = (HANDLE)os[1];
    close_handle(proc);
    close_handle(job); // KILL_ON_JOB_CLOSE: whatever the child left running ends here
    os[0] = os[1] = nullptr;
    pid = out_fd = err_fd = -1;
    exited = false;
    code = 0;
}

} // namespace kv
