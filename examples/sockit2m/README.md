# sockit2m

Sock it to me: an I/O torture test the way the engine does I/O. Nothing blocks and
everything is pumped. It writes a working set of files, reads it back through the I/O
queue cold and hot with a bounded number of reads in flight, blasts loopback TCP through
the non-blocking sockets with an echo server on the same thread (bulk chunks, then
CLI-sized lines), runs file reads and the socket echo at the same time, and loads the
whole set again at idle priority from the slack of paced frames. Every byte that comes
back is checked against the pattern that went out. The numbers are throughput, request
latency, message rate and round trip, and how long a frame takes at worst and at the
99th percentile, which is the number that says whether a sim would have hitched.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory
(no window, no assets: the tool runs anywhere):

```bash
tools/kormake/bin/linux/kormake sockit2m dev    # or, in this directory: kormake dev
build/linux-dev/bin/sockit2m --quick
```

`kormake sockit2m` builds this example and what it links, in linux-debug unless a config follows:
debug is everything checked with the validation layers on, `dev` the build to measure
with, `ship` neither. `kormake test sockit2m` runs its tests; at the root, `kormake` builds
every module and example and `kormake test` runs every suite.
The debug build runs too, but it measures its own checks: use dev for numbers.

## Two modes

**Engine** (the default). The jobs are stepped by a module's systems the way a game's I/O
runs: the platform module owns the queue and `platform_pump` pumps it once per frame;
`sockit2m.sockets` steps the echo server and the clients once per tick in phase Input;
`sockit2m.files` reaps and refills in PreSim on the main thread; the driver loop is
`platform_pump` plus `engine_frame` on real time with a fixed tick clock (`--tick-hz`,
60 by default); file data lands in the module's allocator; the idle scenario paces frames
and hands the slack to the Work cycle (`engine_work`). The platform module needs no display and
links no SDL. This is what a game gets.

**Direct** (`--direct`). The same jobs in a tight loop over the primitives: an `IoQueue`
of its own pumped as fast as it can, sockets stepped every iteration. This is what the
primitives can do. `--compare` prints both in one table with a mode column.

## The scenarios

| scenario | what it measures |
|---|---|
| fs write | the disk's write number: `fwrite` then `sync` (on Windows, `FlushFileBuffers` on each file), setup |
| fs read cold | the set evicted from the page cache, read through the async ring (io_uring, Windows I/O rings) with `--inflight` reads out. Linux evicts with `posix_fadvise`; Windows opens each file once with `FILE_FLAG_NO_BUFFERING`, which makes the cache manager purge it; macOS cannot evict without root, so there it is warm and the run warns |
| fs read hot | the same with the cache warm: the kernel completes the reads inside the submit |
| fs read sync | the synchronous backend, hot (direct mode only; the platform's queue chose its backend) |
| sockets bulk | `--conns` connections each streaming `--chunk` KB chunks, echoed and verified byte for byte |
| sockets lines | four connections sending `N\n` lines, timed round trip: the CLI's shape |
| both | file reads and the echo together, with the queue's byte budget |
| both, no budget | the same with the budget lifted: what the budget buys |
| idle reads cold, hot | every read at `IoPriority::Idle`, frames paced at `--frame-ms` (one tick by default): the disk served only from the slack |

Each row reports throughput, latency, what was verified, and the frame (or pump) time at
average, p99 and worst. The run fails, with a non-zero exit, on any data error, dropped
byte, or allocation left live.

## What it found

On the development laptop (NVMe, 256 MB in 256 files, 8 connections), engine mode at
60 Hz: cold reads 1.27 GB/s at 3 us per frame; hot reads 1.65 GB/s with a worst frame of
9 ms under the 8 MB budget against 34 ms unbounded; sockets 120 MB/s each way, which is
exactly the 256 KB flow mark times eight connections times the tick rate (four times
that at `--tick-hz 240`), and a line round trip of one tick; under file load the sockets
keep 103% of their solo rate with the budget and 79% without, with a 45 ms worst frame;
256 MB of idle reads load from the slack of 16.7 ms frames at 1.37 GB/s cold with the
frame's work staying at the period. Direct mode: sockets 700 MB/s each way and 13.8 M
lines per second at 0.3 ms round trip. Zero data errors throughout.

On Windows 11 on the same laptop (2026-11-13, the same set, dev build), the queue on
Windows I/O rings: cold reads 1.69 GB/s at 4 us per frame, against 0.75 GB/s with
`--io sync`; in direct mode a cold pump costs 0.4 ms on the ring against 11.7 ms
synchronous. Hot reads 2.1 GB/s on either backend (a hot read is a copy inside the
submit, as with io_uring), worst frame 8 ms under the budget. Idle reads from the slack
1.36 GB/s cold (0.59 synchronous). Sockets the same 120 MB/s each way in engine mode and
100% of solo under file load with the budget; direct mode is loopback's own number,
260 MB/s each way and 7.9 M lines per second at 0.55 ms round trip, under half of
Linux's. Zero data errors. The ring needs overlapped handles: on a synchronous one
Windows performs the ring's reads one by one inside the submit (cold: 0.8 GB/s, the
whole read time spent in the pump), so `os_file_open` makes every handle overlapped.

