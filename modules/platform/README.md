# platform

The operating system as an engine sees it with no display and no devices: a file read
queue with its mounts, the executable's and the user's directories, and TCP sockets. It
links no external library, so a tool built on the engine (`kpack`) needs nothing
installed on the build machine beyond the compiler. The window, the keyboard and gamepads
are `platform_hid`'s; the audio device is `platform_audio`'s. Nothing here blocks the
thread that calls it, and nothing here runs a thread of its own: everything is pumped once
per frame, by the frame loop in a game and by the `tool` module in a tool.

## How it works

**Files** (`io.h`). A batched asynchronous read queue shaped like io_uring: submit any
time with a priority, the pump hands reads to the backend and collects completions, and
a completion sits on the list until its consumer reaps it and takes the data. Nothing
completes inline. The async backend is a ring: on Linux an io_uring driven by raw
syscalls, on Windows 11 the OS's I/O rings (`ioringapi.h`, version 3 or later for writes;
looked up in KernelBase by name, so an executable still starts on Windows 10). Either way
one submit per pump, completions reaped without waiting, short reads continued, cancels
honoured when the ring answers, and the same plain file handles the synchronous path
uses. Elsewhere (macOS), or when the OS refuses a ring, the pump performs the reads itself
in priority order. `IoBackend::Ring` asks for the ring whatever the OS calls it;
`backend_name()` says which ran: `io_uring`, `ioring`, `dispatch` (the Apple family: Grand
Central Dispatch's workers), `threads` (Android, whose apps are refused io_uring: four
threads of the ring's own) or `sync`. A pump is bounded in bytes, not requests
(8 MB per pump, 64 MB in flight by default), because the kernel does work at submission
in proportion to the size; what does not fit waits. `IoPriority::Idle` reads wait for
idle time: the module's Work unit `platform.io_idle` serves them in the Work cycle
(`docs/design/cycles.md`), sized to the time the cycle has left. `docs/design/idle.md`.

**Sockets** (`net.h`). A listener accepts without blocking; a socket reads what has
arrived and queues what is to be sent, draining the queue when flushed. A queue that
grows past its limit drops writes and counts them rather than stall the frame. Loopback
by default. This is the CLI's transport and may be a game's.

**Watching** (`fs_watch.h`). An `FsWatcher` is given directory trees and polled, like a
socket: no thread, no blocking. inotify on Linux; everywhere, and as the only backend on
the other platforms for now, a polling backend that walks the trees at an interval and
compares sizes and modification times. A link to a directory inside a root is neither
gone down nor reported, on either backend. Events are hints (added, changed, removed, a
directory come or gone, rescan): the reader compares content before believing one. The
header says what holds on both backends. The backend is chosen per root: on a share
(`io_path_is_remote`: NFS, SMB, any FUSE mount) the OS hears only this machine's writes,
so a watcher made with `Auto` polls such a root every `remote_interval` seconds (5), and
`add_root(dir, FsWatchBackend::Poll)` polls one the test misses; `root_backend(id)` says
which a root got. For a loop that waits, `wake_fd()` is inotify's descriptor (else -1) and
`due()` the time by which `poll()` should next be called for news no descriptor tells of.

**Processes** (`process.h`). `Process::spawn` takes an argument vector, a working
directory and additions to the environment; `running`, `read_out`, `read_err` and `kill`
never block. The child has no stdin and its own process group, so killing it takes what
it started. POSIX only so far: on Windows `spawn` fails and says so. For tools.

**Directories.** `platform_base_path` is the executable's directory, read from
`/proc/self/exe` at startup; `platform_pref_path(e, org, app)` is the per-user directory
for saves and settings, `$XDG_DATA_HOME/<org>/<app>/` (or `~/.local/share/<org>/<app>/`),
created on first use. `platform_user_dir(e, which, org, app)` names the three kinds apart,
where each OS keeps them: `Settings` (`$XDG_CONFIG_HOME`), `Data` (the preference
directory) and `Cache` (`$XDG_CACHE_HOME`, which backups leave out); `io.h` has the
table. All end in a separator.

**Settings** (`settings.h`). `platform_settings(e, org, app)` is the program's store:
named integers, numbers, strings and structs (by their field tables) in `settings.json`
in the settings directory, read whole when opened and written whole, off the calling
thread, soon after each set: to a temporary file, flushed, renamed over the old, so a
kill leaves the old file or the new one. A write holds `settings.lock` between processes
and merges what the file has by key, so two programs writing it lose nothing of each
other's. `settings_flush` waits for what was set; the platform's shutdown writes the rest. Each OS has its own source under `src/<os>/`.

**The pump.** `platform_pump(e)` runs the pump hooks other modules added
(`platform_pump_add`: `platform_hid` turns window events into bus events there), then the
I/O queue's own pump. One call per frame covers every platform module linked.

**The wait.** `platform_wait(e, seconds)` blocks till the window has something for the pump,
`platform_wake(e)` is called, or the time is up, and says which (`PlatformWoken`). It takes
nothing off any queue: the pump after it reads what came. The window's module sets the
wait (`platform_set_waiter`: SDL's `SDL_WaitEventTimeout` with no event to fill, woken by a
user event it pushes); with no window open it is a condition, timed. `platform_wake` may be
called from any thread, not a signal handler, and a wake that arrives before the wait
begins ends it at once. A frame loop with nothing to do waits here between passes
(`frameloop`); `platform_waits(e)` counts what ended them.

**Sockets that wake it.** `platform_wake_set(e, owner, fds, n)` states the sockets an owner
wants the loop woken by, replacing what it said before (`n` 0 forgets them): a watched
waker's (`engine_add_watched_waker`, the owner its `user`), the CLI's listener and
sessions. One thread of the platform's, started at the first socket and blocked in
`poll` (`WSAPoll` on Windows), calls `platform_wake` when one can be read, and leaves it
out till the next `platform_wait`, by when the pass it woke has read it: a socket nobody
drains wakes each wait, not the thread for ever. 64 sockets in all. `platform_watches(e)`
says whether the platform has the thread (every one does now); where it does not, a waker
registers with `engine_add_waker`, and the loop polls for it.

## Using it

**Declare the dependency**: `deps = { "core", "platform", ... }` in `module.lua`. The
frameloop module depends on it already, so a game gets it for free.

**Read a file.** Submit with the asset's handle as the user word, reap in a main-thread
system, free the data with the allocator you named:

```cpp
IoRequest req; req.path = full_path; req.alloc = &self.general; req.user = my_handle.bits;
Handle h = platform_io(engine)->read_file(req);

// later, in a system with .main_thread = true:
IoQueue* io = platform_io(*ctx.engine);
for (u32 i = 0; i < io->completions().len;) {
    IoCompletion c = io->completions()[i];
    if (!is_mine(c.user)) { ++i; continue; }   // other consumers' completions stay in the list
    io->reap(c.request);                       // the data is yours now
    if (c.status == IoStatus::Done) { use(c.data, c.size); free_in(&self.general, c.data, c.size); }
}
```

A preload goes in at `IoPriority::Idle` and loads only when the frame has time. The
asset module is the model consumer.

**Serve a socket.** Own a listener and the sockets it accepts; step them from a
main-thread system every tick:

```cpp
for (int fd; (fd = listener.accept()) >= 0;) sessions.push(...).sock.adopt(fd);
for (Session& s : sessions) {
    i32 n = s.sock.read(buf, sizeof buf);      // 0: nothing yet; -1: the peer closed
    if (n > 0) s.sock.write(buf, (u32)n);      // queued
    s.sock.flush();                            // sent, as much as the kernel takes
}
```

Throughput through a tick system is what the kernel buffers hold times the tick rate;
`sockit2m` prints the numbers.

**Writes** go through the same queue: `write_file(IoWriteRequest{path, data, size})` copies
the bytes at submission (the caller's buffer is free to go), creates or truncates the file
at the pump under the same priorities and budget as reads (`IORING_OP_WRITE` on the ring,
`fopen` on the fallback), and completes with the bytes written and no data. A save or a
settings file is a write to `platform_pref_path(e, org, app)`, the per-user directory
for the names given ($XDG_DATA_HOME, or ~/.local/share, then `<org>/<app>/`) (created on first use).

**A part of a file** is the same request with `offset` and `length`: `read_file` reads
`length` bytes from `offset` (0 for either: from the start, to the end), cut to the file
at open, so a range past the end completes `Done` with what is there and an offset past
it with 0 bytes. The budget counts the range, so the first 64 KB of a large file costs
64 KB. `read_asset` reads whole files only.

**Ask what a path is without waiting** with `stat_file(IoRequest{path})`: the completion's
`stat` has `is_file`, `is_dir`, `size`, `mtime_ns` and `volume`, as `io_stat` gives them, and no
data. Nothing there is `Done` with both flags false; `Failed` is an error on the way. On
io_uring it is one `IORING_OP_STATX`; where the ring has no stat (Windows, Apple, Android)
it runs on the task workers, and under `--io sync`, or with no workers started, at the
pump.

**Paths have no limit of the queue's.** One of 256 bytes or more is copied into a block
from the request's allocator and freed with the request; a path the OS refuses (past
4,096 bytes on Linux, or a name past 255) fails at the pump with the OS's error. On
Windows a path past `MAX_PATH` is opened under the `\\?\` prefix. Mounts are the
exception: a mount's own path is at most 255 bytes.

**Assets go through mounts.** `read_asset(IoRequest{path = "rocks/rock.png"})`
takes a logical path, normalizes it (`a//b`, `./a` and backslashes are `a/b`; `..` and
absolute paths fail with `EINVAL`) and looks it up in the queue's mounts, the last mounted
first: a directory (`mount_dir`) is a whole-file read of the file under it, a pack
(`mount_pack`, any zip: see `modules/pack`) is a ranged read on the pack's own descriptor,
inflated on delivery for a deflated entry and checked against its CRC-32 in debug and dev
builds. A path no mount has completes `Failed` with `ENOENT`; the completion looks the
same whichever answered. Mounting a pack reads its tail, central directory and index
synchronously and logs one line: entries, whether the index was used, how many entries
are deflated or unaligned. At startup the module mounts, in order, the program's loose
assets (the output tree goldberg cooked for it, named by `<exe>.assets` beside the
executable in a build tree; `<exe dir>/assets/` for a packaged program: `platform_assets_dir`),
`<exe dir>/<exe>.kpak`, then what `platform_configure_mounts` named (the frame loop's
`--pack FILE` and `--assets DIR`), so a pack overrides loose files and a mod overrides the
pack. A tool mounts nothing.

