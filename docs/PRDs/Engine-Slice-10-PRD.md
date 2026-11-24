# Engine Slice 10 — What Blackwing asked for

Nov 22, 2026 · @starekrow · draft

## Goal and exit criteria

Blackwing is a music player built on Korvid from a repository of its own. Building its
slice 1 found nine things it needs from the engine, written up on its side as KR1 to KR9
(`docs/PRDs/Korvid-Changes-For-Blackwing.md` in Blackwing's checkout). This slice takes
all nine. None is a new subsystem: three are kormake's, three the I/O queue's, two
`core`'s and one `cli`'s. Each keeps Blackwing's ID so the two documents read side by
side.

Four more came after this draft and are stacked on it: KR10, a timed wait, from
Blackwing's slice 2, and KR11 to KR13 from planning its slice 3, the library. Three are
the platform module's and one `core`'s. KR11 blocks the library's third milestone.
KR14, C sources, came with the library's sqlite. Four more are stacked after it: KR15
and KR16 from slice 3's close, KR17 from the script-module spec and KR23 from planning
Blackwing's slice 4. Each is small. KR17 blocks that slice's fourth milestone. The rest
of what Blackwing asked on Nov 23 (KR18 to KR22, KR24, KR25) is slice 11's.

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
- A thread can wait on a condition variable until a signal or a deadline.
- One directory lists without its subdirectories, links marked, hidden entries on
  request, and without a stat an entry when only kinds are asked.
- A path can be asked whether it is on a share, and a watcher made with `Auto` polls a
  root that is.
- A stat names the volume: a mount point's differs from its parent directory's.
- `io_make_dirs` and `io_remove_tree` take a path of 3,000 bytes.
- A recursive watch does not go down a link to a directory, on any backend.
- A resource can have a field table and stay out of the state hash.
- A dev build can hand a module every delivered event with its type's name and fields.
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
| KR10 | `os_thread.h` | `os_cond_wait` only. The conditions are made with default attributes, so a POSIX timed wait would be on the realtime clock |
| KR11 | `io.h`, `os_file.h` | `io_list_files` and `io_list_files_stat` walk a whole tree, hidden entries skipped. Under them, `os_dir_each_stat` lists one directory, following links, with a `stat` an entry on POSIX and an error of no kind |
| KR12 | `fs_watch.h` | `Auto` is the native backend wherever there is one, for every root. Nothing asks what a file system is |
| KR13 | `io.h`, `os_file.h` | `IoStat` and `OsStat` have kind, size and time. `st_dev` and the ring's `stx_dev_*` are read and dropped |

Checked Nov 23 on `slice-10` (3627a5a) for KR10 to KR13.

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

## Stacked on: the timed wait and slice 3's three

**One directory (KR11).**

```cpp
enum IoListFlags : u32 { IoListHidden = 1u << 0, IoListSizes = 1u << 1 };
struct IoDirEntry {
    const char* name;    // the entry's own name, valid during the call
    IoStat      st;      // what it is, following a link; size, time and volume with IoListSizes
    bool        is_link; // a symbolic link (a junction too, on Windows); st is its target's, all false if it has none
    bool        hidden;  // a leading '.', or Windows' hidden attribute
    bool        utf8;    // false: not valid UTF-8; it still opens the file, and wants care to be shown
};
bool io_list_dir(const char* dir, u32 flags, bool (*fn)(void* user, const IoDirEntry& e), void* user, i32* err = nullptr);
```

- **Blocking, on any thread**: a job, or a task through `IoQueue::run_task`. Never a
  tick, as for `io_list_files`.
- **Sorted by name**, bytewise, as `io_list_files` is, so a walk is the same every time.
  The caller orders the directories.
- **What it costs.** Without `IoListSizes`, kinds come from the listing (`d_type` on
  POSIX), and only a link or an entry the file system does not type is asked of
  (`fstatat` on the open directory, no path walked). With it, POSIX asks of each entry,
  since `readdir` has no sizes; Windows' listing has them either way.
- **Names that are not UTF-8.** On POSIX a name is bytes and is handed back as it came,
  which `read_file` opens. On Windows a name is UTF-16 that may hold half a surrogate
  pair; it is narrowed to WTF-8 (the surrogate encoded as UTF-8 would encode it), and
  `wide.h` widens WTF-8 back, so the name opens the same file.
- **Failure** is false with the errno: `ENOENT`, `ENOTDIR`, `EACCES`, `EIO` or what the
  OS said. A stop from `fn` is not a failure.
- `io_list_files` and `io_list_files_stat` stay as they are.

**A share (KR12).** `bool io_path_is_remote(const char* path)`: whether the file system
under it may change without this machine hearing.

| platform | how |
|---|---|
| Linux, Android | `statfs`'s `f_type`: NFS, SMB and CIFS, FUSE (sshfs, rclone and the rest; `fuseblk`'s ntfs-3g too, which polls for nothing), 9p, AFS, Coda, Ceph |
| Windows | a `\\server\share` path, or `GetDriveTypeW` of its volume (`GetVolumePathNameW`) is `DRIVE_REMOTE` |
| macOS, iOS | `statfs` without `MNT_LOCAL`, or a macFUSE file system |

