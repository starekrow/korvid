# Profiler

Nov 7, 2026. Zones, waits, marks and counters in core; what reads them, and the rules.

## Principle

**The hot path is the profiler's own and nothing else's.** Recording a zone touches the
clock and the calling thread's ring: no allocation, no lock, no call into anything that
could carry a zone. What happens between a begin and an end is not constrained at all. A
present, a file read, a mutex, a call into a library, an allocation: those are exactly the
things worth a zone, so a zone can hold them.

## The pieces

- **Platform clock** (`core/time.h`, `src/<platform>/time.cc`): `os_ticks` is the finest
  monotonic clock the platform has, `os_ticks_per_second` its rate, `os_thread_cpu_ticks`
  the calling thread's CPU time where there is one. Records hold raw ticks; conversion
  happens at output.
- **Records** are 24 bytes: ticks with the kind in the top byte, a name pointer, a value.
  Kinds: zone begin, wait begin, zone end, mark, counter, frame, tick. Names are static
  strings compared by address; a dynamic name goes through the interner once.
- **Rings**, one per thread, claimed on first use from a fixed table like the memory
  accounting blocks, 64K records each on reserved pages. The thread writes and publishes
  its index with a release store; the collector reads with an acquire load and never
  stops the writer. Overflow overwrites the oldest records and is counted. A thread that
  exits marks its ring dead; the collector drains it and frees the slot.
- **The collector** runs on the main thread between frames (`prof_collect`): it pairs
  begins and ends per ring into a per-name table for the frame (calls, wall, worst, CPU)
  and a running table for the next summary, tracks busy and waiting per thread, writes
  every record to the trace if one is open, and keeps the frame's records if a dump may
  be wanted. Zones nest; the reader rebuilds the tree, so the writer tracks no depth and
  an end that never arrives is reported, never fatal.
- **Outputs**: `prof_log_summary` (top zones per frame since the last call, with the worst
  frame of each), `prof_log_frame` (the kept frame as a tree per thread), and a Chrome
  JSON trace (`--profile FILE`) that Perfetto or the browser opens, threads as tracks,
  frames and ticks on a track of their own, waits in their own category. Tools
  (`modules/tool`) take the same `--profile FILE`, and `--prof-report`, which prints the
  zones by total time when the tool exits: `goldberg --prof-report cook ...`.

## What is instrumented for free

The scheduler zones every unit by name, every boundary, command application, the
previous-column copy, triggers and the hash; the tick runner marks every tick. The pool
zones every chunk by its batch's name (a `parallel_for` is named after its unit, a level
batch "level") and marks its waits as waits: a worker's idle and a caller's wait for its
batch. The frame loop zones pump, ticks and render and marks every frame; memory counters
come from the frame sample. The engine marks module startup, ktable full, heap refused,
wave capped, stale events dropped and timers fired. A module adds zones only for what it
wants to see inside its own systems. The renderer writes the GPU's spans (below).

## Tracks

`prof_track(name)` claims a ring that no thread owns, for work timed on another clock;
`prof_track_begin`, `prof_track_end` and `prof_track_counter` write into it at times the
caller gives, in `os_ticks`. The collector treats it as a thread: its zones join the frame
stats and the summary, its busy time the thread stats, and the trace shows it as a track
of its own. The renderer's `gpu` track holds `gpu frame` and, inside it, `gpu upload`,
`gpu sort` and `gpu draw`, from timestamp queries read at the frame's next fence (two frames
late, so they land in a later frame's stats at the times they happened), with the frame's
fragment count and overdraw as counters. Calibrated timestamps put the GPU's clock on
`CLOCK_MONOTONIC` (the performance counter on Windows), which is `os_ticks`; without them
a frame's start is taken as its submit.

## Rules

- Never allocate or block on the recording path. The rings are pre-reserved.
- Profiling must not change the sim: it reads clocks and writes rings. The determinism
  tests hold with it on.
- CPU time per zone is a system call on most platforms, so it is off unless asked.
- The profiler is not instrumented with itself.
- Dev builds record when something will read: a trace, the report, a hitch dump. Ship
  builds compile the macros away.

## Later

A `prof` command set when the CLI exists; a Tracy sink as a module, dev builds only,
under the vendored tooling policy. (GPU tracks: done, above.)
