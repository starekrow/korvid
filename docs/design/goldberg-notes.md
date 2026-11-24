# goldberg: notes from slice 1

Nov 14, 2026 · @starekrow

What building slice 1 showed, and what was measured. The design is in
`docs/PRDs/Goldberg-Master-PRD.md`; the record of what was built, milestone by
milestone, is the "As built" section of `Goldberg-Slice-1-PRD.md`; the manual is
`tools/goldberg/README.md`. Everything here was run on Linux only.

## Numbers

One Linux machine (16 threads), dev build, arcfire's closure: 106 source files in three
asset roots, about 3 MB.

| What | Time |
|---|---|
| First cook of arcfire, empty output tree | 65 ms |
| Second cook, nothing changed | 4 ms |
| `kormake build linux-dev` with nothing to do (seven cook edges, nothing else) | 41 ms |
| `watch`, inotify: a PNG saved to its copy readable in the output tree | 1.3 ms median, 2.5 ms worst of ten |
| `watch`, polling: the same | not measured; a walk is every 250 ms |

The exit criterion was a second cook under 100 ms. The cook edge always runs, at every
build, and at 4 ms that decision costs nothing anyone will notice.

Size: about 1,400 lines of C++ in the host, 2,600 of Lua in the application, and 900 of
tests that run the built binary.

### At scale: the heavy test

`kormake test --heavy goldberg linux-dev`, defaults: 1.07 GiB in 18,357 files over six
size classes, every 32 bytes hashed by a shell converter, ext4 on NVMe, the cache dropped
(0.0% resident). Digest `1fccef70833884e2`. Nov 14, 2026.

| What | First run | `posix_spawn` | The syscall cuts | I/O as tasks |
|---|---|---|---|---|
| Cold cook | 192 s | 23 s | 22.7 s | 12.5 s (2.0 s before the first converter, 7.7 s of converters, 2.8 s after) |
| Converters busy (their own time only) | 1% of eight | 7% | 8% | 22% |
| CPU, converters / goldberg and the shell | 9.7 s / 237 s | 9.7 s / 62 s | 10 s / 62 s | 13 s / 81 s |
| The 16,667 files of 10 KB | 163 s, 171 us a run | 10 s, 135 us a run | 8.4 s, 137 us a run | 5.3 s, 187 us a run |
| The 21 files of 10 MB to 200 MB | 2 s, 125 to 140 MB/s a run | the same | the same | 2.5 s, 90 to 110 MB/s a run (hashing and copying beside them now) |
| Second cook, nothing changed (the whole run) | 6.2 s | 6.7 s | 4.5 s | 4.6 s |
| One file per class changed | 8.8 s | 9.0 s | 9.3 s | 7.6 s |

The changed cook is not slow: one file per class includes the 200 MB, 100 MB and 50 MB
ones, about 380 MB hashed, converted and copied again. On a tree of 10,000 small files,
six changed files cook in the time nothing changed does.

At first goldberg was the cook, not the work, and nearly all of it was `fork()`: each
converter was started by a fork of goldberg (5.9 GB mapped, 16 threads), which copied its
page tables on the main thread and left its pages copy-on-write until the child exec'd.
The main thread spent four fifths of its time in the kernel. Linux and macOS now start a
child with `posix_spawnp` (a vfork underneath: modules/platform/src/posix/process_posix.cc);
iOS and Android still fork. `Graph:plan()` also walked and sorted every step (36,714 here)
whenever a converter ended; it now walks only the steps that are unchecked or have ended,
which measured nothing at this size (23.0 s with the old walk).

Then a syscall trace of a 10,000-file cook (strace on the main thread) showed 100 calls a
converter, and these were cut, halving them (1,002,316 to 532,581):

- `io_make_dirs` made every directory from `/` down on every call, three calls a step: 33
  `mkdir`, 31 of them failing. It now tries the leaf first and walks up only past a
  missing parent. `commit_run` also made the step's directory twice and before it needed
  to.
- A spawn made a third pipe that only the fork path reads, and set close-on-exec with two
  `fcntl` a pipe where Linux has `pipe2(O_CLOEXEC)`.