`FsWatcher::add_root(dir, FsWatchBackend how = Auto)` chooses per root: `Auto` is the
watcher's own backend, except that a watcher made with `Auto` polls a remote root.
`Poll` polls this root whatever the watcher is, for a share the test misses; `Native`
insists. `root_backend(id)` says which a root got. A remote root is walked every
`remote_interval` seconds (a new argument to `init`, default 5): the watcher's
`poll_interval`, 0.25 s, is for a local tree. The walk is on the thread that polls, as
the polling backend's always was, so a library that watches a share polls from a thread
of its own.

**A volume (KR13).** `IoStat::volume`, a `u64` for equality only: `st_dev` on POSIX, the
ring's `stx_dev_major` and `stx_dev_minor` joined as `makedev` joins them, the volume
serial number on Windows. 0 is "not asked": `io_list_files_stat`'s walk leaves it so, as
does `io_list_dir` without `IoListSizes`. On Windows a stat that opens no handle asks
`GetVolumeInformationW` of the path's volume, one call more; a listing asks once for the
directory.

**The timed wait (KR10).** `bool os_cond_timed_wait(OsCond* c, OsMutex* m, f64 seconds)`:
as `os_cond_wait`, false when the time ran out first. True is any wake, a spurious one
included, so the caller's loop is the same. On POSIX the condition is made on
`CLOCK_MONOTONIC` (`pthread_condattr_setclock`), so a change to the wall clock moves no
deadline; Apple has no such attribute and waits with `pthread_cond_timedwait_relative_np`.
Windows: `SleepConditionVariableSRW` with the milliseconds rounded up. A time of 0 or less
does not wait.

