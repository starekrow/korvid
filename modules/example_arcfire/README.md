# example_arcfire

A vertical bullet-hell shooter, and the engine's first stress test that is also a game
(`docs/Arcfire-Master-PRD.md`). This is slice 1, the skeleton (`docs/Arcfire-Slice-1-PRD.md`):
a 3:4 field between two side panels, three parallax planes scrolling under it, a ship that
flies in eight directions at two speeds and fires Spread at its first level, formations of
popcorn and two turrets from a stage script keyed to scroll distance, aimed bullets,
kills, grazes, and the egg: a death in slow motion, after which a small spherical machine
builds the next ship. Every count
is a dial, and the game can fly itself three ways. The art is CC0, from Kenney, Screaming
Brain Studios, Master484 and GGBotNet (`assets/CREDITS.md`).

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory:

```bash
tools/kormake/bin/linux/kormake build linux-dev
build/linux-dev/bin/example_arcfire
```

Controls: arrows or WASD (or the left stick or the d-pad) fly; Z or Space (South) fires,
held; Shift (West) focuses: half speed, the hitbox and the graze ring shown, the spread
narrowed; Escape (Start) pauses; Q quits. X (East) is the bomb, which does nothing yet.

The ways it flies itself:

- `--demo` plays a scripted minute and quits at the stage's end: it fires throughout,
  flies focused through the first formation's stream (the ring grazes), dodges by the bot's
  rule, stands in a bullet from tick 1500 until one finds it (the egg plays once), and the
  end mark quits it. `--demo --hash` prints the state hash every sim tick. The hashes match
  across runs, thread counts (`--threads 1`, `--threads 8`), I/O backends (`--io sync`,
  `--io uring`) and asset sources (loose files, `--pack` with a kpak of `bin/assets`, or
  `--pack` with a zip of it).
- `--autoplay` puts the bot on the stick at Normal: it holds fire, sidesteps the bullets
  whose lines would pass within 30 units, keeps off the air enemies and the walls, and
  otherwise lines up under the nearest enemy.
- `--stress` turns the dials to their stress position: rank 24, three times the
  formations at half the spacing, four times the sparks, the planes in 128-unit tiles,
  twice the enemy health, the bot flying an invulnerable ship, the stage looping.
- `--bench NAME` fires one pattern of the library from a fixed emitter at the top of the
  field, again and again, under an invulnerable bot: how a pattern is written and measured.
- `--dial name=value` moves one dial (repeat it for more): `rank`,
  `bullet_speed`, `formation_count`, `formation_spacing`, `particles`, `plane_density`,
  `enemy_health`, `bot`, `invulnerable`, `loops`. The log prints the dials at the start.

Every mode logs the game's counters beside the frame loop's fps line (bullets, sprites,
the extract's Set deltas this tick, the broadphase's colliders and the pairs it tested)
and a summary with the peaks and means at the exit. Dev builds show the same counters in
the right panel. The measurements are in `docs/performance-notes.md`.

## How it is built

The game is one module whose files are its parts. `src/arcfire.h` is what they share: the
components, the events, the kinds' ids, the stage as its file says it, and the module's
state.

- **The world row.** One row of the kind `.world` holds the dials, the stage's clock
  (`Stage`: distance, speed, the cursor over the marks), the run (`Run`: score, eggs,
  restarts), the egg sequence and the sim's random stream, so the state hash covers all of
  it. The module's own state holds only what is not the sim's: asset handles, the stage
  file as parsed, the UI's screens, the counters.
- **The field** (`src/field.h`) is 540 x 720 world units around the origin. Nothing in the
  sim reads the window: the camera (`hud.cc`) zooms to fit the field's height (or its
  width, in a window narrower than 3:4), and the UI anchors the side panels at the field's
  edges.
- **The stage** (`stage.cc`, `stage_data.cc`) is `assets/stages/shoreline.stage.json`:
  a speed, named paths, and marks by distance (a formation, a ground row, a speed change,
  the end). `scroll` adds the speed to the distance every sim tick; `stage` runs every mark
  the distance has passed. A formation mark schedules its members as events on the ticks
  their spacing gives; each member's event carries the run's epoch, so members scheduled
  before a restart never arrive. The sim holds (`HoldLoading`) until the file is in.
- **Planes** (`planes.cc`): three grids of tiles of a seamless background, a cosmetic
  kind, each moved by its rate times the scroll and wrapped as a teleport. This is the
  game-side parallax the PRD asks to measure: every tile is a Set delta every tick.
