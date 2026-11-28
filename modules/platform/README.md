`user`, `owner`, `error` (errno, by `Err`'s names), `not_found()`, `stat` (a `stat_file`'s answer) |# platform

The operating system as an engine sees it with no display and no devices: a file read
queue with its mounts, the executable's and the user's directories, TCP sockets and Unix-domain ones. It
links no external library, so a tool built on the engine (`kpack`) needs nothing
installed on the build machine beyond the compiler. The window, the keyboard and gamepads
are `platform_hid`'s; the audio device is `platform_audio`'s. Nothing here blocks the
thread that calls it unless its name or its header says it is for tools (the blocking
file calls, `Process::wait`, and the sockets' waiting twins: `write_now`, `accept(seconds)`, `read(buf, cap, seconds)`, `net_connect_local_now`, and KR49's `net_connect_unix_now`, `write_now_with` and `net_wait`): everything else is polled or pumped once per frame, by the
frame loop in a game and by the `tool` module in a tool. The module's own threads are
few and never call a caller's code but to wake it: the queue's task workers once
`start_tasks` asks for them, the ring's workers where the OS has no ring of its own
(Apple, Android), one thread watching what a waiting loop is woken by, and the settings
store's writer.

**One shape for all of it.** What the OS opened is a `Handle`, the engine's own, as an
entity or a texture is: it knows what kind of thing it names, describes itself
(`kv_handle_describe`), and goes stale in every copy when what it names is closed. What is opened is closed (`open`, `close`, `is_open`). A blocking call that
can fail takes a last `i32* err`, an errno on every OS, compared by name (`Err::NotFound`,
`core/os_error.h`) and never by number. A read gives an `IoBytes`,
whether it was asked for now or later. What a waiting loop is woken by goes in a
`PlatformWatch`, whatever it is. Times are seconds, paths are UTF-8 with `/`.

## How it works

**Files** (`io.h`). A batched asynchronous read queue shaped like io_uring: submit any
time with a priority, the pump hands reads to the backend and collects completions, and
a completion sits on the list until its consumer reaps it and takes the data. Nothing
completes inline. The async backend is a ring: on Linux an io_uring driven by raw
syscalls, on Windows 11 the OS's I/O rings (`ioringapi.h`, version 3 or later for writes;
looked up in KernelBase by name, so an executable still starts on Windows 10). Either way
one submit per pump, completions reaped without waiting, short reads continued, cancels
honoured when the ring answers, and the same plain file handles the synchronous path
uses. The Apple family and Android have no such ring an app may use, so there the ring is
a few workers behind the same seam. When the OS refuses a ring, or `IoBackend::Sync` asks,
the pump performs the reads itself in priority order. `IoBackend::Ring` asks for the ring
whatever the OS calls it;
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