**C sources (KR14).** Blackwing's slice 3 vendors sqlite's amalgamation, which is C and
not C++ (`new` is a name in it; g++ stops at 740 errors), and kormake compiled only `.cc`
and `.mm`. Now `.c` in `src/` (and the platform's source directories) is a third language
beside them, with a unity file of its own:

- **The toolchain says how**: `cc`, `cflags` (and `cflags` in each config), and a `c`
  rule. Linux: `gcc -std=gnu11`, the C++ build's `-fno-strict-aliasing -ffp-contract=off
  -fno-math-errno`, and `-w`, since only vendored code is C. Apple and Android: `clang`,
  the same. Windows: `cl /TC /W0` with `/utf-8 /MD /fp:precise`. The probes give the C
  compiler beside the C++ one, with the same system flags (the SDK and target from xcrun,
  the NDK's `clang`, cl's external includes).
- **A module's own flags apply**: its includes, defines and `codegen` profiles;
  `codegen_sources` may name a `.c`. Sanitizer flags go on C as on C++.
- **A toolchain with no C** cannot build a module with a `.c`: generation stops and names
  the file.
- **The convention**: a vendored C library is wrapped by a `.c` of the module's own in
  `src/` that sets its options and includes it from `src/vendor/`, which is not looked in.
  C that is also C++ (yxml) stays behind a `.cc`.

## Stacked on: slice 3's close, the spec and slice 4

**Deep directories (KR15).** `io_make_dirs` copied its path into 1,024 bytes, and so did
`io_remove_tree`; both now take what the OS takes, on the stack when the path is short.
On Windows `os_make_dir`, `os_remove` and `os_remove_dir` widen through `WidePath`, as
the queue's opens do since KR4.

**The watcher and links (KR16).** A recursive watch never goes down a link to a
directory: what the library's walk does (KR11 marks links), the watcher does. Such a link
is not reported either, since there is no kind for it; a link to a file is the file, as
before. The alternative, an option to follow with the reported path saying it came
through a link, is not built; nothing asks for following.

**A cosmetic resource's fields (KR17).** `ResourceDesc::cosmetic`. With `fields` set it
says the table describes the object and nothing more: no state hash, no plain-data check
at publish, no save. `describe` and the script binding read it as any resource's.

**The event trace as data (KR23).** In dev builds, `engine_set_event_hook(e, fn, user)`:
`fn` is called with each event as it is delivered, a `DeliveredEvent` with its tick, its
type's id, name and field table (empty for a type without one), the emitter's name, the
boundary that merged it, and all its bytes; not the trace ring's 40. It runs where
`--trace-events` prints, on the main thread between phases, in the same order. One hook;
setting another replaces it, and null takes it away. In ship it returns false.

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
| KR10 | `os_cond_timed_wait` | P2 |
| KR11 | `io_list_dir`: one directory, links marked, hidden on request, sizes on request, names that round-trip, a reason on failure | P0 |
| KR12 | `io_path_is_remote`; `FsWatcher` polls a remote root under `Auto`, and a root can be polled by name | P2 |
| KR13 | `IoStat::volume` | P2 |
| KR14 | `.c` sources compiled as C, with each toolchain's `cc`, `cflags` and `c` rule | P0, blocks Blackwing slice 3 milestone 3 |
| KR15 | `io_make_dirs` and `io_remove_tree` take any path the OS takes | P2 |
| KR16 | A recursive watch does not follow links to directories | P1 |
| KR17 | `ResourceDesc::cosmetic`: fields that describe and are not hashed | P0, blocks Blackwing slice 4 milestone 4 |
| KR23 | `engine_set_event_hook`, dev builds | P2 |
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
7. **One directory** (KR11). Tests: a directory with files, a subdirectory, a hidden
   entry, a link to each and a dangling one, and a name that is not UTF-8 read back
   through the queue; each failure's errno.
8. **The volume** (KR13). Tests: a file and its directory agree, under `io_stat`, the
   ring's stat and the workers'; a mount point (here `/proc`, or a FUSE mount) differs
   from its parent.
9. **Shares** (KR12). Tests: a FUSE mount, where there is one, is remote and `/tmp` is
   not; a root added with `Poll` to a native watcher reports a change.
10. **The timed wait** (KR10). Tests: a wait that times out, one that is signalled, and 0.
11. **Deep directories** (KR15). A tree of 3,000 bytes made, written in and removed.
12. **The watcher and links** (KR16). A link inside a root to the root, and one to a
    directory outside it: a file written in the root is reported once, and one written
    outside not at all, under each backend this machine has.
13. **A cosmetic resource's fields** (KR17). Its object changes, the hash does not, and
    its fields are read by name.
14. **The event hook** (KR23). Events of two types, one without a field table, reach the
    hook in delivery order with their bytes.

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
5. **Where a link points (KR11, 4).** Blackwing asks that a link say what it points at,
   so a walk can refuse one that leaves its root or goes round a loop. `io_list_dir` says
   it is a link and what kind of thing is at the far end, not where that is. A walk can
   refuse every link today. The rest would be `io_real_path(path, out, cap)` (`realpath`,
   `GetFinalPathNameByHandleW`), which a walk compares with its root; the loop check is
   the same comparison against the directories above. Left for Blackwing to say whether
   it is wanted before its milestone 3.

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

**Milestone 6, `wrapv` (KR8): built Nov 22, 2026.**

- **`codegen` takes a list**, its profiles' flags in list order; a generated source's
  rule `codegen` takes one too, on the same path.
- **`codegen_sources`**: each named source leaves the unity file and compiles alone, with
  the module's `codegen` first and then the profile's flags; a source in two profiles gets
  both, and an `isa_sources` copy of it gets them too. A name that is not one of the
  module's sources is an error naming it. Paths are from the module's directory, as
  `isa_sources`' are.
- **`wrapv`** is `-fwrapv` in `toolchain-linux`, `toolchain-apple` and, beyond the text
  above, `toolchain-android` (clang, the same flag, so a module that asks for it builds
  there too). **For review, 3: picked, not settled.** `toolchain-windows` has
  `/d2UndefIntOverflow-`. The milestone meant to settle it by reading what cl generates,
  and there is no Windows machine here; its comment says it is not yet checked.
- **kormake's test**: the project's `codec` now has `codegen = { "fast" }` and
  `codegen_sources = { wrapv = { "src/decode.cc" } }` beside two other sources (the unity
  file holds those two, `decode.o` gets `-O3 -fwrapv`), and a `gen` with a name it does
  not have. The fixture's expected files do not change, and neither does the engine's
  `build.ninja`.
- **Blackwing, in a scratch copy** (its own checkout has work in progress, so nothing was
  changed there): `flac.cc` without its `#pragma GCC optimize("wrapv")` block and
  `hifi_codec` with `codegen_sources = { wrapv = { "src/flac.cc" } }`; `main.cc`'s no-op
  sink replaced by `nullptr`; the player's "the path is too long for the I/O queue, or the
  queue is full" now "the I/O queue is full". `kormake test` passes (`hifi_codec`, `hifi`,
  `blackwing_player`), and so do the same three under `--sanitize address,undefined` and
  `--sanitize thread`, with the engine's closure built so. The player's own `path[]` limit
  on a queued item (`player.cc`, "the path is too long") is Blackwing's and is left.

