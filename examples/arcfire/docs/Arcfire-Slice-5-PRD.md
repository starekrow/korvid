# Arcfire Slice 5 — Presentation

Nov 10, 2026 · @starekrow

## Goal and exit criteria

Slice 4 made Shoreline a game to play; this slice makes the program around it one. Today
the game boots straight into a run, the pause screen is a line of text, the run's end is a
banner and a log line, and nothing is remembered between runs. Slice 5 builds the front
door and the screens a game has (title, pause, options, game over, results, name entry,
high scores), saves what a player would expect saved, finishes the effects the master PRD
lists (explosions at three sizes, glows, a vignette, the particles question), and answers
the record-and-replay question in the engine.

The engine's part is two named pieces, both general: a **fresh sim** (every sim kind, the
sim clock, the timers and the queued events back to how they stood at startup, so a second
run in one process is the same run as the first in a new one), and **input record and
replay** in the frame loop, keyed to sim ticks, so a tester's run becomes a hashing test.

It is done when:

- Run plainly, the game opens on a title: Arcade, Stage select, High scores, Options,
  Quit. Left alone on the title it plays the demo as its attract mode; any button returns
  to the title. `--demo`, `--autoplay`, `--stress`, `--bench`, `--stage` and `--arcade`
  go straight into their runs as now.
- A run starts from a fresh sim under a cover; the pause menu (Resume, Options, Quit to
  title) and the run's end return to the title; two runs in one process hash as two
  processes do.
- Out of eggs shows a game-over screen; the results screen shows the run (score, stage and
  round reached, difficulty, restarts, eggs lost, bombs used, longest chain); a score that
  makes the table goes to name entry; the high-score tables persist through the save
  module, as the options do.
- Effects: explosions at three sizes, additive glows under the bullets (a dial), a
  vignette, a stage title card; the particles at render rate measured, and either left as
  they are or moved, with the reason in the notebook.
- `--record FILE` writes a run's input; `--replay FILE` plays it back, checks the state
  hash as it goes, and says where it diverged if it does. A recorded run of Shoreline
  replays with matching hashes across thread counts, I/O backends and asset sources.
- The attract mode's demo hashes tick for tick as `--demo` does.
- The README, the credits and the notebook are current; validation is clean; the leak
  report is empty.

## Fortress is cut (decided)

The master PRD's stage two, Fortress (a canyon and a base, bunkers and silos, walls that
squeeze the field, a gunship pair and a gate), is cut: it is a fair amount of content that
asks nothing new of the engine. The game is two stages, Shoreline and Orbit; arcade plays
them in order, then goes round again a difficulty up as now. Slice 6 is Orbit and the
battle. The gunship (an air enemy with turrets as child parts) may still appear in Orbit;
that is slice 6's call. The master PRD is revised to match.

## Where slice 4 left it

- **Flow.** Run plainly, the game is arcade from Shoreline at Normal; `--stage NAME`,
  `--difficulty`, `--demo`, `--autoplay`, `--stress`, `--bench NAME` are frame-loop flags.
  The run starts at sim tick 0 when the boot system has its assets. Out of eggs puts up a
  cover ("OUT OF EGGS / The stage begins again") for half a second and restarts the stage.
  A run's end shows the stage-clear banner (bonus, the next round or THE END), logs the
  results line and quits 240 ticks later.