**Blocking files**, for tools, startup, shutdown and tests (a tick uses the queue):
`io_read_now(path, &bytes)` reads a whole file into an `IoBytes` (a NUL after the bytes,
so text is a C string; `free()` returns them), `io_write_now(path, data, size)` writes one
whole, and an `IoFileWriter` (`open`, `write`, `print`, `close`, a failure that sticks)
writes one in pieces. `io_is_file` and `io_is_dir` say what a path names. They stand on
the queue's own OS layer, UTF-8 paths and all, so nothing in the engine needs `<stdio.h>`
to touch a file. `io_list_files(dir, fn, user)` walks a directory (sorted, hidden entries
skipped) and `io_make_dirs(path)` is `mkdir -p`. `io_list_dir(dir, flags, fn, user, &err)`
lists one directory without its subdirectories, for a walk that picks its own order:
links marked, hidden entries with `IoListHidden`, sizes and times with `IoListSizes`
(without them, POSIX asks nothing of a file), and a reason when it fails. A name comes
back as the OS gave it, UTF-8 or not, and opens the same file.
`io_make_temp_dir(prefix, ...)` makes a fresh directory under the OS's temporary one, and
`io_remove_tree(path)` removes a directory and all under it (a link inside goes as a link,
never followed): a test's scratch, made and cleaned up.

