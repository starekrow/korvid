# Engine Slice 10 — What Blackwing asked for

Nov 22, 2026 · @starekrow · draft

## Goal and exit criteria

Blackwing is a music player built on Korvid from a repository of its own. Building its
slice 1 found nine things it needs from the engine, written up on its side as KR1 to KR9
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout). This slice takes
all nine. None is a new subsystem: three are kormake's, three the I/O queue's, two
`core`'s and one `cli`'s. Each keeps Blackwing's ID so the two documents read side by
side.

KR1 goes first. Blackwing's next milestone is its PipeWire backend, and a project cannot
link PipeWire today without editing `modules/toolchain-linux/module.lua`.

It is done when:

- Blackwing's `hifi_platform` builds against PipeWire on Linux with only files in
  Blackwing's tree changed, and a machine without the development package gets the
  `fetch` line from `kormake check`.
- `kormake test <module> --sanitize address,undefined`, and `--sanitize thread`, build a
  project's module, its closure (engine modules included) and its tests instrumented,
  and run them.
- `cli_run(e, line, nullptr, nullptr)` returns true for a command that prints.
- A file under a path of 1,000 bytes reads and writes through the I/O queue.
- A stat is a request with a completion, and no tick waits on it.
- The first 64 KB of a large file reads through the queue without the rest.
- A thread can ask for the OS's realtime audio class, and is told when the OS refuses.
- A module can build named sources with wrapping signed arithmetic on GCC, Clang and
  MSVC.
- The events documentation says that order across event types is not kept, and what to
  do instead.
- Korvid's own tree builds as before: the build files kormake writes for it differ only
  in the version on their first line, and the demos' hashes do not move.

## What the code has

Checked Nov 22 on `main` (c55b475).

| ID | Where | What is there now |
|---|---|---|
| KR1 | `tools/kormake/scripts/resolve.lua` | A module's `external` name must be in the toolchain's `externals`, or it is a soft failure ("which toolchain '%s' does not know"). `external_dir` finds a package by `KORMAKE_<NAME>_DIR`, `env` or `dir`, and `include` and `libdirs` are joined to that directory. Nothing asks the system |
| KR2 | `tools/kormake/scripts/emit.lua`, the toolchains | No sanitizer flags anywhere. `codegen_profiles` is the only per-module flag mechanism |
| KR3 | `modules/cli/src/module.cc`, `respond` | `if (rq.sink) {...}` and then the session, which is null for `cli_run`: a null sink falls through to it |
| KR4 | `modules/platform/include/korvid/platform/io.h`, `src/io.cc` | `IoEntry::path` is `char[256]`. `read_file` and `write_file` refuse `strlen(path) >= 256` |
| KR5 | `io.h` | `io_stat(path, out)` answers on the calling thread. `IoStat` has `is_file`, `is_dir`, `size`, `mtime_ns` |
| KR6 | `io.h`, `io.cc` | `IoEntry` already has `ranged`, `offset` and `length`, used for pack entries. `IoRequest` has no way to ask for them |
| KR7 | `modules/core/src/thread.cc` | `thread_set_realtime` sets a thread-local flag that the allocators verify against. No scheduler call in `core` |
| KR8 | `emit.lua`, the toolchains | `codegen` is one profile name per module; every toolchain has only `fast`. `isa_sources` shows the per-source shape: `{ avx2 = { "src/raster.cc" } }` |
| KR9 | `modules/core/include/korvid/core/event.h` | The file comment says every consumer sees every event once in the same order. It does not say that this is per type |

## kormake

kormake becomes 1.2. Blackwing's `korvid.config` can then say `requires_kormake "1.2"`.

**Externals a project defines (KR1).** An external is one record wherever it is written,
with the fields a toolchain's has today (`libs`, `frameworks`, `dir`, `env`, `include`,
`libdirs`, `runtime`, `check`, `fetch`) and one new one, `pkg_config`. A project writes
them in two places:

