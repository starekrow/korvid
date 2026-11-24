# Engine Slice 12 — A still window that sleeps

Nov 23, 2026 · @starekrow · draft

## Goal and exit criteria

Blackwing built its slice 4 on slices 10 and 11 and found two more things
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout, "Found in building
slice 4"): KR26 and KR27. Each keeps Blackwing's ID. It also wrote down two things it saw
and did not ask for; both are small and are taken here.

KR26 is the larger. `Pacing::Decoupled` draws nothing and ticks four times a second for a
paused player, as KR20 asked, and between those ticks wakes every 4 ms to ask the window
whether anything happened: about 230 wakeups a second and 2.5% of a core in a dev build on
Blackwing's laptop, against four wakeups and 0.03% with no window. `Pacing::OnDemand`
polls the same way, so goldberg's window pays it too, and Krome's K13 ("a still tool
window sleeps") is met only in the sense that it draws nothing.

It is done when:

- A windowed program that asks for nothing (no ticks, no frame, no Work, no I/O under
  way) wakes about as often as its heartbeat: `examples/ticker` under `--pacing decoupled`
  and under `--pacing demand`, ten seconds each, about 40 wakes where slice 11 measured
  2,440 and 2,495.
- A key press, a pointer move, a resize and the window's close are seen within a frame of
  the OS delivering them, as now.
- A frame asked for at a time (`engine_want_frame_at`: a caret's blink, a tooltip's delay)
  is drawn at that time, not at the next heartbeat.
- A dev build connected to `goldberg watch` sleeps as a ship build does, and a save still
  reaches the window as fast as slice 6 measured.
- `each row in rows height=24 count=@n from=@f { text "{row.name}" placeholder { text
  "..." } }` cooks; a `placeholder` anywhere else is a mistake with its line.
- A Krome screen that changes once is presented once under decoupled pacing.
- `krome_set` with a string longer than a `KromeSet` holds does not lose the rest in
  silence.
- Korvid's own tree builds as before, and the demos' hashes do not move.

## What the code has

Checked Nov 23 on `main` (52f9963).

| ID | Where | What is there now |
|---|---|---|
| KR26 | `frameloop/src/frameloop.cc` | `decoupled_pass` pumps, and with nothing to do calls `time_sleep(k_demand_poll)` (4 ms) and returns; the on-demand branch of `run` does the same after `demand_frame` says no. `engine_want_ticks` already sleeps till the next tick is due |
| KR26 | `platform/src/module.cc` | `platform_pump` runs the pump hooks (`platform_hid`'s, `touchpad`'s) and `IoQueue::pump`. Nothing waits |
| KR26 | `platform_hid/src/*` | SDL3 drains with `SDL_PollEvent`. Win32's window has a thread of its own in `GetMessageW`, which queues what it gets under a mutex for `pump` to swap out. Android pumps an `ALooper` that already has a wake pipe. iOS runs the main run loop for no time |
| KR26 | `core` (`engine_add_waker`) | A waker is a function the loop calls at each poll: `asset_live`'s connection and goldberg's window have one each, both reading a socket that does not block |
| KR26 | `cli/src/module.cc` | `cli.pump` is a system in `Phase::Input`, so the listener and its connections are read once a tick: at the heartbeat when nothing else ticks |
| KR27 | `kimb/src/kimb.cc` | `kimb_check`'s `node` walks the children of an `each` as nodes. `placeholder` is no kernel node and no definition, so it is refused. `kim` parses it (`kim_test.cc` has the case); Krome's builder finds it by its word (`build.cc`, under `virt->sparse`); `virtual_test.cc` hands the builder documents made by hand |
| seen | `krome/src/view.cc` | `krome.paint` calls `engine_want_frame` when it hands the renderer a list. It runs in the Frame cycle, after `decoupled_pass` cleared the request for the frame being drawn, so the request stands when that frame is done and the next pass draws again |
| seen | `krome/src/module.cc` | `krome_set` copies at most `KROME_SET_TEXT` (56) bytes into the event and says nothing of the rest |

## Waiting (KR26)

**`platform_wait(e, seconds)`** blocks the calling thread until the window has something
for the pump, `platform_wake(e)` is called, or the time is up, whichever is first. It
takes nothing off any queue: the pump after it reads what came. `platform_wake` may be
called from any thread (not from a signal handler); a wake that arrives before the wait
begins ends that wait at once.

| backend | waits in | woken by |
|---|---|---|
| SDL3 (Linux, macOS) | `SDL_WaitEventTimeout` with no event to fill, which leaves the event queued | a user event pushed with `SDL_PushEvent` |
| Win32 | an event object the window's thread sets when it queues a message | the same event, set |
| Android | `ALooper_pollOnce` with the time | the looper's wake pipe |
| iOS | the main run loop, run for the time or until it handles a source | `CFRunLoopWakeUp` |
| no window | a condition, timed | its signal |

Blackwing suggested `MsgWaitForMultipleObjects` for Win32. The loop's thread owns no
message queue there (the window's thread does), so an event object is the same thing with
less.

**The loop.** Where `decoupled_pass` and the on-demand branch sleep 4 ms they wait
instead, for the soonest of: the heartbeat (0.25 s after the last pass that did
anything), `engine.frame_due`, the next tick when ticks are wanted, and `--run-seconds`'
end. They do not wait at all when a frame is wanted, Work is pending, a load holds the
sim, or the frames after input are still owed. `Pacing::Continuous` shown is untouched;
hidden, it waits till its next tick as now.

**What else can end a wait.** Three things were seen by polling and are not the window's:

- **I/O under way.** While the queue has reads pending or completions unreaped the loop
  keeps the 4 ms poll it has, as `io_busy` does now. A program that reads nothing pays
  nothing. Waking from the ring's own completion is not in this slice (For review, 2).
- **Sockets.** `platform_wake_on(e, fd)` and `platform_wake_off(e, fd)`: the platform
  watches the sockets it is given from one thread of its own, blocked in `poll`
  (`WSAPoll` on Windows), and calls `platform_wake` when one can be read. It watches that
  socket again after the next pump, so a socket nobody drained does not spin the thread.
  `asset_live`, goldberg's window and the CLI's listener and connections give theirs.
  With no socket given the thread is never started.
- **A waker with no socket.** `engine_add_waker` stays as it is. While a waker is
  registered whose owner gave the platform nothing to watch, the wait is no longer than
  4 ms, as now, and the loop says so once at start-up. Nothing regresses; nothing new is
  required of a waker's author to keep working.

A wake by a watched socket ends the wait, and the pass after it ticks once, so `cli.pump`
reads what came without waiting for the heartbeat.

**Counted.** The loop's last log line gives waits in place of sleeps, and of them how
many ended by the window, by a wake and by the time. `examples/ticker` prints the same.

**Not woken.** A gamepad on Windows is read through SDL beside the Win32 window, and its
events do not end the wait: a pad is seen at the next heartbeat while nothing else is
wanted. A game runs `Continuous` and is not affected. `--cli-stdin` on Windows the same.

`frameloop_step`, `--record` and `--replay` are unchanged: a wait is the loop's, outside
any tick.

## The check and `placeholder` (KR27)

`kimb_check` learns what the builder already does. In an `each` with `count=` or `from=`
a child whose word is `placeholder` is the block for rows the model does not hold: its
children are checked as the each's own are (the row's variable is in scope, its fields
come out empty), and it takes no properties, flags, styles or strings. A second one in the
same `each` is a mistake. A `placeholder` in an `each` with neither, or anywhere else,
is a mistake: "placeholder is a sparse each's: one with count= or from=", with its line.

A definition named `placeholder` (Blackwing's workaround, `def placeholder { box { slot }
}`) still checks outside a sparse `each`, where the word is free. Inside one the block
wins, as it does in the builder.

`kimc format` keeps the block where it is. The tests cook one: `kim`'s check test, and a
Krome test that goes from `.kim` text through `kimb_check` to the tree, which no sparse
test does today.

## Two things seen on the way

**One present for one change.** `krome.paint` runs inside the frame that draws the list it
hands over, so its `engine_want_frame` asks for a second frame that shows the same
pixels. The request at the hand-over goes; the one for a transition mid-way stays, since
that is for the next frame. A test counts presents for one `krome_set` under decoupled
pacing: one.

**`krome_set` and a long string.** A string longer than `KROME_SET_TEXT` is sent whole, as
`krome_set_doc` sends a value, in order with the other sets. The caller does not have to
know the limit. `KromeSet`'s layout does not change.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR26 | `platform_wait`, `platform_wake`, on every backend and with no window; the decoupled and on-demand loops wait where they slept | P1 |
| KR26 | `platform_wake_on`, `platform_wake_off`; `asset_live`, goldberg's window and `cli` give their sockets; a waker without one keeps the 4 ms poll | P1 |
| KR27 | `kimb_check` takes `placeholder` under a sparse `each` and refuses it elsewhere and twice; a test that cooks one | P1 |
| S1 | A list handed over in the Frame cycle asks for no second frame | P2 |
| S2 | `krome_set` sends a string of any length | P2 |
| R13 | The module pages and quick references of `platform`, `platform_hid`, `frameloop`, `kimb` and `krome` change with their headers; `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **The check** (KR27). The smallest, and the only one with a workaround in someone's
   tree. Blackwing's `widgets.kim` drops its `def placeholder`.
2. **The wait, on SDL3 and with no window** (KR26). `platform_wait` and `platform_wake`;
   both loops; the counts. `ticker` for ten seconds under each pacing, on Wayland and X11,
   with a key pressed mid-run and its latency logged.
3. **Sockets** (KR26). The watching thread; `asset_live`, goldberg and `cli`. A dev build
   of `kromedemo` under `goldberg watch`: its wakes for ten still seconds, and a save's
   time to the window against slice 6's 9 ms.
4. **The other backends** (KR26). Win32, Android, iOS written; verified where a machine is
   to hand.
5. **One present, and the long string** (S1, S2).
6. **Blackwing.** `blackwing` with its face, paused, for a minute: wakeups a second and
   the share of a core, against 230 and 2.5%.

**Verification.** `kormake test` on linux-debug, linux-dev and linux-ship; `platform`,
`platform_hid`, `frameloop` and `krome` under `--sanitize address,undefined` and `thread`
(the wake is the first thing here another thread does to the loop); arcfire's and
arcanum's demo hashes as recorded.

## For review

1. **The heartbeat stays at 0.25 s.** Blackwing's test is "about as often as its
   heartbeat", and four wakes a second is what its windowless build already costs. With
   the wait in place the heartbeat could be longer, or go for programs that say so. Not
   in this slice: goldberg's tools count on it to show what the CLI patched.
2. **I/O keeps its poll.** Ending a wait from a completion means an eventfd on the
   io_uring, an event on Windows' ring, and a wake from each task worker. It matters only
   to a program that is still but reading, and the draft leaves it until one is measured.
3. **A thread for sockets, or the window's own wait.** SDL's wait takes no descriptors, so
   a socket cannot join it. The alternative to a thread is each backend's native wait
   under SDL (Wayland's and X11's descriptors in a `poll` with ours), which is SDL gone
   around on two platforms for one thread saved.
4. **`krome_set` sent whole, or refused.** The draft sends a long string the way
   `krome_set_doc` does. Whether that path takes a bare string, not a list or a table,
   was not checked; if it does not, the choice is to teach it or to make `krome_set` an
   error past 56 bytes that names `krome_set_doc`.
5. **S1's other callers.** The request at the hand-over was put there so an on-demand
   window shows a new list. Whether anything paints outside a frame that is then drawn
   (`frameloop_step`, a tool's own loop) was not traced; milestone 5 does that first.

## Status

**Milestone 1, the check (KR27): built Nov 24, 2026.** As above, and:

- **On lines of their own.** Kim takes one node to a line, so the exit criterion's
  `{ text "{row.name}" placeholder { text "..." } }` does not parse as written (the text
  takes the placeholder as its child). Written over three lines it cooks, which is what
  the tests do.
- **What it refuses**, each with its line: a placeholder in an `each` without `count=` or
  `from=`, or outside an `each`; a second one in the same `each`; one with properties,
  flags, styles, strings, a range or an event. Its children are checked as the row's are.
  A `def placeholder` still checks outside a sparse `each`; inside one the block wins.
- **The Krome test** compiles the text with `kim` (a test dependency only: Krome still
  does not link the parser), opens it, and gives it again as a save does, which runs
  `kimb_check` in Krome's own scope: before this change that reload was refused.
- **Found on the way:** with that reload refused, the test hung at shutdown. Freeing a
  screen's overlays closed "the top one", which is never a passive overlay, and the dev
  build's reload strip is one: any dev build that quit with a reload error showing went
  round for ever. Fixed; `reload_test` shuts down with the strip up.
- **Not done here:** Blackwing's `widgets.kim` keeps its `def placeholder` until it builds
  on a Korvid with this change.

**Milestone 2, the wait on SDL3 and with no window (KR26): built Nov 24, 2026.** As above,
and:

- **The waiter.** `platform` knows no window, so `platform_hid` gives it one at startup
  (`platform_set_waiter`: a wait and a wake), used while the window is open; before it
  opens, and in a tool, the platform waits on a condition. `platform_wake` sets a flag
  first, so a wake before the wait ends it at once, then signals the condition and calls
  the window's wake, under the lock that clearing the waiter at shutdown takes.
- **SDL3.** `SDL_WaitEventTimeout(nullptr, ms)`, rounded up to the millisecond so a wait does
  not end just short of its time. The wake is one user event at most
  (`SDL_RegisterEvents`), queued till a pump takes it. `HostWindow::input_delay()` keeps
  how long the last press waited, from SDL's stamp to the pump. Win32, Android and iOS
  wait 4 ms at most, as the loop's sleep did, until milestone 4.
- **The loop** waits where it slept, for the soonest of the heartbeat, `frame_due`, the
  next tick when ticks are wanted and `--run-seconds`' end. A wait that is not till a tick
  makes the next tick a tick's worth, as a sleep did. It is a 4 ms poll while I/O is under
  way, a load holds the sim, or any waker is registered (said once at start-up), which
  until milestone 3 is every waker. The last line counts the waits and what ended them.
- **Found on the way:** on-demand's idle path never ran `run_time_check`, so once
  `--run-seconds` passed it went round without waiting till the next heartbeat's frame
  (about 7,600 empty passes in the last quarter second). It runs there now.
- **Measured** (linux-dev, GNOME 50, SDL 3.4.2), `ticker --run-seconds 10`:

| run | waits in 10 s (window / wake / time) | main thread's wakeups a second, seconds 3 to 8 | CPU, the same |
|---|---|---|---|
| Wayland, decoupled | 43 (1 / 0 / 42) | 7 (before: 243) | 0.4% (before: 2.0%) |
| Wayland, on demand | 42 (1 / 0 / 41) | 22 (244) | 0.6% (2.0%) |
| X11, decoupled | 42 (0 / 0 / 42) | 7 (243) | 0.4% (2.0%) |
| X11, on demand | 41 (0 / 0 / 41) | 12 (242) | 0.6% (1.6%) |

  "Before" is the same binary built at milestone 1 (9f21b83), on the same machine. The CPU
  column is to the kernel's 10 ms tick, so 0.2% steps.
- **A frame at a time:** `ticker --frame-every 530` asks for a frame 530 ms ahead, again
  and again. Of 18 drawn in 10 s, decoupled: 0.63 ms late on average, 1.24 ms the latest;
  on demand: 0.89 and 1.58. Not at the next heartbeat.
- **Not measured: a key's latency.** Nothing here presses a key in a window it does not
  focus: GNOME gives a window started from a shell no focus, so XTest's key goes elsewhere;
  a key sent to the window alone (`XSendEvent`) is not taken by SDL (which reads keys
  through XInput2); and Mutter does not act on another client's resize of the window.
  `ticker` logs each key's `input_delay()` and each resize's time on the monotonic clock
  for a run by hand.
- **A wait of no time** is none: `platform_wait(e, 0)` returns at once, counts nothing, and
  leaves a wake for the next.
- **Verified, Nov 24, 2026.** `kormake test` passes on linux-debug, linux-dev and
  linux-ship (once each, two flakes rerun clean: goldberg's `berg cli` under ship, and
  asset's font fallback, which waits on a read, under dev while other builds ran).
  `platform` and `platform_hid` pass under `address,undefined` and `thread`, with the
  platform's new test of a wake from another thread. arcfire's and arcanum's demos hash
  the same as at milestone 1, tick for tick (14,154 and 1,865, at the default rate and at
  `--rate 4`). `tools/doccheck.sh` reports nothing new.

**Milestone 3, sockets (KR26): built Nov 24, 2026.** Different from the shape:

- **A waker says it is watched.** `platform_wake_on(e, fd)` gave the loop no way to know
  which wakers its sockets covered, and goldberg's waker has sources that are not sockets.
  So a waker that the platform can wait for registers with `engine_add_watched_waker`
  (`engine_add_waker` stays as it was, and polls). It states its sockets with
  `platform_wake_set(e, owner, fds, n)`, its `user` the owner, the whole set each time
  (so goldberg, whose programs and UI child come and go, states it each time it is asked),
  and any time it must be asked without a socket stirring with `engine_want_wake_at`. The
  loop waits no longer than that, and polls only while a waker added the old way is
  registered (`Engine::wakers_polled`).
- **The thread** (`posix/wake_watch_posix.cc`) starts at the first socket, polls the armed
  ones and a pipe of its own, and leaves out what woke it till the next `platform_wait`,
  not the next pump: by then the pass that woke has read it, so rearming does not fire
  again on what was just read. 64 sockets in all; past that `platform_wake_set` is false
  and the owner asks to be woken soon. Windows says it cannot watch
  (`platform_watches`), so its wakers register the old way until milestone 4.
- **Who gives what.** `cli`: its listener, each session, and stdin's descriptor where it
  is one (POSIX). `asset_live`: its connection; the next try while it has none; soon while
  its hello is queued (a connect under way shows nothing a read would see). goldberg's
  window: the file watcher's descriptor (`FsWatcher::wake_fd`, inotify's; -1 for FSEvents
  and ReadDirectoryChangesW, which `FsWatcher::due` then asks to poll soon), its listener,
  each program, the UI child's pipes; the watcher's next walk of a polled root; soon while
  its own queue's tasks run (they finish on workers, which wake nothing); every 50 ms while
  the session is not quiet, as before.
- **After a wake the next pass ticks** (decoupled), or draws (on demand, where ticks come
  with frames), so the CLI reads what came.
- **Measured** (linux-dev, GNOME 50):

| run | before | now |
|---|---|---|
| `kromescreens` connected to `goldberg watch`, 12 s still | a 4 ms poll while connected | 48 waits (window 6, a wake 1, the time 41), no poll |
| `goldberg watch --window`, still, seconds 3 to 8 | a 4 ms poll | 29 main-thread wakeups a second, 0.8% CPU |
| a CLI request (`help`) to an idle `ticker`, 12 each pacing | up to the heartbeat, 250 ms | 0.5 ms median, 1.2 the most |
| a save to the window (`live_numbers.py`, 10 each): screen `.kim`, prelude, icon, texture | 9.2, 8.9, 7.2, 49.9 ms median (slice 6) | 6.1, 10.1, 7.3, 49.9 |

- **Not done here:** Windows' thread (WSAPoll), milestone 4's.
- **Verified, Nov 24, 2026.** `kormake test` passes on linux-debug, linux-dev and
  linux-ship. core, platform, cli and asset_live pass under `address,undefined` and
  `thread`, with platform's test of a watched socket (a wake when something arrives, again
  while it is unread, none once read or forgotten). The demos hash as at milestone 1, tick
  for tick. `tools/doccheck.sh` reports nothing new.

**Milestone 4, the other backends (KR26): Windows built Nov 24, 2026.** As above, and:

- **Win32's wait.** Two events: `queued` (manual reset), which the window's thread sets as
  it appends to its queue and `pump` resets as it swaps the queue out, both under the
  queue's mutex, so a wait ends early exactly when something came since the last pump; and
  `woken` (auto reset), `wake()`'s. The loop's thread waits on both with
  `WaitForMultipleObjects`, in whole milliseconds rounded up. `pump` also clears `woken`:
  the platform's flag, set before the event, is what says a wake came, so a wake the
  platform already took does not end the next wait as well. Both events are made at the
  first `open` and kept for the process, so a wake from another thread never finds them
  closed.
- **`input_delay()` on Win32**, which had none: each message is stamped as the window's
  thread queues it, and a key or button press's delay is from there to the pump. That is
  the part the wait decides, not the OS's delivery before it.
- **The socket watch** (`win32/wake_watch_win32.cc`): the posix thread's shape, in
  `WSAPoll`. WSAPoll takes sockets only, so the thread's own wake is a UDP socket bound
  to the loopback and connected to itself, where POSIX has a pipe. `platform_watches` is
  true on Windows, so `cli`, `asset_live` and goldberg's window give their sockets there
  too; platform's test of a watched socket now runs on Windows.
- **Measured** (windows-dev, Windows 11, the Linux machine dual-booted), `ticker
  --run-seconds 10`, CPU from the process's times between seconds 3 and 8 (Windows counts
  them in 15.6 ms steps, so 0.3% is its smallest):

