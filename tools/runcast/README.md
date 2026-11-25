# runcast

Serves a child process's output over a loopback socket, and fetches it. It exists as a
check on the platform layer more than as a tool: it was written from the public headers
and READMEs alone by someone who had never seen the engine, to find where a newcomer
trips, and it is kept because it uses most of that layer at once.

```
runcast serve PORT -- CMD ARGS...    # PORT 0: the OS picks; prints "port N"
runcast fetch PORT [FILE]            # prints what the server sends; with FILE, writes it and checks it
```

`serve` takes a lock (one server per PORT argument), listens, and when a client connects
starts CMD and streams its stdout and stderr down the socket. If the client leaves, the
child and everything it started are killed. It exits with the child's code. `fetch`
connects and prints until the server closes; with FILE it writes what it received through
the I/O queue, reads it back the same way and compares.

## What it shows

- **Nothing in it waits or polls.** Its one system does what can be done now and
  returns. A waker states what the run is waiting on in a `PlatformWatch` (the listener,
  the socket, the child, the queue), and the tool's loop sleeps until one stirs. An idle
  `serve` uses no CPU; `--verbose` prints how many ticks and waits the run took.
- **A socket's connect result**: `fetch` to a port nobody listens on says so, from
  `NetSocket::error` and `link_state`.
- **Backpressure**: the child is read only while the socket has `room()`.
- **`io_lock_file`**: a second `serve` on the same PORT names the holder.
- **Async file I/O**: `write_file`, `take`, `read_file`, `take`.

CMD is an argument vector, so it is not a shell's to read: `runcast serve 0 -- cmd /c echo hi`
runs whatever `echo` is on PATH, not cmd's builtin. `Process::shell` is for a shell's line.