```lua
-- in the module.lua of the module that uses it
module "hifi_platform" {
  external  = { pipewire = { ">=0.3.60", platforms = { "linux" } } },
  externals = {
    linux = { pipewire = { pkg_config = "libpipewire-0.3", fetch = "sudo apt install libpipewire-0.3-dev" } },
  },
}

-- or in korvid.config, for one that several modules share
external "sqlite" {
  linux   = { pkg_config = "sqlite3", fetch = "sudo apt install libsqlite3-dev" },
  windows = { dir = "vendor/sqlite", include = { "include" }, libdirs = { "lib" }, libs = { "sqlite3" } },
}
```

- **One namespace.** A platform's externals are its toolchain's, then the config's, then
  every collected module's. A name defined twice for one platform is an error that names
  both files, so a project cannot redefine `sdl3`. A module's definition is usable by any
  module, like any other name.
- **`dir` in a project** resolves against the file that wrote it, not the engine root.
- **`pkg_config = "<package>"`** runs at generation time, when the target's platform is
  the host's: `--exists`, then `--cflags-only-I`, `--libs-only-L` and `--libs-only-l`
  fill `include`, `libdirs` and `libs`. Includes are passed with `flag_include_system`,
  as a directory external's are. Explicit fields beside it are the fallback when there is
  no `pkg-config` or the target is not the host. `KORMAKE_PKG_CONFIG` names the program
  (default `pkg-config`), which is also how kormake's own test supplies a fake one.
- **`include` and `libdirs` may be absolute** without a `dir`.
- **The version** (`">=0.3.60"`) is checked with `--atleast-version` when the external
  has `pkg_config`. Otherwise it stays documentation.
- **Missing is a message.** `--exists` failing is a soft failure in resolve, where a
  missing directory external is one now: the module, the package and the `fetch` line,
  from `kormake check` and before any compile.
- **Regeneration.** The package's `.pc` file (`--variable=pcfiledir`) is a regeneration
  input, so upgrading the package regenerates.
- **`kormake graph`** lists each instance's externals, a project's marked with the file
  that defined it. The host-tools line already shows what a tool links, whoever defined
  it.

**Sanitizer builds (KR2).** `--sanitize <list>` on `build` and `test`, Linux first.

- A toolchain gains `sanitizers`: `address`, `undefined` and `thread`, each with
  `cxxflags` and `ldflags`. Linux's are `-fsanitize=<name> -fno-omit-frame-pointer`,
  with `-fno-sanitize-recover=undefined` so a finding fails the test. A toolchain
  without the table refuses the option by name.
- Every instance in the graph is built with them, the engine's included, with
  `KV_SANITIZE_ADDRESS=1` and so on defined.
- The build directory is its own: `build/linux-dev+address+undefined/`. The plain
  target's is not touched.
- `address` with `thread` is refused, as the compilers refuse it.
- The realtime-thread allocation check is ordinary code and stays on.
- `core`'s arenas are not poisoned: ASan sees the general allocator's blocks and the
  stack, not use-after-reset inside an arena. That is a later piece of work.
- What the sanitizers find in the engine's own tests is fixed in this slice for the
  modules in Blackwing's closure. Anything else is listed in Status.

**Flags for named sources (KR8).** Two changes, both in the shape `isa_sources` already
has.

- `codegen` takes a list as well as a name: `codegen = { "fast", "wrapv" }`.
- `codegen_sources = { wrapv = { "src/flac.cc" } }` applies a profile to named sources
  only. A source it names leaves the module's unity build and is compiled alone.
- Every desktop toolchain gains the profile `wrapv`: `-fwrapv` for GCC and Clang. For
  MSVC see "For review", 3.

No raw per-toolchain flags in a `module.lua`: a profile keeps the word the same on every
toolchain, and a profile a toolchain lacks is already an error that names it.

## The I/O queue

**Long paths (KR4).** `IoEntry` keeps its 256 bytes inline and gains a pointer: a longer
path is copied into a block from the request's allocator and freed when the entry is.
The short case costs nothing new. The queue then has no limit of its own: a path the OS
refuses (past 4,096 bytes on Linux) completes `Failed` with the OS's error.
`KV_PLATFORM_PATH_MAX` (512, for the base and preference directories) is not involved
and does not change. `read_asset`'s joined mount path gets the same treatment. Mounts themselves (`IoMount::path`) stay at 256. Windows needs the `\\?\`
prefix past `MAX_PATH` in `src/win32/wide.h`; that is written here and verified when a
Windows machine next builds.

