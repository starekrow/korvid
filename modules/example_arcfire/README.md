# example_arcfire

A vertical bullet-hell shooter, and the engine's first stress test that is also a game
(`docs/Arcfire-Master-PRD.md`). Built in slices (`docs/Arcfire-Slice-N-PRD.md`); this is
slice 4, Shoreline with the arsenal: a 3:4 field between two side panels; a Tiled map of
sea and islands scrolling under two decks of cloud, each at its own speed; a ship that
flies in eight directions at two speeds with four weapons at five power levels (Spread,
the Beam, Storm, Lance) and up to two options; formations of popcorn, turrets, tanks and
bunkers on a stage script keyed to scroll distance, some of them carrying items; BulletML
patterns; bombs, chips and grazes; the chain, extends and the stage bonus; the egg, a
death in slow motion after which a small spherical machine builds the next ship; two
bosses made of parts, the carrier and the battleship; Easy, Normal and Hard, arcade and
stage select. Every count is a dial, and the game can fly itself three ways. The art is CC0, from Kenney, Screaming
Brain Studios, Master484 and GGBotNet, and the bosses are kitbashed here from Kenney's
pieces; the sound is Kenney's and the music Juhani Junkala's, CC0 too (`assets/CREDITS.md`).

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
narrowed; X (East) bombs; Escape (Start) pauses; Q quits.

Run plainly, the game opens on its title, over Shoreline's sky: Arcade, Stage select
(the stage, then Easy, Normal or Hard), High scores, Options, Quit. Arcade is the stages
in order (Shoreline alone for now), and at the end the stage-clear banner with the stage
bonus and a round again a difficulty up, the score carried, until the round at Hard ends
the run. A run starts behind a fade from a fresh sim, so every run from the title is the
same run a new process would play, and opens with the stage's card (again at each round
and restart). The pause menu (Escape or Start): Resume, Options, Quit to title (asked
twice). Out of eggs, the stage starts over behind it, the score as it was when the stage
began, and a player gets the game-over screen: Continue, or End the run, which the
ten-second count running out does too. A run's end (or End) logs its results line and
shows it: the score, the stage and round, the difficulty, restarts, eggs lost, bombs used,
the longest chain, a line at a time. An arcade score that makes its table (one per
starting difficulty, ten rows) asks for a name; then the table, the new row lit; then the
title.

Options: the master, music and effects volumes, arcade's starting difficulty, the dev
counters. They and the tables are saved through the save module to the per-user directory
(`~/.local/share/korvid/arcfire/` on Linux: `options.json`, `scores.json`); a file that is
missing or will not read is the defaults. The demo keeps `scores-demo.json` and never
touches the options.

A flag that names a run goes straight into it and quits at its end: `--arcade`, `--stage
NAME` (`assets/stages/NAME.stage.json`), and the ways it flies itself below; `--difficulty
easy|normal|hard` is where arcade or the stage starts (Normal). Left alone on the title for 20 seconds (`--attract SECONDS`; 0 never), the game plays the
demo from a fresh sim with PRESS START blinking over it, hashing tick for tick as `--demo`
does; a key or a pad's button, or the demo's end, goes back to the title.