- `io_remove_tree` removed a directory with `remove()`, which tries `unlink` first.
- A cook with nothing to do re-encoded and rewrote the whole state (8.4 MB at 10,000
  files, pretty-printed): 1.2 s of a 3.2 s cook. It is now written only when a file was
  hashed, went, or a step's record changed, and without indentation (7.8 MB).

Then hashing, copying and converter runs became tasks on an `IoQueue` of goldberg's own
(`IoQueue::run_task`, `modules/platform/src/io_task.cc`): worker threads in lanes with
limits (runs 8, hashes one a core, copies half that), a worker taking the next task the
moment it is free instead of at the next tick. Hashes and copies stream (`io_read_each`,
`io_copy_file`) instead of reading whole files, and are no longer a batch the tick waits
for. A run's task spawns, drains and waits for its converter, moves what it wrote and
hashes it, and the tick gets one `file-made` event an output instead of a change, a hash
request and its answer. The first try was slower: `io_read_each` took a 1 MB buffer for
every file, which glibc maps and unmaps, and an unmap in a process of 40 threads stops
every core it runs on (24 workers at a fifth of a core each, mostly in the kernel). Sized
to the file and on the stack for small ones, the workers went quiet.

Now the main thread is the limit: Lua at 100% while the converters run. A watch takes a
share of it (`--share`, 40% by default): its events are budgeted to that share of a 60 Hz
tick, and it sleeps in proportion to what each tick used. Hashing and converters are not
held back, so a watch's first cook of 10,000 small files took 4.3 s at 40% against 3.3 s
at 100%. `cook`, asked of a watch, runs at 100% until stable.

A watch wrote its state each time it was stable, and encoding 7.8 MB of it in Lua was one
tick of 1.2 s at 10,000 files: what a GUI would have frozen for. The state is now written
as text directly (`string.format`, not a JSON value per entry and then an encoding), and
in a watch as a coroutine that does a slice a tick within the share: 17 slices at 10,000
files, the last (joining the text) 10 ms, the write 2.4 ms. Entries are read as they are
reached, not from a snapshot, which is safe because an out-of-date entry only costs a hash
or a cook at the next start; a change during the write makes the session dirty again.

The share broke a watch's start, and the heavy test now has a watch phase that would have
caught it. Nothing was planned until the output tree's scan was in, but nothing waited for
the work root's: a cook takes its events in a few large ticks and saw the converters'
outputs first in practice, while a watch at 40%, taking a few hundred events a tick, planned
before it had seen them, found the outputs missing and ran the converters again (3,567
cooks on an up-to-date tree of 10,000 files; at 18,357 it never settled). Planning now waits
until every root's scan is in. On the 1 GiB tree, a watch at 40% is stable in 7.6 s having
cooked nothing (a cook with nothing to do: 4.0 s at 100%), and a saved 8 KB source is right
in the output tree 6.9 ms later.

What still takes a long tick, at 10,000 files: a watch's start, where solving every file
(190 ms) and the first plan over every step (275 ms) are each one tick; and the line a
watch prints when stable, whose counts walk every step (50 ms).

Left as they are: `posix_spawn`'s file actions ask `getrlimit` five times a spawn (2 us
each); the work root holds a directory per step, so a cook opens and, in a cook as in a
watch, puts an inotify watch on each (10,000 here); and most of a converter's time on the
main thread is Lua. Each child has ended by the first time goldberg asks, so the eight run
as fast as goldberg ticks, not as fast as they finish. Also seen: `io_make_dirs` was the
45 `mkdir` a file the first run counted, and the `goldberg.event` pool fills at 16,384 near
the end of the cold cook (logged; nothing lost).

## What held

- **One path for a cook and a watch.** There is no code that knows which it is in until
  the session is stable: a cook exits there and a watch prints a line. The streaming
  converter test runs in a plain cook because of it, and the live tests are the cook
  tests with the process left running.
- **Events in, requests out.** The session reads no file and no clock. That made the
  replay harness nearly free: the test for rules changing under a running graph was a
  log with events added by hand, before any watch existed. Scripts did not strain it: a
  step's end is one more event.