**Ranged reads (KR6).** `IoRequest` gains `offset` and `length`, with `length == 0`
meaning the whole file as today. The entry's existing `ranged` path does the work, with
the same priorities, budget and completion. A range that runs past the end completes
`Done` with the bytes that are there (`size` says how many); an `offset` past the end is
`Done` with zero. The budget counts `length`, not the file's size. `read_file` only:
`read_asset` stays whole-file in this slice, since a deflated pack entry has no ranges.

**Stat (KR5).** `Handle IoQueue::stat_file(const IoRequest&)`, completed like a read.
`IoCompletion` gains `IoStat stat`; `data` is null. A missing path is `Done` with
`is_file` and `is_dir` both false, as `io_stat` reports it, and `Failed` is for an error.
On io_uring it is one `IORING_OP_STATX`. Where the ring has no stat (Windows, Apple,
Android) it runs on the queue's task workers. Under `--io sync` it is done at the pump,
as reads are, and the header says so. `io_stat` stays for tools.

## core and cli

**The realtime audio class (KR7).** `bool os_thread_enter_audio_class()` in
`os_thread.h`, for the calling thread. It sets the allocator's flag
(`thread_set_realtime(true)`) whether or not the OS agrees, returns whether it did, and
logs the reason once when it did not.

| platform | what it does |
|---|---|
| Linux | `SCHED_FIFO` with `SCHED_RESET_ON_FORK`, at a priority inside `RLIMIT_RTPRIO`. Refused when the limit is 0. rtkit: see "For review", 1 |
| Windows | `AvSetMmThreadCharacteristicsW(L"Pro Audio")`; `avrt` becomes an external of the Windows toolchain |
| macOS | `THREAD_TIME_CONSTRAINT_POLICY`. Joining a device's workgroup is the backend's, which has the device |
| iOS, Android | false: the platform's audio thread arrives in the class already |

`platform_audio`'s backends keep calling `thread_set_realtime`, since their threads are
the device's.

**`cli_run` with no sink (KR3).** In `respond` and in the status line, a request with no
session and no sink discards the line. The return value is unchanged: false on failure.

**Order across event types (KR9).** Documented as not offered. A type is a stream, and a
handler sees its own type's batch. The text goes in `event.h`'s file comment (its home),
with a line in `modules/core/README.md` under "Behaviour worth knowing" and one in
[Modules](../concepts/modules.md). It says what to do instead: when the order between
two facts matters, make them one event type with a field that says which, or keep the
state in a resource and treat the events as "something changed". Blackwing's `seq` is
the first of these done by hand.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| KR1 | Externals defined in a `module.lua` or `korvid.config`; `pkg_config`; absolute `include` and `libdirs`; the version check; a duplicate name is an error; shown by `graph` | P0 |
| KR2 | `--sanitize address,undefined` and `thread` on `build` and `test`, into a build directory of their own, Linux | P1 |
| KR3 | A null sink in `cli_run` discards the lines | P1 |
| KR4 | `read_file` and `write_file` take any path the OS takes | P1 |
| KR5 | `stat_file` with a completion | P1 |
| KR6 | `offset` and `length` on `IoRequest` | P1 |
| KR7 | `os_thread_enter_audio_class` on Linux, Windows and macOS | P1 |
| KR8 | `codegen` as a list, `codegen_sources`, the `wrapv` profile | P2 |
| KR9 | The events documentation says order across types is not kept | P2 |
| R10 | kormake 1.2: `KORMAKE_VERSION`, the manual's `module.lua`, `korvid.config`, OPTIONS and ENVIRONMENT sections | P0 |
| R11 | The module pages and quick references of `platform` and `core` change with their headers (`cli` has no page yet; its header comment changes); `tools/doccheck.sh` reports nothing new | P0 |

## Milestones

1. **Externals** (KR1, R10). Collect, resolve and emit; a test project beside the
   fixture with a `pkg_config` external and a fake `pkg-config`, with expected files of
   its own. Then Blackwing's `hifi_platform` against the real PipeWire.
2. **The small ones** (KR3, KR9). A `cli` test for the null sink; the three pieces of
   text.