**Unix-domain sockets** (`net.h`, KR49) are for the other way of using a socket: a thread of
its own that speaks a local protocol (D-Bus, a desktop portal). `net_connect_unix_now`
reaches a name (a path, or Linux's abstract namespace), `net_socket_pair` makes two ends
in one process, `read_with` and `write_now_with` carry descriptors with the bytes
(`SCM_RIGHTS`), and `net_wait` sleeps on the socket and a `NetWaker` with no slices, so
an idle thread costs nothing until the peer speaks or another thread wakes it. Linux and
macOS; on Windows each says `EAFNOSUPPORT` until something there needs it.

**Watching** (`fs_watch.h`). An `FsWatcher` is given directory trees and polled, like a
socket: no thread of its own, no blocking. The native backend asks the OS (inotify on
Linux, ReadDirectoryChangesW on Windows, FSEvents on macOS); the polling backend, which
is all there is on iOS and Android and what any root can ask for, walks the trees at an
interval and compares sizes and modification times. A link to a directory inside a root is neither
gone down nor reported, on either backend. Events are hints (added, changed, removed, a
directory come or gone, rescan): the reader compares content before believing one. The
header says what holds on both backends. The backend is chosen per root: on a share
(`io_path_is_remote`: NFS, SMB, any FUSE mount) the OS hears only this machine's writes,
so a watcher made with `Auto` polls such a root every `remote_interval` seconds (5), and
`add_root(dir, FsWatchBackend::Poll)` polls one the test misses; `root_backend(id)` says
which a root got. A loop that waits adds the watcher to its `PlatformWatch` and is woken
when there is news; underneath, `wake_handle()` is inotify's descriptor (a `Handle`, else
null), `set_wake(fn, user)` has ReadDirectoryChangesW (a one-shot wait in the thread pool
on each root's read) or FSEvents (its queue's callback) call `fn` once till a poll takes
the news, and `due()` is the time by which `poll()` should next be called for what neither
tells of (a polled root's walk).

**Processes** (`process.h`). `Process::spawn` takes an argument vector, a working
directory and additions to the environment; `running`, `read_out`, `read_err` and `kill`
never block; `wait(seconds)` is for a thread that owns the child and has nothing else to
do. A process whose spawn failed and is used anyway reads as a child that printed nothing
and exited 0, so check spawn's answer (`error` says why). The child has no stdin and its own process group (a job object on Windows), so
killing it takes what it started. `process_supported()` is false only on iOS. For tools.

**Standard input** (`stdin.h`). `io_stdin_read` takes what has arrived on the process's
stdin without blocking, whatever it is (a pipe, a file, a terminal): the CLI's transport
when another program drives this one.

**Directories** (`paths.h`). `platform_base_dir` is the executable's directory (an Apple
bundle's resources), asked of the OS at startup. `platform_user_dir(e, which, org, app)`
is one of the three places a program keeps what is its user's, where each OS keeps that
kind, created on first use: `Settings`, `Data` (saves) and `Cache` (which backups leave
out); the header has the table for every OS. These end in a separator.
`platform_assets_dir` is where the program's loose assets are, and does not.

**Locks.** `io_lock_file(path, who, &lock, holder, cap, &err)` is a lock between
processes by a file's name, asked for without waiting: a tool that must be the only one
on its output tree. The OS lets go of it when the process ends, however it ends.

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

**What wakes it.** One way to say it, whatever the thing is, handed over in one of two
ways. A **standing watch**: each thing added once, watched until it closes, with no waker
and nothing stated again; the tick that follows a stir is the owner's systems' to read it
in.

```cpp
platform_watch_add(e, &me, listener);   // and the socket once accepted, the child once started
platform_unwatch(e, &me);               // at shutdown, before closing them
```

A **stated watch**: an owner with a waker (`engine_add_watched_waker`, the owner its
`user`) fills a `PlatformWatch` each time its waker is asked and hands it over, replacing
what it stated before. For a set worked out afresh each pass, or one with a time in it:

```cpp
PlatformWatch w(e);
w.add(listener);  w.add(sock);  w.add(watcher);  w.add(child);  w.add(queue);
w.at(next_retry);
platform_watch(e, &me, w);        // platform_unwatch(e, &me) at shutdown
```

An owner may have both. How each is heard is the platform's business. A socket or a
listener goes to one thread of the platform's, started at the first handle and blocked in
`poll` (`WSAPoll` on Windows), which calls `platform_wake` when one can be read and leaves
it out till the next `platform_wait`, by when the pass it woke has read it: a socket
nobody drains wakes each wait, not the thread for ever. An `FsWatcher` gives that thread
inotify's descriptor, or has the OS call (`ReadDirectoryChangesW`, FSEvents), and a polled
root gives the time of its next walk. A `Process`'s pipes go to the thread where a pipe can
be polled (POSIX), and its exit wherever the OS gives a handle for it: a pidfd (Linux from
5.3, Android) or a kqueue (Apple) on the thread, a one-shot wait in the thread pool on
Windows. The exit is heard once. An `IoQueue`'s task workers wake the loop as each task
finishes. A socket with output still queued, a read on the ring, and whatever this OS
cannot watch (a pipe on Windows, whose watch takes sockets only) are asked about again in
`kPlatformSoon` (4 ms) instead: the owner's code is the same on every OS, and only how
promptly it hears differs. The wait keeps to the time asked for on every OS (an
`OsEvent`, a high-resolution timer on Windows), so soon is 250 times a second. 64 handles
in all; past that, soon. A handle knows its kind (`os_handle_kind`), so the wrong one
cannot be watched by mistake: a file's is refused with an error line. A handle closed
while it is watched is stale, and left out: its number is never polled for whatever has
it next, and a standing member that closed is let go. Why it is shaped this way:
[waiting](../../docs/design/waiting.md).

**Alerts** (`alert.h`). An entity with an `Alertable` component names an OS handle, and is
sent an `Alerted` event in the Input phase every tick that handle has something to read;
a loop asleep is woken for it. The platform watches on the entity's behalf: the game
registers no waker. A closed handle is stale and asks for nothing. An alert is a wake,
not data: `Alertable` is a cosmetic component and nothing of it is recorded, so it is for
tools, UI and debug channels, not for gameplay that must replay (`docs/design/handles.md`,
"Alerts", has what would change that).

**Which to use.** A standing watch (`platform_watch_add`) for a module that waits on
things it opens and closes: a listener, its connections, a child, its exit; nothing to
state again, and closing ends it. A stated watch (a watched waker and `PlatformWatch`)
when what to wait on is worked out each pass, or includes a time (`at`), an
`FsWatcher`'s or an `IoQueue`'s. An `Alertable` when an entity reads one handle in a
handler, as an event. A tool's pump is the first kind
([tool](../tool/README.md#a-tool-that-waits) has it whole).

**Looking at it.** In a program with the CLI, `ls /dev/handles` lists the kinds of OS
handle, `ls /dev/handles/socket` each open socket with what it is, who
opened it and the bytes each way, and `print /dev/handles/file/7` one of them in full.
A handle opened outside any module's code is the program's main module's.

## Using it

**Declare the dependency**: `deps = { "core", "platform", ... }` in `module.lua`. The
frameloop module depends on it already, so a game gets it for free.

**Read a file.** Now, or later: the two read alike. Both give an `IoBytes` (the file, a NUL
after it, the allocator it came from), freed with `free()`:

```cpp
IoBytes b;                                       // now: tools, startup, tests
if (io_read_file(path, &b)) { use(b.data, b.size); b.free(); }

IoRequest req; req.path = path;                  // later: anything in a tick
Handle h = platform_io(engine)->read_file(req);
// ... in a main-thread system, each tick until it answers:
IoCompletion c;
if (platform_io(engine)->take(h, &c)) {          // false while it is on its way
    if (c.status == IoStatus::Done) use(c.bytes.data, c.bytes.size);
    c.bytes.free();                              // empty on a failure: frees nothing
}
```

A consumer of many requests (the asset module) submits them under an owner, any address
that is its own, and gets a list of its own: `completions(owner)` holds what is its and
nobody else's, so no module walks another's completions. At its shutdown it says
`drain(owner)`: what it has out is cancelled and waited for, and what came is freed.

```cpp
IoRequest req; req.path = full_path; req.alloc = &self.general; req.user = my_handle.bits; req.owner = &state;
platform_io(engine)->read_file(req);

IoQueue* io = platform_io(*ctx.engine);
for (u32 i = 0; i < io->completions(&state).len;) {
    IoCompletion c = io->completions(&state)[i];
    if (!ready_for(c.user)) { ++i; continue; } // stays on the list for a later pass
    io->reap(c.request);                       // the bytes are yours now
    if (c.status == IoStatus::Done) use(c.bytes.data, c.bytes.size);
    else if (c.not_found()) missing();         // by name: c.error == Err::NotFound
    c.bytes.free();
}
```

A preload goes in at `IoPriority::Idle` and loads only when the frame has time. The
asset module is the model consumer.

**Serve a socket.** Own a listener and the sockets it accepts; step them from a
main-thread system every tick:

```cpp
for (Handle h; !(h = listener.accept()).is_null();) sessions.push(...).sock.adopt(h);
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
at the pump under the same priorities and budget as reads (`IORING_OP_WRITE` on io_uring;
the blocking layer's own write on the synchronous path), and completes with the bytes written and no data. A save or a
settings file is a write to `platform_user_dir(e, UserDir::Data, org, app)`, the per-user directory
for the names given ($XDG_DATA_HOME, or ~/.local/share, then `<org>/<app>/`) (created on first use).

**A part of a file** is the same request with `offset` and `length`: `read_file` reads
`length` bytes from `offset` (0 for either: from the start, to the end), cut to the file
at open, so a range past the end completes `Done` with what is there and an offset past
it with 0 bytes. The budget counts the range, so the first 64 KB of a large file costs
64 KB. `read_asset` reads whole files only.

**Ask what a path is without waiting** with `stat_file(IoRequest{path})`: the completion's
`stat` has `kind` (`is_file()`, `is_dir()`), `size`, `mtime_ns` and `volume`, as `io_stat_file` gives them, and no
data. Nothing there is `Done` with `IoKind::None`; `Failed` is an error on the way. On
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
`io_read_file(path, &bytes)` reads a whole file into an `IoBytes` (a NUL after the bytes,
so text is a C string; `free()` returns them), `io_write_file(path, data, size)` writes one
whole, and an `IoFileWriter` (`open`, `write`, `print`, `close`, a failure that sticks)
writes one in pieces. `io_is_file` and `io_is_dir` say what a path names. Each that can fail
takes a last `i32* err`: an errno on every OS, set on failure, with `Err`'s names for the
common ones (the numbers differ from OS to OS; `os_error_name` and `os_error_text` say one).
A dev or debug build warns when a system of a paced loop (the frame loop's) makes one of
these calls, once for the system and the call: a tick asks the queue. They stand on
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
| `IoStat`, `IoKind` | record, enum | `kind` (`None`, `File`, `Dir`, `Other`), `is_file()`, `is_dir()`, `size`, `mtime_ns`, `volume` (for equality: another disk at the same path differs; 0 where not asked): what a path names, following links |
| `IoDirEntry` | record | `name`, `st`, `is_link`, `hidden`, `utf8`: one entry of `io_list_dir` |
| `io_list_dir(dir, flags, fn, user, err)` | function | one directory's entries in name order; flags (`IoListFlags`) `IoListHidden`, `IoListSizes`; false with the errno |
| `IoCompletion` | record | `request`, `status`, `bytes` (a read's `IoBytes`: `data`, `size`, a NUL after, `free()`), `written`, `user`, `error` (errno), `stat` (a `stat_file`'s answer) |
| `IoQueue` | record | `init(storage, max, backend)`, `free()`, `read_file(req)`, `stat_file(req)`, `read_asset(req)`, `mount_dir(dir)`, `mount_pack(path, prefix)`, `mount_pack_at(path, offset, size)`, `mount_count()`, `mount_table(i)`, `capacity()`, `write_file(IoWriteRequest{path, data, size})`, `cancel(h)`, `pump()`, `pump_idle(bytes)`, `idle_bytes_for(seconds)`, `take(h, &c)`, `is_pending(h)`, `completions(owner)`, `completed_count()`, `reap(h)`, `pending()`, `pending(owner)`, `drain(owner)`, `budget`, `backend_name()`, `stat_on_tasks` (a test of the workers' stat); counters `pumps`, `pumps_capped`, `bytes_submitted`, `idle_bytes`, `stats_done`, `stats_failed` |
| `io_read_file(path, &bytes, alloc, &err)`, `io_write_file`, `io_copy_file`, `io_stat_file`, `io_read_each` | functions | the blocking twins of the queue's requests, for tools, startup and tests |
| `io_make_dirs`, `io_remove`, `io_remove_tree`, `io_rename`, `io_make_temp_dir`, `io_list_files`, `io_current_dir`, `io_is_file`, `io_is_dir` | functions | directories and names, blocking; each that can fail takes a last `i32* err` |
| `io_lock_file(path, who, &lock, holder, cap, &err)`, `io_unlock_file(lock)` | functions | a lock between processes by a file's name, asked for without waiting; `EWOULDBLOCK` and the holder's line when another has it |
| `platform_io(engine)` | function | the module's queue |
| `platform_settings(engine, org, app)`, `SettingsStore` | function, record | the program's settings store, opened in the settings directory at the first call |
| `settings_open_dir`, `settings_close`, `settings_flush` | functions | a store in a directory; closed (written); waited on |
| `settings_has`, `settings_int`, `settings_number`, `settings_string`, `settings_struct` | functions | a value, or the fallback; a struct's stored fields into it |
| `settings_set_int`, `settings_set_number`, `settings_set_string`, `settings_set_struct`, `settings_remove` | functions | a value set (written soon, off the thread), or gone |

Header `korvid/platform/paths.h`:

| name | kind | what |
|---|---|---|
| `platform_user_dir(engine, which, org, app)`, `UserDir` | function, enum | `Settings`, `Data` or `Cache`: where each OS keeps that kind, created on first use, trailing separator |
| `platform_base_dir(engine)`, `platform_exe_dir(out, cap)` | functions | the executable's directory with a trailing separator; assets live beneath it. The second asks the OS, for code with no engine |
| `platform_assets_dir(out, cap)` | function | the program's loose assets, no trailing separator |

Header `korvid/platform/os_handle.h`:

| name | kind | what |
|---|---|---|
| `OsKind` | enum | `None`, `File`, `Socket`, `Pipe`, `Notify`, `Process`: what sort of OS object a `Handle` names |
| `os_handle_kind(h)`, `os_handle_open(h)` | functions | the kind, or `None` for a handle that is null, stale or not the OS's; whether it is still open |
| `os_handle_info(h, &info)`, `OsHandleInfo` | function, record | dev and debug builds: what was opened (`what`), by which module (`by`), when, and `bytes_in`, `bytes_out` |
| `os_handles_open()`, `os_handles_each(fn, user)` | functions | how many the process has open through the platform, and each in turn |

Header `korvid/platform/alert.h`:

| name | kind | what |
|---|---|---|
| `Alertable` | component (cosmetic) | `source` (the OS handle to hear about; null: none), `alerts` (how many so far) |
| `Alerted` | event | `entity`, `source`: the handle has something to read; every tick it has, in the Input phase |

Headers `korvid/platform/fs_watch.h`, `process.h`, `stdin.h`:

| name | kind | what |
|---|---|---|
| `FsWatcher` | record | `open(backend, poll_interval, remote_interval, &err)`, `close()`, `is_open()`, `add_root(dir, how, &err)`, `remove_root(id)`, `poll(now)`, `backend_name()`, `root_backend(id)`, `lateness()`, `wake_handle()`, `set_wake(fn, user)`, `due()` |
| `Process`, `ProcessDesc` | records | `spawn(desc)` (`error` says why not), `running()`, `read_out`, `read_err`, `wait(seconds)`, `kill()`, `close()`, `is_open()`, `code` |
| `io_stdin_read(buf, cap)`, `io_stdin_is_terminal()`, `io_stdin_handle()` | functions | the process's stdin without blocking; whether it is a terminal; what a `PlatformWatch` watches for it |

Header `korvid/platform/net.h`:

| name | kind | what |
|---|---|---|
| `NetListener` | record | `open(port, scope, &err)` (port 0: the OS picks, see `port`; `NetScope::Loopback` or `Any`; `error` keeps why it failed), `close()`, `is_open()`, `accept()` (a `Handle`, or null), `accept(seconds)` (waits for one) |
| `NetSocket` | record | `adopt(handle)`, `read(buf, cap)` (bytes, 0, or -1 closed), `read(buf, cap, seconds)` (waits for bytes or the end), `write(data, size)`, `write_str(s)`, `write_now(data, size, seconds)` (blocks till the kernel has it all; false: over, or with `seconds`, the time ran out and the rest is queued), `flush()`, `pending()`, `room()`, `close()`, `is_open()`, `link()`, `out_limit`, `closed`, `error`, `bytes_in`, `bytes_out`, `dropped` |
| `net_connect_local(port, &sock)`, `net_connect_local_now(port, &sock, seconds, &err)` | functions | a connect to loopback, for tools and tests: non-blocking, or waiting for the answer (false: refused, or the time ran out) |
| `net_connect_unix_now(name, NetUnixName::Path or Abstract, &sock, seconds, &err)`, `net_socket_pair(&a, &b, &err)` | functions | a Unix-domain socket by name (ENOENT, ECONNREFUSED, ENAMETOOLONG; Abstract is Linux's), or two connected ends (POSIX; EAFNOSUPPORT on Windows) |
| `NetSocket::read_with(buf, cap, handles, cap, &got)`, `write_now_with(data, size, handles, count, seconds)`, `handles_dropped` | methods, field | descriptors across a Unix-domain socket: those received become handles (a socket, a pipe, else a file) the caller owns; at most `kNetMaxHandles` (16) a time |
| `NetWaker` (`open`, `close`, `wake`, `is_open`), `net_wait(sock, want_write, &waker, seconds)`, `kNetReadable`, `kNetWritable`, `kNetWoken` | record, function, constants | a thread's exact wait on its socket, ended by another thread's `wake()`: what happened, or 0 when the time ran out |

Header `korvid/platform/module.h`:

| name | kind | what |
|---|---|---|
| `platform_module` | `ModuleDesc` | the record; kormake puts it in the list |
| `PlatformMounts`, `platform_configure_mounts(mounts)` | record, function | what startup mounts: `exe_name`, `packs`, `assets_dir`, `none`, `io_backend` |
| `platform_pump(engine)` | function | once per frame: the pump hooks, then the I/O queue's pump |
| `platform_pump_add(engine, fn, user)`, `platform_pump_remove` | functions | a device module's per-frame work, run by `platform_pump`; up to `KV_PLATFORM_PUMP_MAX` (8) |
| `platform_wait(engine, seconds)`, `PlatformWoken` | function, enum | blocks till the window has something, a wake, or the time: `Window`, `Wake`, `Time`; no time is no wait |
| `platform_wake(engine)` | function | ends the wait under way or the next; any thread |
| `platform_set_waiter(engine, w)`, `PlatformWaiter` | function, record | the window's `wait(user, seconds)` (1 early, 0 the time, -1 cannot now) and `wake(user)`; null clears |
| `platform_waits(engine)`, `PlatformWaits` | function, record | `waits`, and of them ended by the `window`, `woken`, `timed` |
| `platform_watch_add(engine, owner, thing)`, `platform_watch_remove(engine, owner, thing)` | functions | a standing watch: a listener, a socket, a `Process` (pipes and exit) or a `Handle`, watched till it closes, no waker |
| `PlatformWatch`, `platform_watch(engine, owner, w)`, `platform_unwatch(engine, owner)` | record, functions | what an owner's waiting loop is woken by: `add` a listener, a socket, an `FsWatcher`, a `Process`, an `IoQueue` or a `Handle`, `at(when)`, `soon()`; replaces what the owner stated (its standing members stay); `platform_unwatch` forgets both; what this OS cannot watch is asked again in `kPlatformSoon` |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `platform.io` | handle kind | an I/O request; `kv_handle_describe` prints its state and path |
| `os.file`, `os.socket`, `os.pipe`, `os.notify`, `os.process` | handle kinds | what the OS has open, registered once for the process at the first one opened; `kv_handle_describe` prints what it is and, in dev and debug, who opened it and the bytes each way |
| `platform.alerts` | system, phase Input, main thread | asks the OS about every `Alertable`'s source and emits `Alerted` |
| `Alertable`, `Alerted` | component, event | `alert.h` |
| `/dev` | CLI mount | `/dev/handles/<kind>/<slot>`: the open OS handles |
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
  An owner that is going away says `drain(owner)` and leaves none.
- **Sockets never block and never stall**, but for `write_now`. A queue past `out_limit`
  drops and counts; `closed` says the peer went away. Served from a tick system, a
  connection's throughput is bounded by what the kernel buffers hold per tick.
- **Straight-line code** has its blocking calls, each given a time: `net_connect_local_now`
  says whether anybody answered, `accept(seconds)` waits for a connection, `read(buf, cap,
  seconds)` for bytes or the end, `write_now` sends everything or says the connection is
  over (or, given `seconds`, that the peer is not reading), `Process::wait` sleeps till the
  child writes or exits, `io_read_file` and `io_write_file` block. In a tool, run it with
  `tool_task` and the engine goes on ticking beside it; there, it is as fast as code gets
  (a child streamed at 100 MB/s on Windows, against 28 for a pump paced by the loop). A
  loop of `write` and `flush` that retries until the bytes are taken never ends once the
  peer has gone: `write` refuses a closed socket for good.
- **Used after it is over, a socket, listener or process tells you**, in a dev or debug
  build: a warning, once per object, when code writes to a connection that is over, reads
  a socket after `close()`, accepts on a listener whose open failed (with the reason), or
  reads, polls, waits on or kills a process that is not running (with spawn's `error`).
  The calls still return quietly (false, 0, -1), as they do in ship.
- **No external libraries.** Keep it that way: a module that needs SDL, a GPU or a sound
  card is a module of its own (`platform_hid`, `platform_audio`, a renderer), so a tool
  that does not need one never links it.

## Not in this module

The window and input (`platform_hid`), the audio device (`platform_audio`), inflating on
the job pool (a deflated pack entry is inflated on the main thread at the pump),
unmounting, the ring for sockets, IPv6, UDP and connections to another machine, and one
wait over every kind of handle on Windows (a pipe is asked about again soon instead of
watched: `PlatformWatch`). Residency streaming sits above the queue, in the asset module
and whatever comes after it.

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