- **Hashes decide.** Watcher events are treated as hints. Duplicates (the host's own
  event for a file it just moved, then inotify's for the same move) cost a second hash
  and nothing else. Nothing tries to be clever about which events to believe.
- **A chain is not planned.** The solver gives each file one next step. What a converter
  makes is a file, solved when it arrives. Path search is used to decide that a source
  *has* somewhere to go, and to explain it, not to build a list of steps in advance.
- **Pools plus Lua.** Rows for files, steps, rule files and events are engine pools,
  visible at `/kinds/goldberg` in a running watch; edges and answers are Lua tables. At
  106 files nothing shows in a profile. The claim that this split holds at tens of
  thousands of assets is still a claim.

## What changed on the way

- **Declared outputs go through a private directory**, not through held watcher events
  as the master PRD first said. The script writes where nothing looks; at a clean exit
  the host moves the files in and posts the events itself. It works with `--watch none`,
  and a failed run leaves nothing to undo.
- **The sweep of the output tree moved to the end.** With only copies, everything a cook
  would write was known once the sources were solved. With converters it is not known
  until they have run, so what "nothing accounts for" can only be said last.
- **The output tree is marked**, and goldberg refuses one that is not. A cook that
  removes what it did not make needs to be unable to do that to the wrong directory.
- **Nothing leaves the output tree by deletion.** The attic was asked for in review and
  is the reason the sweep can be trusted while the rules are still young.
- **Tests run the binary.** A test executable cannot link an executable module's
  objects, and for a tool whose contract is its command line that turned out to be the
  right test anyway.
- **`outputs` became optional**, because `goldberg learn` writes it. Running a script to
  see what it writes is the only honest way to know.
- **A watch needed a lock before it needed a protocol.** A cook that finds a watch on
  its tree exits at once and trusts it. Asking the watch to confirm it is stable first
  is the better answer and wants the CLI port to be discoverable.

## Rough edges

- **The Lua is read from the source tree** at start, located from the binary's own
  `__FILE__`. A goldberg moved off the machine that built it needs `GOLDBERG_LUA`.
  Embedding wants a generated-source feature in kormake.
- **Polling and `settle`.** A walk every 250 ms cannot tell a quiet file from one it has
  not looked at, so held files wait at least 300 ms on the polling watcher whatever the
  converter's `settle` says. macOS has only that watcher today.
- **A converter must write everything, every run.** A file it did not write this time is
  taken as left over from last time. A script that skips unchanged outputs loses them.
- **Failure does not spread downstream.** A failed step leaves what was cooked from its
  last good run in place, clean. That is the kindest thing for a running game and the
  wrong thing for a ship build that ignores exit statuses. Nothing here ignores them.
- **Converters are per file.** No fan-in: the atlas, the pack, anything that takes "every
  sprite in this directory" has no place yet. Slice 2.
- **A converter's script lives under an asset root**, so that it is hashed like any
  source. Tools shared across modules will want somewhere better.
- **Eight scripts at once**, a constant (now a lane's limit, set at startup).
- **Paths are 200 bytes** in the pools. A longer one is reported and not tracked.
- **`status` and `explain` reach a watch only through its port or its stdin.** From
  another terminal you have to know the port.
- **The project's `goldberg.berg` is polled** four times a second in a watch, since it
  sits in no watched root. In a cook it is read once.

## Windows

Done on the Windows machine (Nov 15), after slice 1 closed on Linux:

- **It builds.** The only thing in the way was the vendored Lua, which kormake now builds
  for every program with assets: cl's optimizer reports C4701 on Lua's number macros
  (`n1`, `n2` in lvm.c and lobject.c), past the `push, 0` around the vendored lines. It is
  off for the whole of `modules/lua/src/lua.cc`, which is Lua's alone (`unity = false`).
- **Processes** (`win32/process_win32.cc`): CreateProcessW, the child suspended until it
  is in a job object (kill ends the job: what a script started goes with it, and the code
  is 128 + 9 as on posix), stdout and stderr on pipes read through PeekNamedPipe, only the
  three standard handles inherited. `cmd /c` is run as `cmd /d /s /c "..."`: in the plain
  form cmd strips the line's last quote, which took the closing quote off a converter's
  last argument, its work directory. A `tool.bat` converter cooks.
- **The lock** (`lock.cc`): LockFileEx on a byte far past the end of the file, which the OS
  lets go of with the process, as flock; past the end so the holder's line stays
  readable.
- **The watcher** (`win32/fs_watch_win32.cc`): ReadDirectoryChangesW over each root's
  tree, overlapped, so a poll never waits. Windows does not say whether a name that went
  was a directory, so each root keeps the ones it knows of. On the polling watcher
  `learn` measured a script's outputs up to a walk late, and said 0.43 s for files written
  0.7 s apart; on this one the times are the writes'.
- **Tests.** `platform` (725 checks: the process suite and both watchers), `goldberg`
  (237), `lua` and `script` pass in dev; goldberg, lua and script are in the windows root
  now. Two tests were Linux's: the watcher test renamed onto an existing file with the C
  library's `rename`, which Windows' will not do (`io_rename` does), and the watch test
  sent a path unquoted down the CLI, which a temporary directory with a space in it
  (`C:/Users/A B/...`) splits. The render gate passes (D3D12), and arcfire runs from its
  cooked tree in dev and in ship.

### Windows at scale

`goldberg_heavy --size 64M` (18,359 files, the six classes, hashed) and `--size 1M` (550
files in two classes), NTFS on NVMe, 16 cpus, dev, the repository excluded from Defender
from the second column on. Digests `5df3639838e0a3e6` (64M) and `9e24a0808df2b717` (1M)
throughout. Nov 15, 2026.

| What | First run | `exec`, no shell | No console a child | A fresh converter | Delay-loaded DLLs | A converter a CPU |
|---|---|---|---|---|---|---|
| Cold cook, 1M | 7.8 s | 5.4 s | 3.3 to 4.5 s | 1.9 s | 1.44 to 1.50 s | 1.19 to 1.23 s |
| Cold cook, 64M | 270 s | | 150 to 252 s | 75.6 s (72.6 s of converters) | | 51.6 s (48.6 s of converters) |
| Nothing changed, 64M | 14.3 s | | 5.9 s | 6.0 s | | 6.1 s |
| Six changed, 64M | | | 6.1 s | 6.5 s | | 6.5 s |
| A watch, one save | | | 36 to 41 ms | 28 ms (1M) | 25 ms (1M) | 28 ms |

At the end, the default 1.06 GiB (18,357 files, `kormake test --heavy goldberg windows-dev`)
beside Linux's last column: the same machine, the same tree and the same digest, `1fccef70833884e2`:

| What | Windows, a fresh converter | Windows, delay-loaded, a converter a CPU | Linux |
|---|---|---|---|
| Cold cook | 78.2 s (2.4 s before the first converter, 75.0 s of converters, 0.9 s after) | 54.1 s (2.5 s, 50.7 s, 0.8 s) | 12.5 s |
| The 16,667 files of 10 KB | 66.7 s, 870 us a run (p50) | 45.1 s, 1.0 ms a run | 5.3 s |
| The 21 files of 10 MB to 200 MB | 84 to 102 MB/s a run | 54 to 72 MB/s a run (sixteen at once now, beside the small ones) | 90 to 110 MB/s a run |
| Nothing changed | 5.9 s | 5.9 s | 4.6 s |
| One file per class changed | 10.5 s | 10.4 s | 7.6 s |
| A watch, one save | 27 ms | 23 ms | |

The large files cook about as fast as on Linux; the gap is all in the small ones. A 10 KB
run's own time is about a millisecond, and the lanes finish 370 a second (250 with eight
and the shell DLLs): the rest of each lane's time a run, 40 ms of it at sixteen lanes, is
the OS starting and ending a process and goldberg moving its output, not the run itself.
Windows starts a process in milliseconds where Linux takes well under one; what is left
is that, which no change to goldberg removes. A converter that cooks many files a process
would.

Where it went, in the order it was found:

- **The shell.** A `shell` converter is `cmd /d /s /c "tool.bat ..."`: cmd and the script
  are two more programs a run, and a .bat is read a line at a time. A converter can now
  name a program, `"exec": "tools/up"` (or a name on PATH, or an absolute path), which
  is started with its arguments and no shell (`convert.lua`; `.exe` is tried first on
  Windows, and `conventions.berg` does not cook `*.exe`). The heavy test's converter is
  the heavy test itself, `goldberg_heavy xform ...`, where it was a script.
- **A console a child.** `CREATE_NO_WINDOW` gives each child a console of its own, and so a
  `conhost.exe`, a third process a run. A goldberg with a console (a terminal, Ninja,
  kormake) now lets its children share it; only a goldberg without one (started from a
  GUI) asks for none (`GetConsoleCP`: `GetConsoleWindow` is null under mintty, which has
  a hidden console).
- **What a start costs.** CreateProcess and a run of an empty program: 11 to 12 ms,
  against well under 1 ms for posix_spawn on Linux. Each new file: 8 to 9 ms outside the
  Defender exclusion, 3.7 ms in it. Those two are most of a 10 KB file's cook; the hashing
  is microseconds. Eight lanes keep eight starts in flight, not more cores busy.
- **A converter launched too often.** The cold cook grew slower run after run (150, 191,
  252 s) on the same tree. Windows keeps something for every launch of an image *file*:
  the one exe goldberg_heavy had been started from, on the order of 100,000 times, took 28 to 40 ms a
  launch, where a copy of it (any name, any directory) and a fresh link took 12 to 14.
  Writing the same bytes over it did not help; a new file at the same path did. Measured
  at 0.12 ms more a launch for each thousand. The heavy test now cooks with a copy of
  itself made for the run, `<dir>/xform.exe`. A real converter would see it too, after a
  great many cooks; relinking or reinstalling it starts over.
- **The scan** (nothing changed, 18,000 files: 1,733 ms to 20 ms). A walk asked each name
  for its size and time, a CreateFile and a close each. `io_list_files_stat` hands the
  walk's callback what the listing already carries (`FindFirstFileExW` has the size, time
  and attributes; a symbolic link is still asked, so it is followed). The watcher's walk
  uses it too, and `os_path_stat` uses `GetFileAttributesExW`, no handle opened.
