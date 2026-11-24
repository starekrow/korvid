# rocks

An asteroids-like, written the way the engine would like a game written: seven ktables, seven
systems with declared sets, twelve handlers, scheduled events for bullet expiry and waves,
a trigger for the ship's death, a published score, and no handle kept anywhere except on
the events that carry them. The ship flies from the keyboard or the first gamepad, or
from a script with `--demo` so a run is reproducible: two runs of
`rocks --demo --run-seconds 30 --hash` print identical hashes, at `--threads 1`
and `--threads 8`, and the demo pauses itself for 90 ticks along the way without
changing a hash. Escape or Start pauses, Q quits, a hit shakes the camera.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory,
where the shaders and this module's `assets/` have been copied next to it:

```bash
tools/kormake/bin/linux/kormake rocks debug    # or, in this directory: kormake debug
build/linux-debug/bin/rocks --demo
```

`kormake rocks` builds this example and what it links, in linux-debug unless a config follows:
debug is everything checked with the validation layers on, `dev` the build to measure
with, `ship` neither. `kormake test rocks` runs its tests; at the root, `kormake` builds
every module and example and `kormake test` runs every suite.

## How the game is built

**KTables.** `ship` (1: `Transform2`, `Sprite`, `Velocity`, `Health`, `Collider2`, `Ship`),
`bullet` (4096), `rock` (1024, three sizes by `RockSize`), `debris` (8192, with an
`AnimPlayer` that fades it), `flame` (a child of the ship, cosmetic), `overlay` (the
pause dimmer) and `label` (the HUD and the banners). Every ktable but `label` has
`Transform2` and `Sprite`, so the sprite module draws them; the first three have
`Collider2`, so the collision module tests them, on layers `ship`, `bullet` and `rock`
with masks that give ship-rock and bullet-rock pairs and nothing else.

**Systems.** In Sim: `control` reads `input.state` and writes the ship, the flame and the
bullet ktable (it fires by direct spawn, holding the write, and schedules each bullet's
`Expire` 72 ticks out); it says `before = move`. `move` integrates every ktable with
`Velocity` in a `parallel_for`; `wrap` keeps everything on the field, `after = move`, since
both write `*.Transform2`; `game` starts the first game. `move` and `wrap` declare
`StateUse::None`, so they share levels instead of serializing on the module's state.

The field is a fixed 1280 x 720 world units around the origin: waves arrive at its edges,
and `wrap` wraps at them. The HUD is screen-space text anchored to the window's top-right
corner (`ScreenAnchor::TopRight`), so the renderer puts it there on any window. Only
`camera` reads the window, to set the zoom that fits the field to it (a window of another
shape shows a margin on two sides), so nothing in the simulation depends on the window's
size or the display's scale, and `--demo --hash` gives the same run on any screen.

Around it: `pause` (PreSim, `Always`) toggles the pause, `camera` (PostSim) shakes the
camera, `hud` (PostSim, `Always`) rewrites the HUD label.

**The cascade.** `OverlapBegin` from the collision module arrives at a boundary;
`on_overlap` sorts the pair by ktable into `RockHit` or `ShipHit`. `on_rock_hit` (writes the
rock ktable and the score) destroys the bullet, splits the rock into two smaller ones by
direct spawn, spawns debris by command, and emits `ScoreChanged`, which is a next-tick
event so nobody reacts to it within the tick. That is three waves in one tick, and a
bullet that hit two rocks is destroyed once: the second `RockHit` names a dead bullet and
the bus drops it. `on_ship_hit` decrements `Health`.

**Timers.** Every bullet's `Expire`; every wave's `SpawnWave`, 600 ticks apart, whose
ticket the game cancels at game over so a wave does not land on a dead ship; the
`Restart` 120 ticks after death.

**The trigger.** `rocks.death` watches `rocks.ship.Health.hp` crossing
below 1; `on_edge` turns the `StateEdge` into `GameOver` handling: cancel the wave,
schedule the restart, log the score. `on_restart` clears the ktables with `destroy_all` and
starts over.

**The score** is a struct the module publishes (`engine_publish`) as
`rocks.score`; its own handlers declare the write, `game` declares the read, and
another module could read it the same way.