3. **The queue** (KR4, KR6, KR5, in that order: each is smaller once the one before is
   in). Tests in `modules/platform/tests`: a 1,000-byte path read and written, the first
   64 KB of a 16 MB file, a stat of a file, a directory and a missing path, each under
   `--io sync` and the ring.
4. **The audio class** (KR7). Linux built and run; Windows and macOS written.
5. **Sanitizers** (KR2). The toolchain table, the option, the build directory; then the
   engine's tests under each, and the fixes.
6. **`wrapv`** (KR8). Blackwing's `flac.cc` drops its GCC pragma.

**Verification.** `kormake build` and `kormake test` for linux-debug, linux-dev and
linux-ship. `tools/kormake/tests/run.sh` passes.
arcfire's and arcanum's demos hash as recorded. Blackwing builds and passes its tests
against the result, with its no-op sink and its "path is too long" failure removed. The
audio class is checked with `sched_getscheduler` where `RLIMIT_RTPRIO` allows, and by
the refusal's message where it does not. Windows and macOS are not verified in this
slice unless a machine is to hand: the status says which.

## For review

1. **rtkit.** A desktop Linux session usually has `RLIMIT_RTPRIO` 0, and realtime comes
   from rtkit over D-Bus. That would put a D-Bus client in `core`. The draft leaves it
   out: the call tries `SCHED_FIFO` directly and reports the refusal. PipeWire's own
   thread, the one Blackwing uses first, is unaffected. The alternative is a small
   module above `core` that speaks to rtkit, when a backend that owns its thread exists.
2. **Where a project's externals live.** The draft allows both the module and
   `korvid.config`. Allowing only the config would be simpler to explain and to check
   for duplicates, at the cost of a module that is no longer self-describing.
3. **`wrapv` on MSVC.** cl has no documented equivalent of `-fwrapv`. The candidates are
   an empty profile with a comment, or the undocumented `/d2UndefIntOverflow-`.
   Milestone 6 settles it by reading what cl generates for the decoder's overflowing
   expressions.
4. **Sanitizing by default.** Once `--sanitize` exists, should `kormake test` on the
   engine's own CI run once under address and undefined? Not in this slice.

## Status

**Milestone 1, externals (KR1, R10): built Nov 22, 2026 on branch `slice-10`.** kormake
is 1.2. What the build added or settled beyond the text above:

- **`requires_kormake` in a `korvid.config`**, checked down to the minor version (the
  master file's check is of the major only), so a project that uses `externals` says
  "1.2" and an older binary says to run the bootstrap.
- **A toolchain's external may use `pkg_config` too**: it is one code path.
- **pkg-config's other flags are not taken**: only `-I`, `-L` and `-l`. PipeWire's
  `-D_REENTRANT` is dropped, which is harmless beside `-lpthread`.
- **Version operators**: `>=`, `==` (or `=`) and `<=`; a bare version means `>=`.
- **An external a module uses on a platform nobody defines it for** is the soft failure
  it was, with a longer message. In a project every module is a root, so there it stops
  `check`: a module that uses PipeWire on Linux only says
  `external = { pipewire = { ">=0.3.60", platforms = { "linux" } } }`.
- **kormake's own test was failing on `main`** before this work: `tests/expected/` had
  fallen behind (the `KV_BIND_*` defines, among others). The expected files are written
  again here. Against a kormake built from `main`, this branch changes two things in
  them: the version on each build file's first line, and `externals:` on `graph` lines.
- **Checked on Linux:** the engine's `build/linux-dev/build.ninja` differs from `main`'s
  in its first line only. A scratch project outside the engine built and ran a program
  against `libdrm` found by the real pkg-config (2.5.1), its `.pc` file among the build
  file's inputs; `">=99"` was refused with the version found; a module that redefined
  `sdl3` was refused with both files named.
- **Not done:** Blackwing's `hifi_platform` against PipeWire. This machine has no
  `libpipewire-0.3-dev`: a module that asks for it gets "pkg-config does not find
  'libpipewire-0.3'" and the `fetch` line from `kormake check`, which is the other half
  of KR1's test. Nothing in Blackwing's tree was changed.
- **Not verified:** Windows and macOS. The project half of kormake's test is not run on
  Windows (the fake pkg-config is a shell script).

**Milestone 2, the small ones (KR3, KR9): built Nov 22, 2026.**

- **`cli_run` with a null sink** discards every line, the failure's included, and returns
  what it did. Before, `respond` fell through to the session and dereferenced null: a
  command that printed crashed. `cli`'s test runs four requests with no sink, two that
  succeed and two that fail.
- **Order across event types** is in `event.h`'s file comment, under "Behaviour worth
  knowing" in `modules/core/README.md`, and in the events paragraph of
  [Modules](../concepts/modules.md#how-code-flows-through-a-tick). `tools/doccheck.sh`
  reports what it did before (one name in `asset_render`'s quick reference).

**Milestone 3, the queue (KR4, KR6, KR5): built Nov 22, 2026.**

- **Long paths.** `IoEntry` keeps `path[256]` and gains `long_path`: a path of 256 bytes
  or more is copied whole into a block from the request's allocator (and cut inline, for
  logs), freed at `reap` or `free()`. `read_asset`'s joined directory path is built in a
  buffer of the mount path and `KV_PACK_PATH_MAX` and goes the same way. A 299-byte name
  now fails at the pump with `ENAMETOOLONG` where it was refused at submission; a path of
  5,000 bytes fails with the OS's error. A request with no path is still a null handle.
- **Windows.** `WidePath` in `src/win32/wide.h` widens a path of any length, and past
  `MAX_PATH - 12` (CreateDirectoryW's line) makes it absolute with `GetFullPathNameW`
  under `\\?\` or `\\?\UNC\`. `os_file_open`, `os_path_kind` and `os_path_stat` use it.
  Written, not compiled: no Windows machine here.
- **Ranges.** `IoRequest::offset` and `length`. `length` 0 means to the end of the file
  from `offset`, so the defaults are the whole file as before. A range is cut to the file
  when it is opened (`clip`), so its buffer is the bytes that are there, a range past the
  end is `Done` with those and an offset past the end `Done` with 0; the budget counts the
  cut range. A pack entry's range still fails when the archive ends inside it.
- **Stat.** `stat_file(req)` completes with `IoCompletion::stat`; `IoStat` moves up the
  header to sit beside it. On io_uring it is one `IORING_OP_STATX`, asked for only when
  `IORING_REGISTER_PROBE` says the kernel has it (5.6 on); its answer and a copy of the
  path live in a block of their own while in flight, since the pool's entries move when
  it grows. Elsewhere it runs on the task workers, in a lane past the callers' eight that
  no limit applies to. **Settled here:** with no task workers started, or under
  `--io sync`, the stat is done at the pump; the header says so. `os_path_stat` gains an
  optional errno so that "nothing there" (`ENOENT`, `ENOTDIR`) is `Done` and anything else
  `Failed`. `IoQueue::stat_on_tasks` sends stats to the workers on io_uring too, which is
  how the workers' path is tested on Linux.
- **Tests** (`io_long_ranged_stat`): a file under a 973-byte path written, read and
  stat'ed; the first 64 KB of a 16 MB file and four other ranges, with the budget's count;
  a stat of a file (its time equal to `io_stat`'s), a directory, a missing path and a
  cancelled one; each under the synchronous backend, io_uring, and io_uring with the
  stats on the workers.
- **Checked:** `kormake test platform` on linux-debug, linux-dev and linux-ship; the
  whole of `kormake test linux-dev`; `platform` compiled for android-dev. Not checked:
  Windows and the Apple family.

**Milestone 4, the audio class (KR7): built Nov 22, 2026.**

- **`os_thread_enter_audio_class()`** in `os_thread.h`, in `src/posix/os_thread.cc` and
  `src/win32/os_thread.cc`. The refusal is logged once a process, before the thread is
  marked, since logging may allocate.
- **Linux: priority 80**, or the soft `RLIMIT_RTPRIO` if it is lower (PipeWire's own
  data thread asks rtkit for 88). A limit of 0 is still asked, since a process with
  `CAP_SYS_NICE` is not held to it; `EPERM` with the limit at 0 is reported as "RLIMIT_RTPRIO
  is 0 (ulimit -r; ... rtkit, which core does not ask)". rtkit stays out, as "For
  review", 1 has it.
- **Windows: `avrt`** is an external of `toolchain-windows`, and `core` uses it on
  Windows only. Linux's `build.ninja` is byte for byte what it was; `kormake graph core
  windows-dev` lists it.
- **macOS:** a 2.9 ms period (128 frames at 44.1 kHz), 75% of it computation and 85%
  constraint, preemptible. Written, not compiled.
- **Checked:** `core`'s test starts a thread that enters the class twice. Here
  `RLIMIT_RTPRIO` is 0 hard and soft, so it is the refusal: refused both times, logged
  once, the thread marked, the caller's not; the check that the thread is `SCHED_FIFO`
  exactly when the call says yes ran, but only on its "no" side. The posix source compiles
  for Android with the NDK alone: `core` for android-dev does not build on `main` either
  (`job.cc` and `memory.cc`, "variable does not have a constant initializer", NDK 30).
  Not checked: the "yes" on Linux, Windows, macOS.

**Milestone 5, sanitizers (KR2): built Nov 22, 2026.**

- **`--sanitize`** on `build`, `test` and `gen` (and refused by name elsewhere). The names
  are sorted and deduplicated, so `undefined,address` and `address,undefined` are one
  directory, `build/linux-dev+address+undefined/`, which the regeneration rule passes
  on. `clean <target>` removes its sanitized directories too. The build root's
  `compile_commands.json` is not written by a sanitized `gen`, so clangd keeps the plain
  flags. Names are checked against the toolchain before anything is written.
- **`toolchain-linux`'s `sanitizers`** as the text above has them. A toolchain without the
  table, a name it lacks, and `address` with `thread` are each refused with a message;
  kormake's test has all three and a sanitized `gen` of the fixture
  (`expected/host-dev-sanitize.ninja`). The plain `build.ninja` is byte for byte what it
  was.
- **Settled here: which instances.** Every target instance. When the target is the host
  in the host tools' config (`linux-dev`), the tools the build runs are those instances
  and run sanitized (goldberg cooks under ASan); a separate host instance (`linux-debug`'s
  tools, built dev) is plain.
- **TSan and fences.** GCC refuses `atomic_thread_fence` under `-fsanitize=thread`, and
  the profiler's rings, seqlocks that race by design and validate by re-reading their
  index, use one. `KV_NO_SANITIZE_THREAD` (`atomic.h`) leaves exactly those three
  functions and `atomic_fence` out of TSan's instrumentation.
- **What the sanitizers found, fixed** (all in `core` and `platform`, in Blackwing's
  closure):
  - `core`, `memory.cc`: a thread's exit zeroed its accounting block with a plain struct
    assignment, `in_use` included, before the release store that frees the slot, so a
    new thread could claim the slot mid-reset and the exiting thread would then free it
    again under its new owner. The block is now zeroed field by field with `in_use` left
    for the release (TSan).
  - `core`, `memory.cc`: `Page::owner` was written under the page lock and read by
    `small_free` on any thread with plain accesses. Relaxed atomics now; the logic was
    already right (TSan).
  - `core`'s heap test read a ktable's tag after `engine_shutdown` freed it (ASan).
  - `platform`'s mount test leaked its read buffer (ASan's leak check).
- **Results.** Under `address,undefined` and under `thread`, every test of the engine's
  modules in Blackwing's closure passes. Outside it, not fixed in this slice:
  - `audio` (TSan): `audio_stats` copies `mixer.stats` while the device thread writes it
    in `mixer_render`; the counters want relaxed atomics.
  - `sprite_ui` (TSan): a lock-order inversion inside SDL3 and PipeWire's own libraries,
    reached through `audio_startup`; not the engine's code.
  - `goldberg` (both): "a watch with a small share" runs past its 30-second deadlines
    under either sanitizer, with no report; under TSan one cook in "goldberg scripts"
    also fails, with no report in the test's output (not looked into further).
- **Not the sanitizers': `asset_live`'s test fails in linux-ship** (9 of 21 checks: the
  listener never hears its client). It fails the same at `93d0e91`, before this
  milestone and the two before it.
- **Checked:** kormake's test; `kormake test` on linux-debug and linux-dev; linux-ship
  but for `asset_live`, above.

Milestone 6: not started.