On macOS (an M3 Max, 2026-11-13, dev build, three runs of each backend, medians), the
queue on the dispatch ring (Grand Central Dispatch's workers doing `pread`; macOS has no
kernel ring): in direct mode 8.8 GB/s cold and 9.2 hot against 6.2 and 6.6 with `--io
sync` (1.4 times), latency 6.5 ms against 9.2, worst pump 1.0 ms against 1.6; idle reads
from the slack 4.5 GB/s cold and 7.0 hot against 3.5 and 4.8; direct mode with sockets and
no budget keeps sockets at 279 MB/s against 145. Engine-mode reads are 3.0 GB/s on both:
their latency is a tick, and the tick, not the backend, sets that number. The cold pass is
partly warm (macOS cannot drop one file's pages without privileges). Sockets as before:
120 MB/s each way in engine mode, 629 MB/s and 10.4 M lines per second direct. Zero data
errors.

The findings that changed the engine: a pump's cost is proportional to the bytes it
admits, hence the queue's byte budget; sockets served from a tick system are bound by
what the kernel buffers hold per tick; the first cold submission of 64 MB costs 43 ms
where 8 MB costs 5. `docs/design/idle.md` has the design that came of it.

## Options

| option | default | what |
|---|---|---|
| `--dir PATH` | next to the executable | where the working set goes |
| `--files N`, `--size KB` | 256, 1024 | the working set |
| `--inflight N` | 64 | reads out at once (the queue's byte budget still applies) |
| `--conns N`, `--chunk KB` | 8, 64 | the socket torture |
| `--seconds S` | 3 | per socket run |
| `--tick-hz N` | 60 | the engine's tick clock |
| `--budget KB` | the queue's (8192) | bytes per pump; 0 is unlimited |
| `--frame-ms N` | one tick | the paced period of the idle scenarios |
| `--quick` | | a small set, one-second socket runs |
| `--keep` | | leave the files behind |
| `--io sync`, `--io ring` | the platform's choice | the queue's backend in both modes: `ring` is io_uring, Windows I/O rings, or the dispatch ring on the Apple family |
| `--direct`, `--compare` | | the tight loop instead; both |

## Where things are

| file | what |
|---|---|
| `src/torture.h`, `src/torture.cc` | `Options`, the pattern, `PumpClock`, the working set, `FsJob`, `SockJob`, and the module with its two systems |
| `src/main.cc` | the driver (engine or direct), the scenarios, the report, `main` |
| `include/korvid/sockit2m/module.h` | the record, `sockit2m_set_jobs`, `sockit2m_allocator` |
