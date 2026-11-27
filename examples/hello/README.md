# hello

The game [An introduction to Korvid](../../docs/guides/introduction.md) builds, finished:
one sprite in a window. The arrow keys (or WASD, a stick, the d-pad) steer it, and Space
(or the pad's South button) makes it pulse large and orange for 20 ticks. It is the
smallest game that has a ktable, a system, events of its own, a handler and a timer.

## Building and running

```bash
kormake hello
```

```bash
build/linux-debug/bin/hello
```

Escape or the window's close button quits. The CLI is on standard input: `press pulse`
pulses it, `print /ktables/hello/blob/0` shows its row.

## What it shows

**A module, and the modules it brings.** `module.lua` names seven dependencies; the
program starts thirteen engine modules, `frameloop` among them, which opens the window.

**Quitting by event.** `hello.on_exit` handles `ExitRequested` and calls
`engine_request_quit`. Without it, neither Escape nor the close button ends the program.

**A sprite.** `hello.blob` is a ktable of `Transform2` and `Sprite`, so the sprite module
draws it. Startup loads `ball.material.json` and spawns the one row directly.

**Input as actions.** `move` (an Axis2) and `pulse` (a Button), bound as data.

**A system with declared sets.** `hello.steer` (Sim) reads `input.actions` and
`.blob.@alive`, writes `.blob.Transform2`, and emits `Pulsed`.

**Events, a handler and a timer.** `hello.on_pulsed` (Sim, after `steer`) makes the blob
large and orange and schedules `Calm` 20 ticks out; `hello.on_calm` (Input) puts it back.
`hello.on_key` logs every `KeyDown` the window delivers.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the `FrameLoopConfig` (960x540) and one call to `frameloop_main` |
| `src/game.cc` | the ktable, the events, the actions, the system, the handlers, startup |
| `assets/ball.png`, `assets/ball.material.json` | the ball and its material |

What the module registers: ktable `hello.blob` (1 slot); events `Pulsed`, `Calm`; system
`hello.steer` (Sim); handlers `hello.on_pulsed` (Sim), `hello.on_calm` (Input),
`hello.on_key` (Input), `hello.on_exit` (Sim).

## Behaviour worth knowing

- The field the blob stays in is fixed at 960 by 540 world units, the window's size at the
  start. The sim doesn't read the window, whose size a replay would not know, so a larger
  window shows more room the blob can't reach.
- A second Space during a pulse schedules a second `Calm`; the first one to arrive ends
  the pulse.

The ball is testbed's, from **Shiny boulders** by SpiderDave, CC0 1.0
([assets/CREDITS.md](assets/CREDITS.md)).