- **Folding events** (757 ms to 4 ms, "fill rows"). Folding a path's events into one was a
  search back through the pending list for each; it is one pass with a table by path.
- **The watcher's directories** (5,541 of them: 760 ms to 320 ms to start). Each root's
  list of known directories was searched by name; it has an index by hash.
- **What is left** of nothing changed, 18,000 files: 0.7 s in all, of it Lua's settle
  0.3 s, handing events to Lua 0.2 s, making the session 0.17 s.
- **The DLLs a program loads.** Every korvid program imported shell32 and ole32 (for the
  one call that finds the preference directory) and ws2_32 (sockets), and shell32 brings
  user32, gdi32 and much of the shell in with it. A program that does nothing took 6 ms
  to start and exit; with those three imported, 10 ms; delay-loaded (opened at the
  first call), 6.7 ms. The Windows toolchain delay-loads them for every program
  (`modules/toolchain-windows/module.lua`), and goldberg's converter went from 13.5 ms
  of child's life a run to 7.8.
- **Eight at a time.** Process starts go on scaling past eight here (a launcher on its
  own: 650 a second with 8 threads, 820 with 12, 950 with 24), and a small file's run is
  mostly waiting on that. goldberg now runs a converter a CPU, at least 8.
- **What a start costs now**, measured from goldberg's run task (`run.spawn`,
  `run.running`, `run.commit` zones): CreateProcess 8.5 ms in the parent (2.7 ms of it
  CPU, the rest waiting, with the other lanes starting theirs), the child's life 7.8 ms, moving its output into the work tree 2 ms. A
  native launcher with nothing else to do gets 4.5 to 5 ms for the same CreateProcess at
  8 threads: the rest is the machine (it drifted between 4.8 and 8.4 ms over the session
  with nothing of ours changed).