- **The ship** (`ship.cc`): `control` reads the action map and moves, clamps and fires;
  `shots` flies the shots in a `parallel_for`. The hitbox is the ship's own collider (radius
  3); the graze ring (radius 18) and the hitbox's dot are child rows.
- **Enemies** (`enemies.cc`): an air row follows a Catmull-Rom path by arc length
  (`path.cc`), so members of a formation keep their spacing, and starts its pattern where
  its emitter says along the path; a ground row rides the scroll and runs its pattern while
  the ship is above it. A hit flashes them by drawing their texture added.
- **Patterns** (`patterns.cc`, `assets/patterns/`): BulletML documents, one pattern each,
  named in `patterns.json` and compiled at load by the `bulletml` module; the stage names
  them. Every enemy carries a `Runner` (the module's plain-data runner) and an `Emitter`
  (which pattern, when it starts). `patterns` steps every runner in a `parallel_for`; a
  fire spawns by the chunk's commands, and each runner's `$rand` is its own stream seeded
  from its entity and the tick, so the result is the same on any thread count. The `rank`
  dial is BulletML's `$rank`.
- **Bullets** (`bullets.cc`): two kinds, `bullet` (plain) and `active` (a bullet whose
  definition has actions: it carries a `Runner` and steers, fires and vanishes by its
  program), flown and aged in a `parallel_for`; they meet the hitbox, the ring and the
  shell, never each other.
- **Hits** (`hits.cc`): `on_overlap` sorts the collision module's pairs by kind into
  `ShotHit`, `BulletHit`, `Rammed`, `Graze` and `ShellHit`. The handlers that change the
  run are chained so a bullet that hits the hitbox takes back its own graze first.
- **The egg** (`egg.cc`): a death slows time to a quarter (`engine_time_scale`, set each
  tick by the sequence), breaks the ship into its four quarters, cancels the bullets near
  it, drops the egg (a spherical machine) that rests, builds the new ship around itself as
  time comes back and falls away in halves, and leaves a shell around it for two seconds that eats bullets. Out of eggs, the stage starts over
  behind a cover: everything it made is gone, the distance is 0, the score is what it was
  when the stage began, the eggs are 3 again and the restart is counted.
- **The HUD** (`hud.cc`) is `assets/ui/arcfire.ui.json`: the side panels, the score, the
  eggs as pips, the restarts, the grazes, the stage and its distance, the dev counters,
  and the pause, cover and stage-clear screens. The HUD system writes the model each tick.
- **Pilots** (`pilot.cc`): the demo's script and the bot share one steering rule. Both
  drive the action map with `input_action_override`.
- **Effects** (`fx.cc`): sparks and debris are particles in a cosmetic kind, drawn from
  the module's own random stream, so the particle dial changes nothing the sim draws.
- **Reports** (`stats.cc`): the counters as the resource `example_arcfire.stats`, the log
  lines, the summary.

## Where things are

| file | what |
|---|---|
| `src/module.cc` | the registration tables, the schedule, startup, the sprite helpers |
| `src/arcfire.h`, `src/field.h` | what the files share; the field's bounds |
| `src/stage.cc`, `src/stage_data.cc` | the boot, the scroll, the marks; reading a stage file |
| `src/path.cc` | Catmull-Rom paths by arc length |
| `src/dials.cc` | the dials' positions and `--dial` |
| `src/planes.cc`, `src/ship.cc`, `src/enemies.cc`, `src/bullets.cc` | what moves |
| `src/patterns.cc`, `assets/patterns/` | the library and the runners; the ten patterns |
| `src/hits.cc`, `src/egg.cc` | what happens when things meet; the death and the restart |
| `src/hud.cc`, `src/fx.cc`, `src/pilot.cc`, `src/stats.cc` | the camera and the screens; particles; the demo and the bot; the reports |
| `src/main.cc` | the frame loop's config and the game's flags |
| `assets/stages/shoreline.stage.json` | the stage |
| `assets/ui/arcfire.ui.json` | the HUD and every screen |
| `assets/CREDITS.md`, `assets/LICENSE-*.txt` | who made what |
| `tools/make_art.py` | copies the vendored art in and draws `assets/made/` |
| `tests/` | paths; the stage file and the parser's rules |
| `docs/` | the PRDs and the performance notebook |

What the game asks of the engine, found as it is built, is in
`docs/design/arcfire-improvements.md` at the repository's root.