## Quick reference

Header `korvid/platform/io.h`:

| name | kind | what |
|---|---|---|
| `IoPriority` | enum | `Idle` (idle time only), `Low`, `Normal`, `High` |
| `IoStatus` | enum | `Pending`, `Done`, `Failed`, `Cancelled` |
| `IoBackend` | enum | `Auto`, `Sync`, `Ring` (the async ring: io_uring, or Windows I/O rings): what `init` tries |
| `IoBudget` | record | `bytes_per_pump` (8 MB), `bytes_in_flight` (64 MB), `idle_bytes_per_call` (4 MB); 0 is unlimited |
| `IoRequest` | record | `path` (copied, any length), `alloc` (the data's home; null: general), `priority`, `user`, `offset` and `length` (a part of the file, for `read_file`; 0: to the end) |
| `IoStat` | record | `is_file`, `is_dir`, `size`, `mtime_ns`, `volume` (for equality: another disk at the same path differs; 0 where not asked): what a path names, following links |
| `IoDirEntry` | record | `name`, `st`, `is_link`, `hidden`, `utf8`: one entry of `io_list_dir` |
| `io_list_dir(dir, flags, fn, user, err)` | function | one directory's entries in name order; flags (`IoListFlags`) `IoListHidden`, `IoListSizes`; false with the errno |
| `IoCompletion` | record | `request`, `status`, `data`, `size`, `user`, `error` (errno), `stat` (a `stat_file`'s answer) |
| `IoQueue` | record | `init(storage, max, backend)`, `free()`, `read_file(req)`, `stat_file(req)`, `read_asset(req)`, `mount_dir(dir)`, `mount_pack(path, prefix)`, `mount_pack_at(path, offset, size)`, `mounts`, `write_file(IoWriteRequest{path, data, size})`, `cancel(h)`, `pump()`, `pump_idle(bytes)`, `idle_bytes_for(seconds)`, `completions()`, `reap(h)`, `pending()`, `budget`, `backend_name()`, `stat_on_tasks` (a test of the workers' stat); counters `pumps`, `pumps_capped`, `bytes_submitted`, `idle_bytes`, `stats_done`, `stats_failed` |
| `platform_io(engine)` | function | the module's queue |
| `platform_pref_path(engine, org, app)` | function | the per-user directory for saves, created on first use, trailing separator; "" if none (`UserDir::Data`) |
| `platform_settings(engine, org, app)`, `SettingsStore` | function, record | the program's settings store, opened in the settings directory at the first call |
| `settings_open_dir`, `settings_close`, `settings_flush` | functions | a store in a directory; closed (written); waited on |
| `settings_has`, `settings_int`, `settings_number`, `settings_string`, `settings_struct` | functions | a value, or the fallback; a struct's stored fields into it |
| `settings_set_int`, `settings_set_number`, `settings_set_string`, `settings_set_struct`, `settings_remove` | functions | a value set (written soon, off the thread), or gone |
| `platform_user_dir(engine, which, org, app)`, `UserDir` | function, enum | `Settings`, `Data` or `Cache`: where each OS keeps that kind, created on first use, trailing separator |
| `platform_base_path(engine)` | function | the executable's directory with a trailing separator; assets live beneath it |

Header `korvid/platform/net.h`:

| name | kind | what |
|---|---|---|
| `NetListener` | record | `open(port, local_only)` (port 0: the OS picks, see `port`), `close()`, `is_open()`, `accept()` (fd or -1) |
| `NetSocket` | record | `adopt(fd)`, `read(buf, cap)` (bytes, 0, or -1 closed), `write(data, len)`, `write_str(s)`, `flush()`, `close()`, `is_open()`, `out_limit`, `closed`, `bytes_in`, `bytes_out`, `dropped` |
| `net_connect_local(port, sock)` | function | a non-blocking connect to loopback, for tools and tests |

Header `korvid/platform/module.h`:

| name | kind | what |
|---|---|---|
| `platform_module` | `ModuleDesc` | the record; kormake puts it in the list |
| `PlatformState` | record | `io`, `base_path`, `pref_path`, the pump hooks, the waiter and its counts: the module's state, for code outside the schedule |
| `PlatformMounts`, `platform_configure_mounts(mounts)` | record, function | what startup mounts: `exe_name`, `packs`, `assets_dir`, `none`, `io_backend` |
| `platform_pump(engine)` | function | once per frame: the pump hooks, then the I/O queue's pump |
| `platform_pump_add(engine, fn, user)`, `platform_pump_remove` | functions | a device module's per-frame work, run by `platform_pump`; up to `KV_PLATFORM_PUMP_MAX` (8) |
| `platform_wait(engine, seconds)`, `PlatformWoken` | function, enum | blocks till the window has something, a wake, or the time: `Window`, `Wake`, `Time`; no time is no wait |
| `platform_wake(engine)` | function | ends the wait under way or the next; any thread |
| `platform_set_waiter(engine, w)`, `PlatformWaiter` | function, record | the window's `wait(user, seconds)` (1 early, 0 the time, -1 cannot now) and `wake(user)`; null clears |
| `platform_waits(engine)`, `PlatformWaits` | function, record | `waits`, and of them ended by the `window`, `woken`, `timed` |
| `platform_wake_set(engine, owner, fds, n)` | function | the owner's sockets that wake a waiting loop, replacing what it said; false where not watched or past 64 |
| `platform_watches(engine)` | function | whether `platform_wake_set` watches here |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `platform.io` | handle kind | an I/O request; `kv_handle_describe` prints its state and path |
| `platform.io_idle` | system, phase Work, main thread | serves idle reads and what the pumps held back, in the time the cycle has |

## Behaviour worth knowing

- **The pump is the heartbeat.** Window events, input and the I/O queue advance only in
  `platform_pump`, which the frame loop calls once per frame before the ticks. A tool
  that drives the engine itself must call it too, or nothing arrives (`tool_main` does).
- **Reads complete in the disk's order** within a priority, and in priority order only on
  the synchronous backend. Consumers find theirs by the user word.
- **The byte budget shapes the frame.** A pump's cost is proportional to the bytes it
  admits (a hot-cache read runs inside the submit); the default 8 MB is about 9 ms worst
  on an NVMe here against 34 ms unbounded. `pumps_capped` says how often the budget held
  something back.
- **Completions you do not reap are leaked at shutdown with a warning** naming the path.
- **Sockets never block and never stall.** A queue past `out_limit` drops and counts;
  `closed` says the peer went away. Served from a tick system, a connection's throughput
  is bounded by what the kernel buffers hold per tick.
- **No external libraries.** Keep it that way: a module that needs SDL, a GPU or a sound
  card is a module of its own (`platform_hid`, `platform_audio`, a renderer), so a tool
  that does not need one never links it.

## Not in this module

The window and input (`platform_hid`), the audio device (`platform_audio`), inflating on
the job pool (a deflated pack entry is inflated on the main thread at the pump),
unmounting, the ring for sockets, IPv6 and non-loopback listeners by default, an async
backend on macOS. Residency streaming sits above the queue, in the asset module and
whatever comes after it.

## A program's own files

On the desktop they are beside the executable; in an Apple bundle, in its resources; on
Android, inside the installed package, which is a zip the queue reads like any pack.

- `io_read_bundled(name, &bytes)` reads one such file now, by its name under the program's
  directory (`shaders/sprite.spv`): what must be read before the mounts exist.
- `mount_pack(path, "assets/")` mounts only an archive's entries under a prefix, named
  without it. `mount_pack_at(path, offset, size)` mounts an archive stored (uncompressed)
  as an entry of another, in place; `io_archive_entry` finds the offset.
- On Android the module's startup does both for the package: its `assets/assets/` is the
  first mount and `assets/<exe>.kpak` the game's pack. `platform_android_app`
  (`korvid/platform/android.h`) is how the app's entry says where the package is, and
  `platform_android_foreground` how it says the app left the screen.