| run | waits in 10 s (window / wake / time) | CPU, seconds 3 to 8 |
|---|---|---|
| decoupled, before (the 4 ms wait put back) | 2,318 (0 / 0 / 2,318) | 0.31% |
| decoupled, now | 42 (0 / 0 / 42) | none counted |
| on demand, before | 2,292 (0 / 0 / 2,292) | 0.94% |
| on demand, now | 46 (0 / 0 / 46) | none counted |

- **Input, from another process** (Windows delivers a posted message to a window that does
  not have the focus, which X11 and Wayland would not): five key presses posted to
  ticker's window, five resizes (`SetWindowPos`), then `WM_CLOSE`. Decoupled: 16 waits ended
  by the window; each key seen 0.03 to 0.04 ms after the window's thread queued it; each
  resize seen 1.5 to 7.9 ms after it was asked, on the same clock; the close ended the run.
  On demand: 16 by the window, keys 0.03 to 0.06 ms, the resizes as decoupled.
- **A CLI request** (`help`) to an idle ticker, 72 in all over both pacings: 0.30 ms
  median, 0.22 the least; two took 6.7 and 18.4 ms, with no pattern to them.
- **`kromescreens` connected to `goldberg watch`**, 12 s still: 50 waits (window 0, a wake 1,
  the time 49), no poll, as Linux's 48.
- **Not done: goldberg's own window still polls on Windows.** Still for 12 s, it waited
  2,594 times, nearly all by the time. ReadDirectoryChangesW has no descriptor to give, so
  `FsWatcher::due` asks for a poll 4 ms ahead, as milestone 3 chose for it and FSEvents.
  Each root's overlapped read does signal an event when it finishes, which a wait could
  use.
- **Not measured:** a save's time to the window (`live_numbers.py`; there is no Python on
  this machine).
- **Verified, Nov 24, 2026.** `kormake test` passes on windows-debug, windows-dev and
  windows-ship, with asset_live's test (once each; one flake rerun clean: platform's `io
  tasks`, whose `free()` must return within a second, under debug while every suite ran
  at once). `tools/determinism-check.sh` passes: rocks agrees with itself, and arcfire's
  recordings replay matched. `tools/doccheck.sh` reports nothing new. Android and iOS are
  not done here.