- **Not worth it: a new directory each run.** A launcher showed that CreateProcess costs
  3 ms more when its command line names a directory made a moment ago, which a run's
  `{work}` does. Reusing one directory a worker took 2.5 ms off that CreateProcess in the
  launcher and nothing measurable in goldberg (four rounds each way, inside the noise),
  so it was taken out again.

The profile came from the engine's profiler, which every tool now has: `--profile FILE`
writes a trace (Chrome's format), `--prof-report` prints the zones by total time at the
end. goldberg has zones around its scan, events, Lua's tick and each part of a run
(`run.spawn`, `run.running`, `run.commit`, `task.hash`, `task.copy`), and Lua can add its
own (`host.zone(name)` ... `host.zone_end()`).

## macOS

goldberg builds and its tests pass on macOS (M3 Max, 16 cpus, APFS on its SSD), where it
first found two things the Linux and Windows runs had not:

- **No native watcher.** macOS polled. FSEvents is the native one now
  (platform/src/macos/fs_watch_macos.cc): `watching with fsevents`. It reports about 11 ms
  after a write, whatever is done to hurry it (FSEventStreamFlushSync made no difference
  and cost 3.6 ms a call), so `FsWatcher::lateness()` says 0.05 s for it, and `learn`, which
  judges at a converter's exit what it wrote outside its directory, hears the watcher out for
  that long first (`host.watch_sync`). The polling watcher had missed those writes too.
- **Clang's `-Wbitwise-instead-of-logical`** on the heavy test's two `step` calls joined by
  `&`, kept so that both report: two statements now.

### macOS at scale

`kormake test --heavy goldberg dev` on macOS, the default 1.06 GiB (18,357 files), dev,
the I/O queue on the dispatch ring. The digest is Linux's and Windows', `1fccef70833884e2`:
the same work. macOS cannot drop one file's pages unprivileged, so 15.5% of the tree was
still in memory for the "cold" cook. Nov 15, 2026.

| What | macOS | Linux | Windows, a converter a CPU |
|---|---|---|---|
| Cold cook | 49.6 s (1.45 s before the first converter, 47.8 s of converters, 0.32 s after) | 12.5 s | 54.1 s |
| Converters busy | 10% of 16 | 22% of eight | |
| CPU, converters / goldberg | 12.8 s / 133.8 s | 13 s / 81 s | |
| The 16,667 files of 10 KB | 44 s, 1.4 ms a run (p50; 3.7 ms mean, 14.1 ms p95) | 5.3 s, 187 us a run | 45.1 s, 1.0 ms a run |
| The 1,667 files of 104 KB | 3.7 s, 2.6 ms a run (p50) | | |
| The 21 files of 10 MB to 200 MB | 114 to 136 MB/s a run | 90 to 110 MB/s | 54 to 72 MB/s |
| Nothing changed | 4.34 s | 4.6 s | 5.9 s |
| One file per class changed | 6.97 s | 7.6 s | 10.4 s |
| Watch, until stable | 8.33 s | | |
| A watch, one save | 53 ms | | 23 to 28 ms |

The large files cook faster than on Linux and the cooks with little to do a little faster;
the cold cook's gap is all in the small files, as on Windows. A 10 KB run's own time is
about a millisecond, yet goldberg keeps a tenth of its sixteen busy and spends ten times
the converters' CPU: what is left is starting and finishing a process (`posix_spawn`, as
on Linux) and moving its output, not the run. Not profiled yet: `--profile` on goldberg's
main thread (the `run.spawn`, `run.running` and `run.commit` zones), as Windows was, is the
next look.