`--record FILE` writes a run's input and `--replay FILE` plays it back, checking the state
hash every 60 ticks (the frame loop's; its README has the file). A run started from the
title records the arguments that start it, so its replay goes straight in. Two recorded bot
runs, Shoreline at Normal and Hard, are in `tests/replays/`; replayed from the repository
root, `build/linux-dev/bin/example_arcfire --replay
modules/example_arcfire/tests/replays/normal.kvr` must end `replay: ended at tick 11318,
matched`. A change to the
sim makes them diverge, and they are recorded again (`--autoplay --stage shoreline
--difficulty normal --record ...`).

`--again` is a check: a run
that ends starts once more from a fresh sim instead of quitting, so `--demo --again --hash`
prints the demo's hashes twice, and the two must match. Over `--cli-stdin`, `press ACTION
[TICKS] [X Y]` holds an action as a player would, `type TEXT` types, and `screenshot FILE`
takes the next frame, so a script can walk the menus.

The ways it flies itself:

- `--demo` plays the whole stage (about three and a half minutes) and quits at its end:
  it fires throughout, flies focused through the first formation's stream (the ring
  grazes), flies by the bot's rule (below) otherwise, bombs in the spiral at tick 1440 and
  climbs past the magnet line for the chips, and from tick 1650 stands still and holds
  fire until a bullet finds it (the egg plays once; nothing hits it after, the demo's
  `invulnerable` being 2). It wants its weapons by the stage's clock: Spread on the
  opening's power items (to level five), the Beam at the islands, Storm over the airfield
  and the carrier (a bomb in its core's phase), Lance and both options for the
  battleship; it takes an egg on the way. The end mark ends it at tick 11829, the last sim tick; it
  then reads its results, signs "DEMO" into its own table (`scores-demo.json`) and quits. `--demo --hash` prints the state hash every sim tick. The hashes match
  across runs, thread counts (`--threads 1`, `--threads 8`), I/O backends (`--io sync`,
  `--io uring`) and asset sources (loose files, `--pack` with a kpak of `bin/assets`, or
  `--pack` with a zip of it).
- `--autoplay` puts the bot on the stick, in arcade or a stage select: it holds fire,
  steps away from each bullet's closest pass in the next 40 ticks (against the way the ship
  is going) when it comes within 26 units, keeping some of its last step so two passes
  alike do not make it dither; keeps off the air enemies, a boss's hull and, while
  dodging, the walls; with nothing to fear goes for items (power, options, eggs, bombs, a
  weapon while it shows a type wanted: Lance or Storm at a boss, Spread or Storm
  otherwise; one showing another type it keeps clear of), else lines up under the nearest
  enemy (at a boss, beside the lowest open part that ends its phase, by the spread at that
  height, focused once there). It bombs to live: a bullet about to meet the hitbox, bullets
  closing from every side, or more than 40 close in (25 at a boss); from a boss's last ten
  seconds it keeps one bomb back for the stomp. The demo and the bot hold every gameplay action every tick, so a key
  touched while they fly changes nothing.
- `--stress` turns the dials to their stress position: rank 24, three times the
  formations at half the spacing, four times the sparks, the planes in 128-unit tiles,
  twice the enemy health, the bot flying an invulnerable ship, the stage looping.
- `--bench NAME` fires one pattern of the library from a fixed emitter at the top of the
  field, again and again, under an invulnerable bot: how a pattern is written and measured.
- `--dial name=value` moves one dial (repeat it for more): `rank`,
  `bullet_speed`, `formation_count`, `formation_spacing`, `particles`, `plane_density`,
  `enemy_health`, `bot`, `invulnerable` (1, or 2: after the first death), `loops`,
  `weapon` and `power` (what the ship starts with: 0 Spread, 1 Beam, 2 Storm, 3 Lance; 1..5),
  `options` (0..2, the options it starts with), `chain_window` (ticks; 0 turns the chain off), `glows` (1, the bullets' halos; 0 under `--stress`),
  `scroll_rows` (1: move every plane tile
  every tick instead of scrolling the layer, for the notebook's comparison). The log prints
  the dials at the start.

Every mode logs the game's counters beside the frame loop's fps line (bullets, sprites,
the extract's Set deltas this tick, the broadphase's colliders and the pairs it tested)
and a summary with the peaks and means at the exit. Dev builds show the same counters in
the right panel. The measurements are in `docs/performance-notes.md`; for timings, add the
frame loop's `--spike-hunt` (a dev build without its memory fills and guard pages, whose
spikes are its own), and to run a long mode faster, `--cli-stdin` with `rate 4` typed in
(the sim at four times wall speed, no tick changed).

## How it is built

The game is one module whose files are its parts. `src/arcfire.h` is what they share: the
components, the events, the kinds' ids, the stage as its file says it, and the module's
state.

- **The world row.** One row of the kind `.world` holds the dials, the stage's clock
  (`Stage`: distance, speed, the cursor over the marks, the round), the run (`Run`: score,
  eggs, bombs, restarts, the chain, the next extend), the egg sequence and the sim's random stream, so the state hash covers all of
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
- **Planes** (`planes.cc`): grids of tiles of a seamless texture in a cosmetic kind, each
  on a layer of its own, which a stage names: space's three (stars, a nebula, a faint
  cloud: the bench's) or the sky's (Shoreline's). The sky is the parallax over the map: the
  low clouds at 1.35 times the ground's speed under the air, their shadows on the sea and
  islands (the same texture darkened, down and to the right, at the same rate), and high
  wisps at 2.1 times over the air and the bosses but under the ship, its shots and the
  bullets. The tiles never move: each plane's layer is scrolled by its rate times the speed
  (`sprite/layers.h`), wrapped by a whole tile as a snap, so a plane is one value a tick.
- **The ground** (`ground.cc`, `assets/maps/`): a stage may name a Tiled map, here
  Shoreline's sea and islands, written by `tools/make_maps.py` from Kenney's terrain tiles
  and drawn at twice their size. Its 15,000 tiles are spawned once into the tilemap
  module's still kind and scrolled by layer with the stage's distance, so they cost the
  extract nothing a tick. The map's `enemies` layer places the ground rows: turrets, tanks
  that drive their road, bunkers that launch popcorn up a stage path. Each comes into view
  where its place on the map crosses the field's top, and rides the scroll from there.
- **The ship** (`ship.cc`): `control` reads the action map and moves, clamps and fires
  its weapon.
- **Weapons** (`weapons.cc`): four types at five power levels, rows of tables: Spread (a
  fan, wider and denser), Beam (a child row of the ship cut each tick by
  `collision_cast_all` at the first body it meets, which it damages every few ticks; the one
  weapon a boss's hull stops), Storm (bolts that turn toward the nearest enemy) and Lance
  (heavy shots that pass through what they kill). The level is the ship's, kept when the
  type changes; weapon and power items (`items.cc`) switch and raise them. Item carriers
  (a formation's, a ground row's or a boss part's `drop`) leave weapon, power, option and
  egg items. Shots and the
  beam hurt by one path (`hurt`, hits.cc). `shots` flies the shots and steers the bolts. The hitbox is the ship's own collider (radius
  3); the graze ring (radius 18) and the hitbox's dot are child rows.
- **Options** (`options.cc`): up to two satellites, children of the ship, that orbit it
  unfocused and hold beside it focused. Each fires the ship's weapon two levels lower (its
  own beam when the weapon is the Beam) and eats the bullets it meets, as the shell does. A
  death takes both.
- **Enemies** (`enemies.cc`): an air row follows a Catmull-Rom path by arc length
  (`path.cc`), so members of a formation keep their spacing, and starts its pattern where
  its emitter says along the path; a ground row rides the scroll and runs its pattern while
  the ship is above it. A hit flashes them by drawing their texture added.
- **Bosses** (`boss.cc`, `boss_data.cc`, `assets/bosses/`): a boss mark in the stage
  spawns one, and the marks after it wait (the stage's clock holds; the scroll and the map
  go on) until it has died or left. A boss is a hull (`boss`, a body the ship must not
  touch; shots fly over it) and parts on it (`part`: turrets, launch bays, a core), children
  that `collision.compose` places before the broadphase, so they are hit where they are
  drawn. Its definition gives the entry path, the timeout, the parts (a cell of the parts
  sheet, a collider, health, patterns taken in turn, the phase each opens in, a bay's
  formation) and the phases, each ended by the deaths of the parts it names. A part is
  armored until its phase (shots spark off it). A part's death is a state trigger
  (`part_down`, health crossing below one) whose handler takes it and what rides on it,
  pays, and moves the boss on a phase. The core's death is the boss's: time slows to a
  quarter for a moment, every bullet becomes a chip, the bonus is paid by the time it had
  left, the parts still standing burst in turn and the hull breaks in pieces. Out of time,
  a boss stomps: every part still standing fires its stomp pattern, the bays launch at
  once, the screen tints and the hull comes down the field and back, a body the ship must
  not touch (meant to cost an egg; a bomb is the way out); then it leaves, paying nothing.
  Shoreline has two: the carrier over the airfield (four turrets, two bays, a core) and the
  battleship in the open water at the end (two main guns, four anti-air mounts, a missile
  bay; then two deck hatches; then the bridge). The art is made by `tools/make_bosses.py`
  from Kenney's pieces into `assets/generated/`. The HUD shows a boss's parts' health as a
  bar.
- **Patterns** (`patterns.cc`, `assets/patterns/`): BulletML documents, one pattern each,
  named in `patterns.json` and compiled at load by the `bulletml` module; the stage and the
  bosses name them. Every enemy carries a `Runner` (the module's plain-data runner) and an
  `Emitter` (which pattern, when it starts). `patterns` steps every enemy's runner in a
  `parallel_for`, after `collision.compose` (a part fires from where it is this tick), and
  `actives` the active bullets' (below), before the bullets fly; a
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
- **The bomb** (`bomb.cc`): with one in hand, every bullet on the field is cancelled into
  chips, every enemy on the field takes the blast (a handler, as a shot's hit), the screen
  flashes and shakes, and the hitbox meets nothing for two seconds. Two an egg; a death
  puts the count back to two; at most six.
- **Items** (`items.cc`): a cancelled bullet becomes a score chip (at most 256 a cancel;
  the rest are paid at once); a formation shot down to its last member drops a bomb where
  it fell (a `formation` row counts its members killed and gone). An item carrier (a
  formation's, a ground row's or a boss part's `drop`) leaves its item inside the field,
  wherever it died. Items pop up and fall (a weapon item drifts, at a third of the speed,
  through several turns of its cycle, so a type can be waited for), and come to the ship
  above the magnet line or within a short reach; the graze ring picks them up. An option
  item past the second pays 20,000 instead, an egg item past the fifth egg 50,000.
- **Scoring** (`scoring.h`, `scoring.cc`): the chain (a kill within `chain_window` ticks of
  the last adds one and pays times x1 below 10 kills, x2 to 24, x3 to 49, x4 to 99, x5 from
  100; a graze holds the window 20 ticks more; a gap, a death or a restart ends it),
  extends (an egg at 200,000 and 600,000, then every million; past five eggs, 50,000), the
  stage bonus (20,000 an egg in hand, 5,000 a bomb). A boss's time bonus is paid outside
  the chain. The `scoring` system, before the sim each tick, runs the window down, pays
  the extends and begins a round the end mark called for, with arcade's next difficulty.
- **Difficulty** (`dials.cc`): Easy, Normal and Hard are positions of the dials: rank 0.25,
  0.5, 0.9; bullet speed 0.85, 1, 1.15; enemy health 0.8, 1, 1.3; formations 1, 1, 1.25 times;
  the chain's window 110, 90, 75 ticks.
- **The egg** (`egg.cc`): a death slows time to a quarter (`engine_time_scale`, set each
  tick by the sequence), breaks the ship into its four quarters, cancels the bullets near
  it into chips, drops the egg (a spherical machine) that rests, builds the new ship
  around itself as time comes back and falls away in halves, and leaves a shell around it
  for two seconds that eats bullets. Out of eggs, the stage starts over behind a cover:
  everything it made is gone, the distance is 0, the score is what it was when the stage
  began, the eggs are 3 again and the restart is counted.
- **The HUD** (`hud.cc`) is `assets/ui/arcfire.ui.json`: the side panels, the score, the
  eggs and the bombs as pips, the restarts, the grazes, the stage and its distance, the dev counters,
  a boss's name and health bar while one is up, and the pause, cover and stage-clear screens. The HUD system writes the model each tick.
- **Pilots** (`pilot.cc`): the demo's script and the bot share one steering rule, which
  takes a boss's open parts as targets and keeps off its hull. Both
  drive the action map with `input_action_override`.
- **Effects** (`fx.cc`): sparks and debris are particles in a cosmetic kind, drawn from
  the module's own random stream, so the particle dial changes nothing the sim draws.
  Every air row gets a shadow on the ground's layer, a cosmetic child down and to the
  right that dies with it, born where it belongs. Explosions are Kenney's burst tiles as a
  flipbook (an anim clip made at startup) on a cosmetic row that goes when the clip ends,
  in three sizes (`fx_blast`): popcorn's pop; a ground row's or a part's two bursts; a
  boss's big one, four more around it, a ring of sparks and the screen white for a moment.
  Each enemy bullet has a halo, a dark soft disc in its color under it (a pale sea washes
  out anything that adds light), in two cosmetic kinds kept slot for slot with the bullet
  kinds: a second sprite a bullet, so the `glows` dial, off under `--stress`. A vignette
  darkens the field's edges (the HUD's first image).
- **Sound** (`sound.cc`, `assets/sound/`, `assets/music/`): a table of effects, each
  with its volume, its priority and how far its pitch wanders; the volley and the grazes
  lowest, deaths, the bomb, the egg and a boss's fall highest. The audio module starts a
  sound once a tick however often it is asked for, so a ring of bullets is one sound, and
  the effects follow the time scale in pitch (the egg's slow time drops an octave). The
  music is the stage's track, crossfading to the boss's at a boss mark and back after.
- **Reports** (`stats.cc`): the counters as the resource `example_arcfire.stats`, the log
  lines, the summary.

## Where things are

| file | what |
|---|---|
| `src/module.cc` | the registration tables, the schedule, startup, the sprite helpers |
| `src/arcfire.h`, `src/field.h` | what the files share; the field's bounds |
| `src/stage.cc`, `src/stage_data.cc` | the boot, the scroll, the marks; reading a stage file |
| `src/flow.cc` | the title, stage select, the pause menu; runs begun from a fresh sim and left, behind covers |
| `src/screens.cc`, `src/records.cc` | options, game over, results, name entry, the tables, the stage card; the files they keep |
| `src/path.cc` | Catmull-Rom paths by arc length |
| `src/dials.cc` | the dials' positions (the difficulties among them) and `--dial` |
| `src/scoring.h`, `src/scoring.cc` | the run's record and the score's rules; the chain's clock, extends, rounds, the results line |
| `src/planes.cc`, `src/ground.cc` | the planes and the ground's scroll; the map and its enemies |
| `src/ship.cc`, `src/weapons.cc`, `src/options.cc`, `src/enemies.cc`, `src/bullets.cc` | what moves; the weapons and the options |
| `src/boss.cc`, `src/boss_data.cc`, `assets/bosses/` | bosses: their life and their parts; reading a boss file; the files |
| `src/sound.cc`, `assets/sound/`, `assets/music/` | the effects' table and the music |
| `src/patterns.cc`, `assets/patterns/` | the library and the runners; the ten patterns |
| `src/hits.cc`, `src/egg.cc` | what happens when things meet; the death and the restart |
| `src/bomb.cc`, `src/items.cc` | the bomb; chips, pickups and the magnet |
| `src/hud.cc`, `src/fx.cc`, `src/pilot.cc`, `src/stats.cc` | the camera and the screens; particles; the demo and the bot; the reports |
| `src/main.cc` | the frame loop's config and the game's flags |
| `assets/stages/shoreline.stage.json`, `assets/maps/` | the stage; its ground map and tileset |
| `assets/ui/arcfire.ui.json` | the HUD and every screen |
| `assets/CREDITS.md`, `assets/LICENSE-*.txt` | who made what |
| `tools/make_art.py`, `tools/make_maps.py`, `tools/make_bosses.py`, `tools/make_sounds.py` | copies the vendored art in and draws `assets/made/`; writes the maps; kitbashes the bosses into `assets/generated/`; copies the sounds in and resamples the music |
| `tests/` | paths; the stage file, the boss files and the parsers' rules |
| `docs/` | the PRDs and the performance notebook |

What the game asks of the engine, found as it is built, is in
`docs/design/arcfire-improvements.md` at the repository's root.
