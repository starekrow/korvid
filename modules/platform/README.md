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
idle time: the module's idle task `platform.io` serves them from the slack the frame loop
hands `engine_idle_run`, sized to the time it is given. `docs/design/idle.md`.

**Sockets** (`net.h`). A listener accepts without blocking; a socket reads what has
arrived and queues what is to be sent, draining the queue when flushed. A queue that
grows past its limit drops writes and counts them rather than stall the frame. Loopback
by default. This is the CLI's transport and may be a game's.

**Watching** (`fs_watch.h`). An `FsWatcher` is given directory trees and polled, like a
socket: no thread, no blocking. inotify on Linux; everywhere, and as the only backend on
the other platforms for now, a polling backend that walks the trees at an interval and
compares sizes and modification times. Events are hints (added, changed, removed, a
directory come or gone, rescan): the reader compares content before believing one. The
header says what holds on both backends.

**Processes** (`process.h`). `Process::spawn` takes an argument vector, a working
directory and additions to the environment; `running`, `read_out`, `read_err` and `kill`
never block. The child has no stdin and its own process group, so killing it takes what
it started. POSIX only so far: on Windows `spawn` fails and says so. For tools.

**Directories.** `platform_base_path` is the executable's directory, read from
`/proc/self/exe` at startup; `platform_pref_path(e, org, app)` is the per-user directory
for saves and settings, `$XDG_DATA_HOME/<org>/<app>/` (or `~/.local/share/<org>/<app>/`),
created on first use. Both end in a separator. Each OS has its own source under `src/<os>/`.

**The pump.** `platform_pump(e)` runs the pump hooks other modules added
(`platform_pump_add`: `platform_hid` turns window events into bus events there), then the
I/O queue's own pump. One call per frame covers every platform module linked.

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
skipped) and `io_make_dirs(path)` is `mkdir -p`.
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
| `IoRequest` | record | `path` (copied), `alloc` (the data's home; null: general), `priority`, `user` |
| `IoCompletion` | record | `request`, `status`, `data`, `size`, `user`, `error` (errno) |
| `IoQueue` | record | `init(storage, max, backend)`, `free()`, `read_file(req)`, `read_asset(req)`, `mount_dir(dir)`, `mount_pack(path, prefix)`, `mount_pack_at(path, offset, size)`, `mounts`, `write_file(IoWriteRequest{path, data, size})`, `cancel(h)`, `pump()`, `pump_idle(bytes)`, `idle_bytes_for(seconds)`, `completions()`, `reap(h)`, `pending()`, `budget`, `backend_name()`; counters `pumps`, `pumps_capped`, `bytes_submitted`, `idle_bytes` |
| `platform_io(engine)` | function | the module's queue |
| `platform_pref_path(engine, org, app)` | function | the per-user directory for saves, created on first use, trailing separator; "" if none |
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
| `PlatformState` | record | `io`, `base_path`, `pref_path`, the pump hooks: the module's state, for code outside the schedule |
| `PlatformMounts`, `platform_configure_mounts(mounts)` | record, function | what startup mounts: `exe_name`, `packs`, `assets_dir`, `none`, `io_backend` |
| `platform_pump(engine)` | function | once per frame: the pump hooks, then the I/O queue's pump |
| `platform_pump_add(engine, fn, user)`, `platform_pump_remove` | functions | a device module's per-frame work, run by `platform_pump`; up to `KV_PLATFORM_PUMP_MAX` (8) |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `platform.io` | handle kind | an I/O request; `kv_handle_describe` prints its state and path |
| `platform.io` | idle task | serves idle reads and what the pumps held back |

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