- **Pause.** Escape or Start toggles a screen with one line of help ("Escape or Start
  resumes, Q quits") and the `HoldUser` hold; Q quits from anywhere.
- **HUD** (`assets/ui/arcfire.ui.json`, `hud.cc`): side panels; score, chain and its
  window, eggs, bombs, weapon and power, options, restarts, grazes on the left; stage,
  difficulty, distance and the dev counters on the right; the boss's bar; the stomp's
  WARNING.
- **Effects** (`fx.cc`): sparks (the cosmetic `particle` kind, a count per event from the
  `particles` dial), one explosion flipbook (Kenney's burst frames) drawn at the size each
  caller asks, the white hit flash (`Health.flash`), the camera's shake, slow time at the
  egg and at a boss's death (`engine_time_scale`), sparks on the shell.
- **Audio.** Stage and boss music, the effects table with priorities; the audio module's
  bus volumes (`audio_bus_volume`) exist and the game never sets them.
- **Persistence.** None.
- **Determinism.** Patterns, chips and the pilots seed from `(handle, tick)`; the world
  row holds the run. A second run in the same process would begin at a later sim tick with
  every table's slots and generations as the first run left them: a different run from the
  same input. Nothing does a second run yet; a title will.

## The flow

```
            ┌──────────── attract (the demo, idle 20 s) ───────┐
            ▼                                                  │ any button
  boot ──► TITLE ──► Arcade ─────────────┐                     │
            │   ├──► Stage select ──► stage, difficulty ──► RUN ──► pause ──► Quit to title
            │   ├──► High scores                    │
            │   ├──► Options                        ├── out of eggs ──► GAME OVER ──► continue: the stage again
            │   └──► Quit                           │                             └─► end ──► RESULTS
            └◄──────────────────────────────────────┴── the run's end ──► RESULTS ──► NAME ENTRY (a high score)
                                                                                     └─► HIGH SCORES ──► TITLE
```

Every switch between the title and a run happens under a cover (the UI module's fade), in
its `UiCovered` handler, where nobody sees it: the old run's rows go with the fresh sim,
the new run's world row, ship and planes are spawned, and the cover lifts. The title and
its screens hold the sim (`HoldUi`), so no sim tick passes on them: a run started from the
title begins at sim tick 0 however long the player spent choosing.

- **Title.** The game's name over Shoreline's sky (the planes scroll on held ticks; they
  are cosmetic), a list: Arcade, Stage select, High scores, Options, Quit. The difficulty
  for Arcade is the options' "start at" (Normal until changed).
- **Stage select.** Two lists side by side: the stage (Shoreline; Orbit once it exists)
  and the difficulty. Confirm starts the run.
- **Pause** (Escape, Start): Resume, Options, Quit to title. Quit to title asks once.
- **Options**: master, music and effects volume (meters, 0 to 10, set by left and right),
  the arcade's starting difficulty, and the dev counters on the HUD (shown in a dev build
  by default, as now). Saved when the screen closes.
- **Game over** (out of eggs): "GAME OVER", the score, and a ten-second count: Continue
  (the stage again with three eggs, the score back to the stage's start, a restart
  counted, as now) or End (the run ends there: results). The count running out ends the
  run, as an arcade's game over does (decided). The bot and the demo continue at once.
  With every run ending at Hard's last stage or here, a run is finishable but not owed.
- **Results**: the run's numbers as the results line has them, revealed one at a time
  with a tick each, then the stage bonus, then the total.
- **Name entry**: a score that makes its table asks for a name (the UI's `field`, up to
  eight characters, the last one used offered). Then the table, the new row lit.
- **High scores**: three tables, one per arcade starting difficulty (decided: a run from
  Easy plays three rounds and would outscore any from Hard); stage select does not enter.
  Ten rows a table (name, score, the stage and round reached, the restarts); left and right
  turn between them.
- **Stage title card**: as a stage begins, its number and name ("STAGE 1 / SHORELINE")
  slide in and out over the first two seconds; at a restart, again.

The demo (`--demo`) plays as now and ends through the results screen and name entry,
typing "DEMO" (`input_text_override`) into a table of its own, so the scripted run
exercises the screens, then quits. Its hashes are unchanged: the screens hold the sim, so
no sim tick passes on them and the last hashed tick is still the end mark's.

## Saving

Through the save module (JSON), to the platform's preference path
(`platform_pref_path(e, "korvid", "arcfire")`), as arcanum and rocks do:

- `options.json`: the volumes, the starting difficulty, the dev counters.
- `scores.json`: the three tables, each ten rows of {name, score, stage, round,
  restarts, date}. The demo's table is `scores-demo.json`, so a demo run never touches a
  player's.

A file that fails to read is logged and replaced by defaults; nothing the save holds is sim
state, so none of it reaches the hash (the starting difficulty does, as the dials do,
through the run it starts).

## Effects

- **Explosions at three sizes.** Small (popcorn: Kenney's burst at 40 units, short),
  medium (a ground row, a part: the burst at 64 with a second, delayed, offset), large (a
  boss: a sequence of mediums over its hull, then one at 160 with a ring of sparks and a
  flash of the screen). A table in `fx.cc`, each with its shake and its sound.
- **Glows.** An additive soft disc under each enemy bullet, its color by the bullet's
  look, so the bullets read against the islands and the clouds. A second sprite per bullet
  is a second row in the extract and the renderer: a `glows` dial (on at the
  difficulties, off under `--stress` until measured), and the notebook records what it
  costs at rank 24.
- **Vignette.** A full-field darkening at the edges, one screen-space sprite under the
  HUD (made here: a generated radial ramp).
- **The hit flash and the shake**, reviewed against the three explosion sizes; the boss's
  slow time stays as it is.
- **Particles at render rate.** Sparks are cosmetic rows stepped at 60 Hz and drawn
  interpolated. On a display faster than 60 Hz the question is whether a ten-tick spark's
  fade (its alpha, stepped per tick, not interpolated) or its life reads as stepped. The
  milestone records it at 144 Hz (screenshots of a burst at each render frame, and how the
  interpolation treats alpha), then decides: nothing, the renderer interpolating tint as
  it does position, or the engine's render-rate cosmetic class. Only the last is engine
  work of size, and only a visible fault buys it.

## Record and replay

**What is recorded.** Per sim tick since the last fresh sim (or the process's start), the
values of the actions the game names as its gameplay (`FrameLoopConfig::recorded_actions`:
move, shot, focus, bomb here; pause and quit stay live), and any typed text the game takes
on that tick. Paused and held ticks record nothing and replay nothing: they are not sim
time, so menus, a pause and a disk of any speed leave the file as it is.

**The file** (`.kvr`): a header (a magic, a version, the executable's name, the game's
flags and options as given, so a replay runs with the same dials; the action names), then
changes only: a tick delta, an action, its new value. A state hash every 60 ticks (the
`--hash` hash) goes in beside them.

**Replay.** `--replay FILE` takes the header's flags, overrides the named actions from the
file before each sim tick's sample (as the demo does), leaves the rest live (so a viewer
can pause and quit), compares the hash every 60 ticks, logs `replay: diverged at tick N`
at the first mismatch, and logs `replay: ended at tick N, matched` at the end of the
file. A replay of a file whose header names another executable or version is refused.

**Where it lives.** The frame loop owns the flags, the file and the hash comparison;
`platform_hid` gains what the frame loop needs from the action map (read every named
action's value after the sample; set them all before it), so a game without the frame
loop can record too. The game's part is naming its actions and starting the recording at
its fresh sim.

**What it is for here.** A tester plays Shoreline with `--record run.kvr`; `--replay
run.kvr --hash` is then a hashing test of a real run, played the way a person plays, which
the scripted demo is not. Two such files of the slice's own (one at Normal, one at Hard,
played by the bot under `--record`, since the bot's input is input like any other) go in
the tests' data and the hash matrix.

## The fresh sim

`engine_sim_reset(e)` (core), called on a held tick:

- every non-cosmetic kind emptied, its slots and generations as at startup
  (`Table::restore_slots` with the startup's words), so the next spawn gets the handle a
  fresh process would;
- `tick_count` to 0 and the timer wheel, the scheduled events and the undelivered events
  dropped; triggers' previous values cleared;
- a `SimReset` event to the modules' handlers on the same tick, for the module state that
  follows the sim (the collision module's pairs, the audio's sim-rate voices, a game's
  bookkeeping);
- cosmetic kinds left alone (the game clears what it wants: here, the particles), the
  asset scopes untouched.

The game's part: a `new_run` that clears its own state (the stage cursor's copies, the
demo's flags, the pilots' last move, the counters), spawns the world row and the rest as
`boot` does now, and starts the recording. `boot` becomes the first `new_run`.

The test: a core test spawns, steps and resets and checks the handles and tick match a
fresh engine's; in the game, the attract mode's demo and the second of two arcade runs
under the bot hash tick for tick as `--demo` and a fresh `--autoplay` run do.

## The engine's part

- `core`: `engine_sim_reset` and `SimReset`, with a test.
- `frameloop` and `platform_hid`: `--record`, `--replay`, `recorded_actions`, the action
  map's read-all and set-all, the `.kvr` file, with a test that records a short scripted
  run and replays it.
- Possibly the renderer's tint interpolation, if the particles question finds it wanted.

The UI module's `list`, `field`, `meter`, covers and motions cover every screen above;
anything it lacks that a screen needs is a finding, not a workaround.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `engine_sim_reset` and `SimReset`, tested | P0 |
| R2 | The title and its list; the flags bypass it | P0 |
| R3 | Runs started and left under a cover from a fresh sim; quit to title | P0 |
| R4 | Stage select (stage and difficulty) | P0 |
| R5 | Pause menu: resume, options, quit to title | P0 |
| R6 | Options: volumes, starting difficulty, dev counters; saved | P0 |
| R7 | Game over with continue and end | P0 |
| R8 | Results screen | P0 |
| R9 | Name entry and the high-score tables, saved; the demo's apart | P0 |
| R10 | Attract mode: the demo from the title, hashing as `--demo` | P1 |
| R11 | Stage title card | P1 |
| R12 | Explosions at three sizes | P0 |
| R13 | Glows under bullets, a dial, measured at stress | P1 |
| R14 | Vignette | P2 |
| R15 | Particles at render rate: measured and decided | P0 |
| R16 | `--record`, `--replay`, the `.kvr` file, divergence reported | P0 |
| R17 | Two recorded runs in the tests and the hash matrix | P1 |
| R18 | The notebook's slice 5 entries; README, credits; the master PRD without Fortress | P0 |

## Milestones

Each leaves the game building, running and hashing.

1. **The fresh sim and the flow** (R1, R2, R3, R4, R5): `engine_sim_reset`; `new_run`;
   the title, stage select and the pause menu; runs in and out under covers; two runs in a
   process hashing as two processes.
2. **The screens and saving** (R6, R7, R8, R9, R11): options, game over, results, name
   entry, high scores, the title card; `options.json` and `scores.json`; the demo through
   its results and its own table.
3. **Record and replay** (R10, R16, R17): the engine piece, its test, the attract mode,
   the two recorded runs in the matrix.
4. **Effects** (R12, R13, R14, R15, R18): the explosions, glows and vignette, the
   particles question answered, the stress numbers, the notebook, README and credits.

## Decided

- **Fortress is cut**; the game is Shoreline and Orbit.
- **Screens are presentation.** Nothing a screen does is sim state; what reaches the sim
  (a run's mode, stage and difficulty) does so through the run it starts, so it is hashed
  as the dials are.
- **Out of eggs**: the game-over screen, Continue or End; the count running out ends the
  run.
- **High scores**: one table per arcade starting difficulty; stage select does not enter.
- **Attract mode**: the scripted demo, after 20 seconds idle on the title.
