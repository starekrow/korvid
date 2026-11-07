# CLI

Nov 7, 2026. The command line as a module: what a command is, how one reaches the engine,
and the line protocol.

## Commands are registration data

A module declares what it can be told in its `ModuleDesc::commands`: a name of one or
more words, a help line, and a function taking a `CliContext` and the arguments. The
`cli` module collects every table at startup in module order. A request is matched to the
command whose name has the most words in common with the request's first words
(`patch material` beats `patch`), and the rest are the arguments. Whoever reads a format
owns its commands: `ls materials`, `dump material` and `patch material` live in the asset
module; `help` and `quit` in `cli`.

A command answers through `ctx.print`, one line at a time, and `ctx.fail` marks the
request failed with a message; the first failure stands. Commands never block and never
allocate outside the scratch arena.

## Transports

- **stdin**, when it is not a terminal (a pipe, a file, `/dev/null`) or with
  `--cli-stdin`. Polled with a zero timeout; end of stream ends the session.
- **TCP** on loopback with `--cli-port N`. Non-blocking listener and sockets from the
  platform module; up to 16 sessions, each with its own line buffer. `nc 127.0.0.1 N`
  is a client.

Everything runs in `cli.pump`, phase Input, on the main thread: accept, read, run,
flush. Reads see the last tick's state; a patch reaches the renderer before the next
frame. A session whose output cannot drain gets lines dropped and a `!dropped` marker
once it can again; the sim never waits for a reader.

## The protocol

One request per line. Responses are lines.

    ls materials
    example_rocks/rock.material.json   ready   alpha   row 2   texture 'example_rocks/rock.png'
    1 materials
    ok

    #7 patch material example_rocks/rock.material.json tint 1 0.5 0.5
    #7 { "texture": "example_rocks/rock.png", "blend": "alpha", "tint": [1, 0.5, 0.5, 1], "params": [0, 0, 0, 0, 0, 0, 0, 0] }
    #7 ok

    bogus
    err: unknown command 'bogus'; try help

A request written as `#id ...` gets every response line prefixed with `#id ` and ends with
`#id ok` or `#id err <message>`, so a tool can send several requests and sort the answers.
Without an id a request ends with `ok` or `err: <message>`. Double quotes group words.
Lines are at most 1023 bytes; a longer one is answered with an error and discarded.

`cli_run(engine, line, sink, user)` runs a request from code, for tools and tests.
