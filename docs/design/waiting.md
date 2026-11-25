# Waiting: standing watches, a child's exit, straight-line tasks

Nov 24, 2026. Five changes to how a program waits on the OS, from two cold reads of the
platform layer: one tool written the way the engine likes (systems and a watched waker),
one written the naive way (blocking loops in the command). Both were done from the docs
alone; each showed where the platform made its writer work too hard.

**Principles:** [8. Numbers or it didn't happen](../principles.md#8-numbers-or-it-didnt-happen)
for the Windows wait, [10. Say it once](../principles.md#10-say-it-once) for the standing
watch and `write_now`, [5. Fail loudly, with names](../principles.md#5-fail-loudly-with-names)
for what the naive tool was not told.

## The Windows wait

With no window, `platform_wait` waited on a condition variable. Windows rounds that wait
up to its scheduler tick, 15.6 ms unless something raised the timer resolution, so
`kPlatformSoon`'s 4 ms came about 60 times a second. A tool streaming a child's output
(its pipes cannot be watched on Windows, so they are asked about soon) moved 8.3 MB/s; the
naive tool, sleeping 1 ms at a time on `time_sleep`'s high-resolution timer, moved 12.2.

The wait is an `OsEvent` now (core, `os_thread.h`): on Windows an auto-reset event and a
high-resolution waitable timer, waited on together; on POSIX a flag, a mutex and a
condition, whose timed wait already kept time. The same tool moves 28.1 MB/s and wakes
about 250 times a second while a child runs, which is what `kPlatformSoon` always said.
`timeBeginPeriod` was the other way: one call, but it raises the resolution for the whole
process for as long as it lasts, where the timer costs only the waits that use it.

What is left: a child's pipes on Windows are still asked about, not watched. Overlapped
reads on named pipes, in place of anonymous ones, would let the watch hear them.

## A child's exit

A watch heard a child's pipes, not its end. An owner whose child closed its pipes (or never
wrote) asked `soon()` until `running()` said it was over: a branch in every owner, and a
loop polling at 250 a second for as long as such a child ran.

Each OS gives the exit as something a waiting thread can hear: Windows' process handle, a
pidfd on Linux (5.3 and later) and Android, a kqueue holding `NOTE_EXIT` on Apple.
`Process::process` holds it (`OsKind::Process`), `PlatformWatch::add(Process)` adds it, and
the watch thread polls it on POSIX. Windows' watch is `WSAPoll`, which takes sockets
only, so a child's exit there is a one-shot wait in the thread pool, on a duplicate of the
process handle so that a `Process` closed first does not leave the wait on a dead handle.
An exit happens once and stays signalled, so it is heard once and never rearmed. Where the
OS gives no handle (an older kernel), the old way stands: soon, once both pipes have ended.

## Standing watch membership

A stated watch is restated by the owner's waker on every pass, from whatever state says
what it waits on now. It is the right shape for an owner whose set is worked out afresh
(goldberg's: directories, programs, a queue, a time), and a heavy one for an owner that
waits on the things it opened until it closes them. The cold read's tool kept a stage
machine whose only job was to restate its watch.

`platform_watch_add(e, owner, thing)` adds a listener, a socket, a child or a handle until
it closes or is taken back; no waker. Membership is by handle, not by pointer: a handle
goes stale when its object closes, so a member that closed is let go before the next wait
without its owner saying so, and an object freed without a close leaves nothing that
points at freed memory. What a stated watch works out at each pass, a standing one needs
the platform to see for itself: a socket with output queued sets a flag on its handle
(`kOsQueued`), which the platform reads before each wait, and asks soon for. Members and
stated handles share the thread's 64; each owner's two sets go to the thread together.

Standing is the default the docs lead with; stated is the option. "Transient" membership
is what a stated watch already is.

## write_now, and its deadline

The naive tool's send loop (`write`, `flush`, sleep, until the bytes are taken) never
ended once the peer had gone, because `write` refuses a closed socket for good and says
false either way. `write_now(data, size)` is that loop done once and done right: it sends
what is queued and then `data`, waiting for room, and says false only when the connection
is over. `seconds` bounds the wait: false with the socket still open is a peer not
reading, and what the kernel had not taken is queued, past `out_limit` if need be, so the
next send goes on where this one stopped. The deadline and the queue are written once
(`net_now.cc`); each OS gives one send and one wait for room.

## Straight-line tasks

A command that blocks runs before the first tick: while it does, nothing else in the
engine runs. The naive tool worked, and its engine did nothing for it. `tool_task(e, fn,
arg)` runs the same function on a thread of its own, where blocking is fine, while the
engine ticks; its return is the exit status, as a `tool_finish`. When the run ends first,
`ctl.cancelled()` turns true and the run waits for the function, so its loops look and
its blocking calls have deadlines.

It is a thread per task, not a task on the I/O queue's workers: a straight-line tool's
function lives as long as the run, and on a worker it would hold a lane the queue's own
tasks share. The tool loop no longer counts the queue's running tasks as reasons to tick:
the workers wake the loop as each finishes, and counting them had a tool spin at a tick
per pass for as long as one ran.

## Not done

- Watching a child's pipes on Windows (above).
- Writability (`POLLOUT`) for a socket with output queued, in place of soon: the
  `kOsQueued` flag is what the thread would need to know when to ask.
- A blocking task for a game: `tool_task` is the tool's, since a game's loop has no run
  to finish. The I/O queue's tasks (`IoQueue::run_task`) are the game's way today.
