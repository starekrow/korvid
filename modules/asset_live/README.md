# asset_live

A dev build's connection to a running `goldberg watch`: save a file, and the program running
from the watch's output tree reloads it. Optional: a program that does not link it does not
reload live, and a ship build that links it does nothing.

## How it works

At startup it finds the output tree the program reads (`<exe>.assets`, as the platform's mount
does) and, while it runs, reads `<tree>/.goldberg-port`, which a watch writes. It connects to
that port on 127.0.0.1, says `hello NAME CONFIG` (the tree's name, `dev` or `debug`), and
reads lines:

- `changed PATH`: an output landed again in the tree (whole: goldberg renames it into place).
  The module calls `asset_reload(e, PATH)`, which reads it again and rebinds what was loaded
  from it, if anything was (`korvid/asset/reload.h`); Krome rebuilds the screens it touches.
- `removed PATH`: an output left the tree. Logged; what was loaded from it stays.

No watch, or one that went away: it tries again every two seconds, and reads the port file
again each time, so a watch started later (or restarted on another port) is found.

**Heard at once.** The socket is read in the Input phase of every tick, and also from an
engine waker that an idle frame loop asks each pass. Where the platform watches sockets
(`platform_watches`) it is a watched one (`engine_add_watched_waker`): the
connection is given to `platform_wake_set`, so a line from goldberg ends the loop's wait,
and while there is no connection it asks to be woken for the next try
(`engine_want_wake_at`). Elsewhere the loop polls for it every 4 ms. A save reaches an idle
tool's window at once, not at its next quarter-second heartbeat. Linux, dev build,
`kromescreens` under `goldberg watch` (`tools/goldberg/tests/live_numbers.py`, 10 saves
each), from the write to the screen built again: a screen's `.kim` 6.1 ms median, the
prelude `krome/std.kim` 10.1, an icon's `.svg` to its shape read again 7.3; and a 512 x 512
texture in arcfire 49.9, most of it the script's PNG encode (`docs/design/krome-notes.md`,
slice 6). Slice 6, polling, measured 9.2, 8.9, 7.2 and 49.9; slice 12's wait the same
within a few milliseconds, with the window waking four times a second, not 250.

## Using it

Put `"asset_live"` in a program's `deps` (kromedemo, kromescreens, arcfire and goldberg have it), build, start
`goldberg watch` on the program's roots (`--roots build/<target>/goldberg/<program>.roots`), and
run the program.

| | |
|---|---|
| `KV_GOLDBERG=off` | not this run (frameloop's `--no-goldberg`) |
| `KV_GOLDBERG=PORT` | this port, not the one the tree names (frameloop's `--goldberg PORT`) |
| `asset_live_stats(e)` | `connected`, `port`, `connects`, `changed`, `reloaded`, `removed` |

| name | kind | notes |
|---|---|---|
| `asset_live.pump` | system, phase Input, main thread, always | the socket read; connects and reconnects |

## Not in this module

The up channel (what the program loaded, what it asked for and did not find), KBOR on the wire,
a goldberg on another host, and anything for a ship build.
