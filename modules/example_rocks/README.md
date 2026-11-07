# example_rocks

An asteroids-like, written the way the engine would like a game written: four kinds, five
systems with declared sets, eight handlers, scheduled events for bullet expiry and waves,
a trigger for the ship's death, a published score, and no handle kept anywhere except on
the events that carry them. The ship flies from the keyboard or the first gamepad, or
from a script with `--demo` so a run is reproducible: two runs of
`example_rocks --demo --run-seconds 30 --hash` print identical hashes, at `--threads 1`
and `--threads 8`.

Run it: `build/linux-debug/bin/example_rocks [--demo] [--run-seconds 30] [--hash] [--threads N]`.
Arrows turn and thrust, space fires, Escape quits; on a pad the left stick turns, South
thrusts, West fires.

## How the game is built

**Kinds.** `ship` (1: `Transform2`, `Sprite`, `Velocity`, `Health`, `Collider2`, `Ship`),
`bullet` (4096), `rock` (1024, three sizes by `RockSize`), `debris` (8192, with a
`Lifetime` that fades it). Every kind has `Transform2` and `Sprite`, so the sprite module
draws them; the first three have `Collider2`, so the collision module tests them, on
layers `ship`, `bullet` and `rock` with masks that give ship-rock and bullet-rock pairs and
nothing else.

**Systems, all in Sim.** `control` reads `input.state` and writes the ship and the bullet
kind (it fires by direct spawn, holding the write, and schedules each bullet's `Expire`
72 ticks out); it says `before = move`. `move` integrates every kind with `Velocity` in a
`parallel_for`; `wrap` keeps everything on screen, `after = move`, since both write
`*.Transform2`; `fade` ages debris and dims its sprite; `game` watches the score and the
exit request. `move`, `wrap` and `fade` declare `StateUse::None`, so they share levels
instead of serializing on the module's state; the schedule log shows `fade` beside `move`.

**The cascade.** `OverlapBegin` from the collision module arrives at a boundary;
`on_overlap` sorts the pair by kind into `RockHit` or `ShipHit`. `on_rock_hit` (writes the
rock kind and the score) destroys the bullet, splits the rock into two smaller ones by
direct spawn, spawns debris by command, and emits `ScoreChanged`, which is a next-tick
event so nobody reacts to it within the tick. That is three waves in one tick, and a
bullet that hit two rocks is destroyed once: the second `RockHit` names a dead bullet and
the bus drops it. `on_ship_hit` decrements `Health`.

**Timers.** Every bullet's `Expire`; every wave's `SpawnWave`, 600 ticks apart, whose
ticket the game cancels at game over so a wave does not land on a dead ship; the
`Restart` 120 ticks after death.

**The trigger.** `example_rocks.death` watches `example_rocks.ship.Health.hp` crossing
below 1; `on_edge` turns the `StateEdge` into `GameOver` handling: cancel the wave,
schedule the restart, log the score. `on_restart` clears the kinds with `destroy_all` and
starts over.

**The score** is a struct the module publishes (`engine_publish`) as
`example_rocks.score`; its own handlers declare the write, `game` declares the read, and
another module could read it the same way.

**Materials** come from four files (`ship`, `rock`, `bullet`, `debris`); bullets and
debris are additive dots.

## Where things are

| file | what |
|---|---|
| `src/main.cc` | the `FrameLoopConfig` (1280x720, the `--demo` flag, a `game` heap of 16/64 MB) |
| `src/game.cc` | components, kinds, events, the trigger, the score, systems, handlers, startup, the tables |
| `assets/ship.png`, `rock.png`, `dot.png`, `*.material.json` | the art (generated) and the looks |

## Behaviour worth knowing

- **`--demo` is the acceptance test.** The scripted ship turns, thrusts and fires on a
  fixed schedule from the tick counter, so the whole run is a function of the seed. The
  slice 2 PRD's checklist runs against it.
- **The game heap.** `main.cc` declares a `game` heap with a soft limit of 16 MB and a
  hard limit of 64 MB and puts the module in it; a hard-limit failure logs the heap,
  module and category and the game keeps running.
- **Debris is sim state here**, so its fading is hashed. A game that wanted it out of
  the hash would mark `Lifetime` cosmetic and keep it out of sim systems' sets.
- **Rendering artifacts seen in slice 2** (sprites of the same layer swapping order)
  were the old CPU sort; the slice 3 renderer draws equal layers in slot order.
