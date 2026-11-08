# platform

The operating system, as the engine sees it: a window, the keyboard and gamepads, a file
read queue, and TCP sockets. SDL3 is behind the window and the input and nowhere else;
the I/O queue is io_uring on Linux with a synchronous fallback; the sockets are POSIX.
Nothing here blocks the thread that calls it, and nothing here runs a thread of its own:
everything is pumped once per frame by the frame loop, and the state it produces is
published as resources that systems declare and read.

## How it works

**The window** (`host_window.h`). One per process, opened on request by the frameloop
module and closed at platform shutdown, never by the caller. It is a service, not an
entity. `platform_pump` turns OS events into bus events (`WindowResized`,
`FocusChanged`, `CloseRequested`, key, mouse and gamepad edges) and updates the raw input
state. A Vulkan window hands its native handle to the renderer; a software window can
only `present_blank`.

**Input** (`input.h`, `events.h`). Two copies of the state: `raw`, updated by the pump at
frame rate, and `sampled`, copied once per tick by the system `input.sample` (phase
Input) so the simulation sees one consistent picture per tick. Keys are engine codes
mapped from SDL scancodes; pads follow SDL's standard layout (South is A or Cross), with
a dead zone on the sticks and triggers in [0, 1]. Pads open when they arrive and close
when they leave; up to four. Losing focus releases every key and button so nothing stays
pressed.

**Files** (`io.h`). A batched asynchronous read queue shaped like io_uring: submit any
time with a priority, the pump hands reads to the backend and collects completions, and
a completion sits on the list until its consumer reaps it and takes the data. Nothing
completes inline. On Linux the backend is an io_uring driven by raw syscalls (one
enter per pump, completions reaped without waiting, short reads continued, cancels
honoured when the ring answers); elsewhere, or when the kernel refuses a ring, the pump
performs the reads itself in priority order. A pump is bounded in bytes, not requests
(8 MB per pump, 64 MB in flight by default), because the kernel does work at submission
in proportion to the size; what does not fit waits. `IoPriority::Idle` reads wait for
idle time: the module's idle task `platform.io` serves them from the slack the frame loop
hands `engine_idle_run`, sized to the time it is given. `docs/design/idle.md`.

**Sockets** (`net.h`). A listener accepts without blocking; a socket reads what has
arrived and queues what is to be sent, draining the queue when flushed. A queue that
grows past its limit drops writes and counts them rather than stall the frame. Loopback
by default. This is the CLI's transport and may be a game's.

## Using it

**Declare the dependency**: `deps = { "core", "platform", ... }` in `module.lua`. The
frameloop module depends on it already, so a game gets it for free.

**Read input in a system.** Name the resource in the system's sets and reach it through
the context:

```cpp
void control(TickContext& ctx) {
    static const u32 k_input = ctx.resource_id("input.state");
    const Input* in = ctx.resource<const Input>(k_input);
    bool left = in->sampled.down(Key::Left);
    if (const GamepadState* pad = in->sampled.pad()) {        // the first connected pad, or null
        left |= pad->axis(PadAxis::LeftX) < -0.5f || pad->down(PadButton::DpadLeft);
    }
}
constinit const char* const k_control_reads[] = {"input.state"};
```

Key edges arrive as events too: `for (const KeyDown& k : ctx.events->read<KeyDown>())`.
The window is `ctx.resource<const HostWindow>("platform.window")` for its size.

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
`example_sockit2m` prints the numbers.

**Options the frame loop exposes**: `--borderless`, `--no-vsync`, `--cli-port N`,
`--cli-stdin`, `--idle-margin-ms N`, `--no-idle`.

## Quick reference

Header `korvid/platform/host_window.h`:

| name | kind | what |
|---|---|---|
| `WindowDesc` | record | `title`, `width`, `height`, `vsync` (a preference the renderer reads), `surface` (`Software`, `Vulkan`), `borderless` |
| `HostWindow` | record | `open(desc)`, `close()`, `is_open()`, `width()`, `height()`, `vsync()`, `focused()`, `pump(bus, input)`, `present_blank()`, `native_handle()` |

Header `korvid/platform/input.h`:

| name | kind | what |
|---|---|---|
| `Key` | enum | letters, digits, F keys, `Escape`, `Space`, `Enter`, `Tab`, `Backspace`, arrows, modifiers; `key_name` |
| `PadButton`, `PadAxis` | enums | the standard layout; `pad_button_name`, `pad_axis_name` |
| `GamepadState` | record | `buttons` (bits), `axes`, `connected`, `id`; `down(b)`, `axis(a)` |
| `MouseButton`, `MouseState` | enum, record | `x`, `y` in framebuffer pixels (origin top-left), `wheel` (the scroll since the last sample), `buttons`; `down(b)`; `mouse_button_name` |
| `mouse_screen(x, y, w, h, &sx, &sy)` | function | window pixels to screen space: center origin, +y up, what screen-space sprites use |
| `InputState` | record | `down(key)`, `set(key, on)`, `clear()`, `pads[KV_MAX_GAMEPADS]`, `pad()` (the first connected), `mouse` |
| `Input` | record | `raw`, `sampled`, `sample()` |
| `key_from_scancode`, `scancode_from_key` | functions | the SDL mapping, both ways |
| `KV_MAX_GAMEPADS`, `KV_PAD_DEAD_ZONE` | constants | 4; 0.15 |

Header `korvid/platform/events.h`: `WindowResized{width, height}`, `FocusChanged{focused}`,
`CloseRequested`, `KeyDown{key, device, repeat}`, `KeyUp{key, device}`,
`MouseMoved{x, y, dx, dy}`, `MouseButtonDown{button, x, y}`, `MouseButtonUp{button, x, y}`,
`MouseWheel{dx, dy}`, `GamepadConnected{pad}`, `GamepadDisconnected{pad}`,
`PadButtonDown{button, pad}`, `PadButtonUp{button, pad}`.

Header `korvid/platform/io.h`:

| name | kind | what |
|---|---|---|
| `IoPriority` | enum | `Idle` (idle time only), `Low`, `Normal`, `High` |
| `IoStatus` | enum | `Pending`, `Done`, `Failed`, `Cancelled` |
| `IoBackend` | enum | `Auto`, `Sync`, `Uring`: what `init` tries |
| `IoBudget` | record | `bytes_per_pump` (8 MB), `bytes_in_flight` (64 MB), `idle_bytes_per_call` (4 MB); 0 is unlimited |
| `IoRequest` | record | `path` (copied), `alloc` (the data's home; null: general), `priority`, `user` |
| `IoCompletion` | record | `request`, `status`, `data`, `size`, `user`, `error` (errno) |
| `IoQueue` | record | `init(storage, max, backend)`, `free()`, `read_file(req)`, `cancel(h)`, `pump()`, `pump_idle(bytes)`, `idle_bytes_for(seconds)`, `completions()`, `reap(h)`, `pending()`, `budget`, `backend_name()`; counters `pumps`, `pumps_capped`, `bytes_submitted`, `idle_bytes` |
| `platform_io(engine)` | function | the module's queue |
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
| `PlatformState` | record | `input`, `window`, `io`, `base_path`: the module's state, for code outside the schedule |
| `platform_open_window(engine, desc)`, `platform_window(engine)` | functions | the one window; null if open already or refused |
| `platform_input(engine)` | function | the input, for code outside the schedule |
| `platform_pump(engine)` | function | once per frame: OS events to the bus, raw input, the I/O queue's pump |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `WindowResized`, `FocusChanged`, `CloseRequested`, `KeyDown`, `KeyUp`, `MouseMoved`, `MouseButtonDown`, `MouseButtonUp`, `MouseWheel`, `GamepadConnected`, `GamepadDisconnected`, `PadButtonDown`, `PadButtonUp` | event types | no schemas; none carries an entity |
| `input.state`, `platform.window` | resources, published | `Input` and `HostWindow`; read them through `ctx.resource<const T>` |
| `input.sample` | system, phase Input, `Always` | writes `input.state`: raw becomes sampled, on paused ticks too |
| `platform.io` | handle kind | an I/O request; `kv_handle_describe` prints its state and path |
| `platform.io` | idle task | serves idle reads and what the pumps held back |

## Behaviour worth knowing

- **The pump is the heartbeat.** Window events, input and the I/O queue advance only in
  `platform_pump`, which the frame loop calls once per frame before the ticks. A tool
  that drives the engine itself must call it too, or nothing arrives.
- **Sampled input is per tick, raw is per frame.** A system sees `sampled`; render-rate
  code may read `raw`. A key pressed and released between two ticks is seen only as its
  `KeyDown` and `KeyUp` events.
- **Mouse resizing on GNOME Wayland stalls the pump** for the whole drag: SDL's libdecor
  repaints the frame inside every configure. Keyboard resizes and borderless windows are
  fine; `host_window.h` has the diagnosis and the remedy.
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
- **`SDL_VIDEO_DRIVER=dummy`** lets the module start without a display, for tools and
  tests; `example_sockit2m` sets it.

## Not in this module

Mouse and touch, text input, more than one window, audio, controller rumble, file
writes, directory listing, pack files, io_uring for sockets, IPv6 and non-loopback
listeners by default, Windows and macOS. Residency streaming sits above the queue, in
the asset module and whatever comes after it.
