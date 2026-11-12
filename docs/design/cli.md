# CLI

Nov 7, 2026. The command line as a module: what a command is, how one reaches the engine,
and the line protocol.

## Commands are registration data

A module declares what it can be told in its `ModuleDesc::commands`: a name of one or
more words, a help line, and a function taking a `CliContext` and the arguments. The
`cli` module collects every table at startup in module order. A request is matched to the
command whose name has the most words in common with the request's first words
(`ls materials` would beat `ls`), and the rest are the arguments. Whoever reads a format
owns its commands and its mounts: the asset module mounts `/materials` and friends;
`help`, `quit`, the sim controls `pause`, `resume`, `step [N]` and `rate [X]`, and the
filesystem verbs live in `cli`. The CLI's own system runs on paused ticks, so a paused
game still answers.

A command answers through `ctx.print`, one line at a time, and `ctx.fail` marks the
request failed with a message; the first failure stands. Commands never block and never
allocate outside the scratch arena.

## The filesystem

Everything the game has shows up as a path, the way a Unix system puts the kernel in
`/proc`. `ls /` names the top level; a session has a working directory; `cd`, `pwd`,
`print` and `patch` take absolute or relative paths with `.` and `..`. The tree is
virtual: a module mounts a piece of it by declaring a `CliMountDesc` in its
`ModuleDesc::mounts` (`path`, `list`, `describe`, `patch`), and the cli module owns the
working directory, the path rules and the verbs. Engine names map their dots to slashes
on the way in, so a module is a directory wherever its things appear.

    > ls /
    buses/  clips/  clock/  events/  fonts/  handlers/  kinds/  materials/  mem/
    modules/  perf/  resources/  sounds/  systems/  textures/  triggers/  voices/
    > cd /kinds/rocks
    > ls
    bullet/   14 live of 4096
    rock/     9 live of 1024
    ...
    > print rock/3/Velocity
    Velocity{v=(30, -12), spin=0.4}
    > patch rock/3/Velocity/spin 2
    > cd /materials/rocks
    > patch rock.material.json tint 1 0.5 0.5
    > patch /clock/rate 0.25

Mounts today: `/kinds` (a kind, its live rows, a row's components, a component's fields;
fields patch through the schema), `/systems` and `/handlers` (the schedule with sets and
levels), `/resources`, `/events`, `/triggers`, `/clock` (patch `paused`, `rate`, `step`),
`/modules`, `/mem`, `/perf` (the profiler's zones when it is on), and from the modules
that own the formats `/materials` (patch `texture`, `blend`, `tint`, `params`),
`/textures`, `/fonts`, `/sounds`, `/clips`, `/voices`, `/buses` (patch a gain).

`ls` lists 200 entries and says how many more; `ls -n N` for more. A `print` on a
directory lists it; an `ls` on a leaf prints it. Writes are direct, as material patches
always were: a `patch` lands before the next tick and is not part of the input log.
`list` walks one directory at a time and may stop early, so a kind with a million slots
costs a `ls` only what it shows.

## Transports

- **stdin**, when it is not a terminal (a pipe, a file, `/dev/null`) or with
  `--cli-stdin`. Polled with a zero timeout; end of stream ends the session.

Requests and answers are UTF-8. A transport that must stay ASCII (a UART, a devkit TTY;
`--cli-ascii` makes every transport so for now) escapes each non-ASCII code point of an
answer as `\u{XXXX}` and a backslash as `\\`, and unescapes requests the same way, so a
string field can be printed and patched from a terminal that only speaks ASCII.
`char[N]` fields print as quoted strings and patch from the rest of the line, truncated
at a character boundary.
- **TCP** on loopback with `--cli-port N`. Non-blocking listener and sockets from the
  platform module; up to 16 sessions, each with its own line buffer. `nc 127.0.0.1 N`
  is a client.

Everything runs in `cli.pump`, phase Input, on the main thread: accept, read, run,
flush. Reads see the last tick's state; a patch reaches the renderer before the next
frame. A session whose output cannot drain gets lines dropped and a `!dropped` marker
once it can again; the sim never waits for a reader.

## The protocol

One request per line. Responses are lines.

    ls /materials/rocks
    rock.material.json  ready, alpha, row 2, texture 'rocks/rock.png'
    ok

    #7 patch /materials/rocks/rock.material.json tint 1 0.5 0.5
    #7 { "texture": "rocks/rock.png", "blend": "alpha", "tint": [1, 0.5, 0.5, 1], "params": [0, 0, 0, 0, 0, 0, 0, 0] }
    #7 state ready, row 2, texture slot 3
    #7 ok

    bogus
    err: unknown command 'bogus'; try help

A request written as `#id ...` gets every response line prefixed with `#id ` and ends with
`#id ok` or `#id err <message>`, so a tool can send several requests and sort the answers.
Without an id a request ends with `ok` or `err: <message>`. Double quotes group words.
Lines are at most 1023 bytes; a longer one is answered with an error and discarded.

`cli_run(engine, line, sink, user)` runs a request from code, for tools and tests.