**Materials** come from four files (`ship`, `rock`, `bullet`, `debris`); bullets and
debris are additive dots.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the `FrameLoopConfig` (1280x720, the `--demo` flag, a `game` heap of 16/64 MB) |
| `src/game.cc` | components, ktables, events, the trigger, the score, systems, handlers, startup, the tables |
| `assets/ship.png`, `rock.png`, `dot.png`, `white.png`, `flame.png`, `*.material.json` | the art (generated) and the looks |
| `assets/flame.anim.json` | the flame's clip: a two-frame flipbook and a scale wobble |
| `assets/*.wav` | the sounds (generated: a saw sweep, two noise bursts, a loop with its ends cross-faded) |

## Behaviour worth knowing

- **`--demo` is the acceptance test.** The scripted ship turns, thrusts and fires on a
  fixed schedule from the tick counter, so the whole run is a function of the seed. The
  slice 2 PRD's checklist runs against it. The demo also pauses at tick 400 for 90
  paused ticks; the hash lines are per sim tick, so a paused run and an unpaused one
  print the same sequence.
- **Pause is a system, not a flag.** `rocks.pause` runs on every tick
  (`RunWhen::Always`, phase PreSim): it watches `KeyDown` and `PadButtonDown`, spawns a
  screen-space dimmer in the `overlay` ktable and calls `engine_pause`. Nothing else in the
  game knows about pausing: the sim systems simply do not run, the timers hold, and the
  extract keeps drawing.
- **Text has a ktable of its own.** `label` rows carry a `Text` and a `Label` role: the HUD
  (rewritten every tick by `hud`, an `Always` system, from the published score; the text
  module relays out only when the string changes), the pause banner (spawned and
  destroyed by `pause`) and the game-over banner (spawned by `on_edge`, cleared by
  `on_restart` with everything else). The font is DejaVu Sans at 28 px, from the dejavu
  module.
- **Exit is a handler.** `on_exit` quits on `ExitRequested` whether or not the game is
  paused; a Sim system would not run while paused and a `--run-seconds` that lands
  inside the demo's pause would be lost.
- **The high score is a file.** `rocks_startup` asks the platform for the per-user
  directory (`platform_user_dir(engine, UserDir::Data, "korvid", "rocks")`) and queues a read of
  `highscore.txt` there (`highscore-demo.txt` under `--demo`, so a scripted run never
  touches a player's record); `files` (PreSim, main thread, `Always`) reaps it when it
  arrives, and `on_edge` queues a write when a game ends above it. The `label` ktable is
  cosmetic, so the HIGH figure on the HUD cannot move the hash.
- **Sounds.** Four generated WAVs at 22 kHz (`shot`, `break`, `boom`, `thrust`), loaded
  at startup. `control` plays a shot per bullet with a random pitch and a pan from the
  ship's x; `on_rock_hit` plays a break pitched up by the rock's size; `on_edge` plays the
  boom and stops the thruster; `on_spawned` starts the thruster as a silent loop that
  `control` turns up while thrusting; `pause` plays a click on the Ui bus that does not
  pause with the sim, so the resume is heard too. Every caller declares `appends` on
  `audio.commands`: the calls are requests applied at the boundary in unit order, and need
  no order between callers.
- **The flame is a child with a flipbook.** The `flame` ktable has `Local2`, `Parent` and
  an `AnimPlayer`; `on_spawned` gives each new ship one, 30 units behind it, playing
  `flame.anim.json` (two frames of a sheet at 12 fps and a scale wobble on its `Local2`),
  and the scene module keeps it there and destroys it with the ship. `control` only sets
  its alpha from the thrust. The ktable is cosmetic: the clip comes from a file and starts
  when the file arrives, which is not a tick the hash may depend on.
- **Debris fades by clip.** There is no `Lifetime` and no fade system any more: each
  piece plays `rocks.fade`, a one-second alpha ramp built at startup with
  `anim_create`, at `1 / lifetime`, and `on_anim_finished` destroys it when
  `AnimFinished` arrives.
- **The camera shake** is `rocks.camera` in PostSim writing `sprite.camera`; the
  offset is a function of the tick, so a run is still reproducible, and the camera is not
  hashed in any case.
- **The game heap.** `main.cc` declares a `game` heap with a soft limit of 16 MB and a
  hard limit of 64 MB and puts the module in it; a hard-limit failure logs the heap,
  module and category and the game keeps running.
- **Debris is sim state here**, so its fading is hashed: the `AnimPlayer` time advances
  on the tick and the clip is built at startup, so two runs agree. The flame, whose clip
  arrives from a file, is a cosmetic ktable for exactly that reason.
- **Rendering artifacts seen in slice 2** (sprites of the same layer swapping order)
  were the old CPU sort; the slice 3 renderer draws equal layers in slot order.