## Not run

iOS's own compile paths are built and its suites run in the Simulator (goldberg is not in
the ios root: a tool for the desktop); the Android emulator test runner.
The slice PRD lists each under the milestone that wrote it.

## The window (Krome slice 6)

Nov 18, 2026, Linux, Radeon 780M, dev build.

**A cold cook with the window up** (`goldberg watch --window` over the heavy test's tree at
`--size 64M`: 18,359 files, 36,718 steps through a shell converter, to the first stable line),
two runs each: headless 19.9 and 18.8 s, 48.1 and 47.0 s of CPU; with the window 17.3 and 16.7 s,
55.0 and 54.2 s of CPU. The window's session is a Work unit between frames and sleeps out no
share, so it finishes first; drawing and the models cost about 15% more CPU. As first built it
took 42.6 s and twice the CPU, and a still window over the stable tree used a whole core: the
window rebuilt its step table (36,718 rows, as JSON) four times a second whether or not anything
had changed. Now the counts go out ten times a second at most and only when the session moved,
the large models twice a second while it works, and the step table holds the first 2,000 steps
in state order. Idle over the stable tree, warm: 0.17 s of CPU in 10 s.

**The window editing itself**: a save of `assets/ui/window.kim` to the window rebuilt, through
the child watch of goldberg's own roots and `asset_live`: 10.6 ms (one in five 111 ms).

**`--demo`** passes on arcfire's, kromedemo's, kromescreens' and goldberg's own trees, and on
`tests/window` (Rules and Diff too). The pilot steps once a Krome frame: stepped once a tick, a
key held for a shortcut could be let go before the frame that read the press, since a pass of
the frame loop may run more than one tick.