**Verification, Nov 22, 2026.** `kormake test` passes on linux-debug and linux-dev, and on
linux-ship but for `asset_live` (milestone 5 has it; it fails the same before this
slice's code). `tools/kormake/tests/run.sh` passes. `tools/doccheck.sh` reports what it did
before. The engine's `build/linux-dev/build.ninja` is what milestone 1 left. arcfire's and
arcanum's `--demo --hash --threads 16` give the same hashes, tick for tick, as at
`93d0e91` (arcfire 14,154 ticks, arcanum 1,865), at the default rate and at `--rate 4`;
arcfire's two recordings replay matched. Not done: `hifi_platform` against the real
PipeWire (no `libpipewire-0.3-dev` here), and anything on Windows or the Apple family.
The changes to Blackwing's own tree, made and tested in a copy at the time, are since
committed there (994a392) and pass in its own checkout.

**Milestone 7, one directory (KR11): built Nov 23, 2026.**

- **`io_list_dir(dir, flags, fn, user, &err)`** and `IoDirEntry`, as the text above has
  them, on `os_dir_list` in `os_file.h`, a listing that marks links and hidden entries and
  says why it failed. `io_list_files` and `os_dir_each_stat` are untouched.
- **POSIX** opens the directory once and asks `fstatat` against it: of a link (followed,
  so a link to nothing is kind None), of an entry typed `DT_UNKNOWN`, and of every entry
  with `IoListSizes`. Without the flag, sizes and times are 0 on every platform, Windows
  included, though its listing has them, so a caller that forgets the flag finds out
  everywhere.
- **Windows.** `os_dir_list` goes through `WidePath`, so a directory may be long; a link
  is a reparse point tagged symbolic link or mount point (a junction), and other reparse
  points (cloud placeholders, deduplicated files) are files. `wide.h` now speaks WTF-8
  both ways, in `src/wtf8.h`, which has nothing of Windows in it and is tested here; a
  UTF-16 name with a lone surrogate narrows to WTF-8 and widens back to itself. Valid
  names are as before. **Found on the way:** `WidePath::set` (milestone 3) called
  `errno_from_win32` above its declaration, which cl would have refused; it is moved up.
  Written, not compiled.
- **Tests** (`wtf8`, `io_list_dir`): the coder's round trips, measuring, a short buffer
  and four malformed inputs; a directory with a file, a hidden file, a subdirectory with
  its own entries, links to a file, a directory and nothing, and a name of bytes that are
  not UTF-8, listed in order without and with `IoListHidden | IoListSizes`, a.txt's time
  equal to `io_stat`'s; the odd name read through the queue; a stop; an empty directory;
  `ENOENT`, `ENOTDIR`, and `EACCES` for a directory with no permissions.
- **Checked:** `kormake test platform` on linux-debug; `platform` compiled for
  android-dev. Not checked: Windows, the Apple family.

**Milestone 8, the volume (KR13): built Nov 23, 2026.**

- **`IoStat::volume`**, and `OsStat::volume` under it: `st_dev` from `stat` and `fstatat`;
  from the ring, `makedev(stx_dev_major, stx_dev_minor)`, which statx always fills
  whatever the mask, so the ring's stat and `io_stat` agree. `io_list_dir` has it with
  `IoListSizes`, from the entry's own stat on POSIX.
- **Windows:** a stat that opens a handle (a link's) takes `dwVolumeSerialNumber`; one
  that does not asks `GetVolumePathNameW` and then `GetVolumeInformationW`, two calls more
  a stat, not measured. `os_dir_each_stat`, the walks' listing, does not ask, so a walk
  costs what it did; `io_list_dir` with sizes asks once a directory. Written, not compiled.
- **Tests:** in `io_long_ranged_stat`, under the synchronous backend, the ring and the
  workers, a file, its directory and the long path have `io_stat`'s volume, nothing has 0,
  and `/proc` (another file system) differs from `/`, through the queue as well;
  in `io_list_dir`, a.txt's volume with sizes and 0 without.
- **Checked:** `kormake test platform` on linux-debug. Not checked: Windows, the Apple
  family, and an unplugged disk: no disk here to unplug, so `/proc` under `/` stands in
  for a mount point with something on it.

**Milestone 9, shares (KR12): built Nov 23, 2026.**

- **`io_path_is_remote(path)`** on `os_path_is_remote`. Linux and Android: `statfs`'s
  magic number against fifteen file systems, the table's and kAFS, Lustre, OrangeFS, and
  the shared-disk GFS2 and OCFS2, compared as 32 bits since SMB's numbers have the top
  bit and `f_type` is signed on some ABIs. Apple: no `MNT_LOCAL`, or a macFUSE type name.
  Windows: the volume's path, then a UNC name or `DRIVE_REMOTE`. Written, not compiled,
  for Apple and Windows.
- **`FsWatcher`, per root.** `add_root(dir, how)` and `root_backend(id)` as the text above
  has them; `init` takes `remote_interval`, default 5 s. Each polled root keeps its own
  interval and last walk, so a share at 5 s and a root polled by name at 0.25 s go
  together under one native watcher. **Settled here:** only a watcher made with `Auto`
  polls a share by itself; one made with `Native` keeps it native, as asked. A watcher's
  polled roots are now walked an interval after each was added, not after `init`.
  `backend_name()` is still the watcher's.
- **Tests** (`fs_watch shares`): a temporary directory, "" and a missing path are not
  remote; this machine's document portal (`/run/user/<uid>/doc`, FUSE) is, an `Auto`
  watcher polls it and a `Native` one does not; a root polled by name and a native one
  under one watcher both report a new file, `Native` on a watcher without it fails, and
  a removed root reports nothing more. Run three times.
- **Checked:** `kormake test platform` on linux-debug. Not checked: a real NFS or SMB
  mount (none here), Windows, the Apple family.

**Milestone 10, the timed wait (KR10): built Nov 23, 2026.**

- **`os_cond_timed_wait(c, m, seconds)`** in `os_thread.h`. POSIX: every condition
  variable is now made on `CLOCK_MONOTONIC` (it changes nothing for `os_cond_wait`, which
  has no deadline), and the deadline is now plus `seconds` on that clock. Apple:
  `pthread_cond_timedwait_relative_np`. Windows: `SleepConditionVariableSRW` with the
  milliseconds rounded up and held below `INFINITE`; a failure that is not
  `ERROR_TIMEOUT` is a wake. A time over 10^9 seconds is cut to that; NaN, like 0, returns
  false at once.
- **Tests** (`cond timed wait`): 0 and -1 return false at once with the mutex held; a
  20 ms wait with nothing to wake it takes its 20 ms (a spurious wake is allowed, and
  must then be early); a wait that another thread signals after 10 ms returns true well
  inside its 5 s. Plain and under `--sanitize thread`.
- **Checked:** `kormake test core` on linux-debug, and under TSan; the POSIX source
  compiled for Android with the NDK alone, since `core` for android-dev does not build
  (milestone 4 has why). Not checked: Windows, the Apple family. Blackwing's `hifi`
  still naps; changing it is Blackwing's.

**Verification of milestones 7 to 10, Nov 23, 2026.** `kormake test` passes on
linux-debug and linux-dev, and on linux-ship but for `asset_live`, the same 9 checks as
before. `kormake test platform` and `kormake test core` pass under `address,undefined`
and under `thread`. `tools/kormake/tests/run.sh` passes, and `tools/doccheck.sh` reports
what it did before (`platform` names one fewer name missing from its page). arcfire's and
arcanum's demo hashes are still `93d0e91`'s, tick for tick, at the default rate and at
`--rate 4`. The engine's `build.ninja` gains `platform`'s new test source and nothing
else. Not done: Windows and the Apple family, where all four have code written and not
compiled; a real share; "For review", 5.

**`asset_live` in ship, Nov 23, 2026.** Its failure was the test's: `asset_live_startup`
turns the module off without `KV_DEV`, as its README says, and the test, written with
that guard (258a300), expected a connection in every config. In ship the test now checks
the opposite, that a listener named by `KV_GOLDBERG` is never called. `kormake test
asset_live` passes on linux-debug, linux-dev and linux-ship, so linux-ship's
`kormake test` has no known failure left.

**KR14, C sources: built Nov 23, 2026.** As above. kormake's fixture gains a C rule on its
host toolchain and two `.c` files in the test project's codec, whose expected Ninja file
shows the C unity file built with `c_host`, the module's profile on it, and archived with
the C++ objects; a host instance's build statements override `cc` and `cflags` beside
`cxx` and `cxxflags`. `tools/kormake/tests/run.sh` passes. The engine builds as before.
Linux built and used (Blackwing's sqlite); Apple, Android and Windows written, not
compiled: no machine here.

**Found with KR14: a test's test_deps, linked whole.** A module's heavy tests were built
without its `test_deps`, though the manual says they are linked as its tests are, and a
test's `test_deps` gave their libraries but not their externals (Blackwing's library tests
take the player, which takes PipeWire). Both now link through `link_inputs` with the
`test_deps` and their closures: libraries, externals and runtime files. The fixtures have
no such case, and their expected files are unchanged.

**Milestone 11, deep directories (KR15): built Nov 23, 2026.** As above. `io_remove_tree`
keeps its path in an `Array<char>` that grows a level at a time. Test
(`io_make_dirs_deep`): thirteen levels of 230 bytes, over 3,000 in all, made twice, a file
written in the deepest, `io_make_dirs` of that file refused, and the tree removed.
Checked on linux-dev and linux-debug. The Windows change is written, not compiled.

**Milestone 12, the watcher and links (KR16): built Nov 23, 2026.** As above, in every
backend:

- **The shared walk** (`fs_walk`: the polling backend, and the Windows and macOS
  backends' walks of a directory that appears) lists with `os_dir_list`, which marks
  links, and leaves out a link to a directory.
- **inotify** lists the same way when it adds a tree, watches nothing under a root with
  `IN_DONT_FOLLOW`, and drops a created or moved-in entry that is a link to a directory.
- **Windows and macOS** drop such an entry as it arrives. A new helper,
  `os_path_is_dir_link`, asks: `lstat` and `stat` on POSIX; on Windows the entry's own
  find record, whose reparse tag tells a link or junction from a cloud placeholder.
- **Its removal** may still come as `Removed` of a path never `Added`: inotify cannot say
  what a removed name was. The header says so.
- **Test** (`fs_watch links`, poll and native): a root holding a link to itself, a link
  under a subdirectory to a tree outside, and a link to a file outside; another link to
  the root made while it runs. A file written in the root is reported once by its own
  path; one written outside, and every link to a directory, not at all. Without the fix
  the test does not finish: two loops make a walk that doubles at each level.
- **Checked** on linux-dev and linux-debug. The Windows and macOS changes are written,
  not compiled.

**Milestone 13, a cosmetic resource's fields (KR17): built Nov 23, 2026.** As above.
`ResourceEntry` keeps the table in `fields` for whoever describes it and says in `hashed`
whether it is sim state; the hash and publish's plain-data check look at `hashed`. A
cosmetic table must still fit the published object. `cosmetic` with no field table is
refused at `engine_init`: it would mean nothing. `print /resources/<module>/<name>` now
shows any resource's field table, whether it is hashed, and the published values.

- **Not done:** reading a resource by its fields from Lua. `script` binds ktables only,
  and a resource binding belongs with KR18's events in slice 11.
- **Left alone:** the check that a unit reading a cosmetic column may not write sim
  state. It is not applied to cosmetic resources, which the sim may read as it may read
  any resource without fields. Blackwing's player is all cosmetic, so it would not
  notice; a game might.
- **Tests:** `hash` (a cosmetic resource whose object is not trivially copyable publishes;
  changing it leaves the state hash alone; its fields describe it; a cosmetic resource
  with no table is refused) and `cli_fs` (its fields and values printed, a resource
  without a table prints none).

**Milestone 14, the event hook (KR23): built Nov 23, 2026.** As above. The hook is called
from `engine_trace_delivery` beside the print, so it costs nothing when unset, and
nothing at all in ship, where the trace is compiled out. Test (`event hook`): a system
emits two notes with a field table and a 64-byte blob without one, over two ticks; the
hook sees four notes in emission order and two blobs with every byte, each named, with
its table, its emitter and `sim`; after `engine_set_event_hook(e, nullptr, ...)` it sees
nothing. In ship the call is refused and nothing arrives. Checked on linux-dev,
linux-debug and linux-ship.
