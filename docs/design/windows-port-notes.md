# Windows port notes

What Windows does differently from Linux and macOS, as the engine and its tools found it,
and what each one costs: most of it is about starting processes and making files, which
Windows does in milliseconds where Linux takes microseconds. Measured on Windows 11
(26200), 16 cpus, NTFS on NVMe, Defender on with the repository excluded, MSVC 14.51, Nov
14 and 15, 2026. The renderer is `renderer-d3d12.md`'s; goldberg's cook at scale, with the
numbers before and after each change, is `goldberg-notes.md` ("Windows" and "Windows at
scale").

## The build

- **cl's C4701 on Lua.** The optimizer reports a possibly uninitialized local (`n1`, `n2`
  in lvm.c and lobject.c) past the `push, 0` around the vendored lines. It is off for the
  whole of `modules/lua/src/lua.cc`, which is Lua's alone (`unity = false`), so it does
  not leak into a unity build.
- **One runtime, the release DLL (/MD)** in every config, so prebuilt libraries link
  whatever the config (`modules/toolchain-windows/module.lua`).
- **Delay-loaded system DLLs.** The platform module uses shell32 and ole32 for one call
  (`SHGetKnownFolderPath`, the preference directory) and ws2_32 for sockets. Imported
  outright, shell32 loads user32, gdi32 and much of the shell into every program before
  `main`. The toolchain links them with `/DELAYLOAD`, so they open at the first call:

  | An empty program, started and waited for | |
  |---|---|
  | nothing but the CRT | 6.0 ms |
  | shell32, ole32 and ws2_32 imported | 10.1 ms |
  | the same, delay-loaded | 6.7 ms |

  `/DELAYLOAD` cannot go in a `#pragma comment(linker)`; it is on the link line, with
  `delayimp.lib`, and LNK4199 (a program that never calls one of them) is ignored. A new
  system DLL that only some programs need belongs in the same list.
- **`far` and `near`** are macros in windows.h; code that names a variable either
  undefines them after the include (`renderer-d3d12/src/renderer_internal.h`).

## Processes

`modules/platform/src/win32/process_win32.cc`. CreateProcessW, the child suspended until
it is in a job object (KILL_ON_JOB_CLOSE: killing it ends what it started too), stdout and
stderr on pipes, only the three standard handles inherited
(`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`).

- **`cmd /c` reads its line its own way.** Given more than two quotes it strips the first
  and the last of the whole line, which took the closing quote off the last argument.
  `cmd /d /s /c "<the line, quoted as usual>"` is the form it reads as written; spawn and
  kormake's .bat cooks both use it.
- **A console for every child is a process for every child.** `CREATE_NO_WINDOW` gives the
  child a console of its own, and every console a `conhost.exe`: three times the cost of a
  start (24.7 ms against 7.9 ms, one at a time). With a console attached (`GetConsoleCP()`
  is nonzero: a terminal, Ninja, mintty's hidden one) the child shares it; only a parent
  with none (a GUI) passes `CREATE_NO_WINDOW`. `GetConsoleWindow` is the wrong test: it is
  null under mintty, which has a console. `DETACHED_PROCESS` costs the same as sharing,
  but gives a shell's own children a console each.
- **What a start costs.** CreateProcess and waiting for an empty program to exit: 6 ms in
  an excluded directory, 14 ms in one Defender scans, 9 ms for a signed system program;
  CreateProcess alone is 3 ms of it. Eight at once, 650 starts a second; it keeps scaling
  past eight (820 at 12 threads, 950 at 24) though each call gets slower (4 to 5.5 ms).
  The figure drifts with what else the machine is doing: the same eight-thread test read
  4.8 ms and 8.4 ms hours apart, with nothing of ours changed. Linux's `posix_spawn` of the
  same program is well under a millisecond.
- **A program launched very often gets slower to launch.** An executable started about
  100,000 times took 28 to 40 ms a launch where a copy of it (any name, any directory) or
  a fresh link took 12 to 14: about 0.12 ms more a launch for each thousand before it.
  The state is the file's, not its path's or its contents': writing the same bytes over
  it kept it slow, and a new file at the same path was fast. Where Windows keeps it was
  not found (Prefetch and the compatibility stores need an administrator to read). A
  benchmark that starts one program many times starts a copy made for the run
  (`goldberg_heavy`); a tool's converter would see it after a great many cooks, and
  relinking or reinstalling it starts over.
- **A new directory on the command line costs.** CreateProcess takes 3 ms more when its
  command line names a directory made a moment ago than one that has been there. Seen in
  a launcher; reusing directories in goldberg measured nothing, inside the noise
  (`goldberg-notes.md`).
- **`Process::wait`.** Anonymous pipes cannot be waited on, so it looks at each pipe and
  waits on the process for a millisecond at a time until something is there.

## Files

- **A stat is a handle opened and closed.** `os_path_stat` uses `GetFileAttributesExW`
  (no handle) and only opens a reparse point to follow it. A walk does better still:
  `FindFirstFileExW` already carries each entry's size, time and attributes, and
  `io_list_files_stat` hands them to the walk's callback (`os_dir_each_stat`), where
  asking each name was a CreateFile and a close: 1,733 ms to 20 ms for 18,000 files. The
  watcher's walk uses it too.
- **A new file costs** 8 to 9 ms outside the Defender exclusion and 3.7 ms in it, which is
  most of a small file's cook.
- **`rename` does not replace.** The C library's will not rename onto an existing file;
  `io_rename` (MoveFileEx with replace) does, and the tests use it.
- **The watcher** (`win32/fs_watch_win32.cc`): ReadDirectoryChangesW over each root's
  tree, overlapped, so a poll never waits. It does not say whether a name that went was a
  directory, so each root keeps the directories it knows of, found by hash (5,541 of them:
  760 ms to 320 ms to start).

## Tests and the machine

- **`sh`.** The platform and goldberg tests run shell scripts, and Windows PowerShell has no
  `sh` on PATH: run them from Git Bash, or `kormake test` from it. From PowerShell, 60 of
  goldberg's checks and 19 of platform's fail, all of them scripts.
- **Paths with spaces.** `C:/Users/A B/...` splits wherever a path goes down a command
  line unquoted; the tests quote theirs.
- **The clock can move back.** The system clock fell 7.5 hours mid-session (it read like a
  change from UTC to Pacific), and every file written before it was in the future: Ninja
  rebuilt `build.ninja` forever, its inputs always newer. Setting those files' times to now
  ended it; git does not keep times.
- **Defender.** Excluding the repository halves the cost of a new file and of a start from
  it; it does not remove either.

## Measuring

Every tool takes `--profile FILE` (a Chrome trace) and `--prof-report` (the zones by total
time, printed at exit), before its command: `goldberg --prof-report cook ...`
(`modules/tool`, `profiler.md`). The numbers above came from that and from a stand-alone
launcher (CreateProcess in a loop on N threads, with each of spawn's options on or off),
which separates what the OS charges from what the program adds.
