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
