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
- **Eight scripts at once**, a constant.
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

## Not run

macOS and iOS compile paths touched by the handover; the Android emulator test runner.
The slice PRD lists each under the milestone that wrote it.
