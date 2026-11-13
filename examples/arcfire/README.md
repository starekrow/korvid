# arcfire

A vertical bullet-hell shooter, and the engine's stress test that is also a game
(`docs/Arcfire-Master-PRD.md`), built in six slices (`docs/Arcfire-Slice-N-PRD.md`). Two
stages on a 3:4 field between side panels:

- **Shoreline**: a Tiled map of sea and islands scrolling under two decks of cloud;
  popcorn, turrets, tanks and bunkers; heavies and a missile boat; the carrier and the
  battleship.
- **Orbit**: a procedural nebula drawn by a shader of the game's own under the stars; a
  fleet battle in the background, a capital ship breaking up under the dissolve shader;
  debris, station segments with turrets; a pair of gunships and the station core.

A ship that flies in eight directions at two speeds, with four weapons at five power
levels and up to two options; BulletML patterns; bombs, chips and grazes; the chain,
extends and the stage bonus; the egg, a death in slow motion after which a small machine
builds the next ship; bosses made of parts, in phases; the air bending around every big
explosion (a full-frame post shader). Easy, Normal and Hard; arcade, stage select, high
scores, options; a demo and a bot that fly it. Every count is a dial. The art is CC0, from
Kenney, Screaming Brain Studios, Master484 and GGBotNet, and the bosses and Orbit's pieces
are drawn here in Kenney's style; the sound is Kenney's and the music Juhani Junkala's,
CC0 too (`assets/CREDITS.md`). How it performs, slice by slice, is
`docs/performance-notes.md`.

## Watching it work

Built (below), from `build/linux-dev/bin`:

| to see | run |
|---|---|
| the game as a player meets it | `./arcfire` (the title; left alone 20 s, it plays the demo) |
| Shoreline flown by its demo (3.5 min) | `./arcfire --demo` |
| Orbit flown by its demo (5.5 min): the gas, the battle, the dissolve, the gunships, the station core | `./arcfire --demo --stage orbit` |
| both stages in arcade, the stage change, the bot flying | `./arcfire --arcade --autoplay` |
| twenty thousand bullets | `./arcfire --stress` (Shoreline) or `--stress --stage orbit` (and 3,000 ships in the battle) |
| one pattern, measured | `./arcfire --bench ring` (any name in `assets/patterns/patterns.json`) |
| determinism | `./arcfire --demo --hash --threads 1` against `--threads 16`: the same hashes, tick for tick; `--again` plays it twice in one process |
| a recorded run replayed | `./arcfire --replay ../../../examples/arcfire/tests/replays/normal.kvr` (ends `matched`) |
| timings | add `--spike-hunt --prof-report`; to run a long mode four times faster, `--cli-stdin` and type `rate 4` |

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory:

```bash
tools/kormake/bin/linux/kormake arcfire dev    # or, in this directory: kormake dev
build/linux-dev/bin/arcfire
```

Controls: arrows or WASD (or the left stick or the d-pad) fly; Z or Space (South) fires,
held; Shift (West) focuses: half speed, the hitbox and the graze ring shown, the spread
narrowed; X (East) bombs; Escape (Start) pauses; Q quits.

Run plainly, the game opens on its title, over Shoreline's sky: Arcade, Stage select
(Shoreline or Orbit, then Easy, Normal or Hard), High scores, Options, Quit. Arcade is the
stages in order, Shoreline then Orbit: at a stage's end the stage-clear banner with the
stage bonus, then behind a fade the next stage (its card says "STAGE 2"), the ship, its
weapon and options, the score and the eggs carried; after Orbit, Shoreline again a
difficulty up, until the round at Hard ends the run. A run starts behind a fade from a fresh sim, so every run from the title is the
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
NAME` (`shoreline` or `orbit`: `assets/stages/NAME.stage.json`), and the ways it flies itself below; `--difficulty
easy|normal|hard` is where arcade or the stage starts (Normal). Left alone on the title for 20 seconds (`--attract SECONDS`; 0 never), the game plays the
demo from a fresh sim with PRESS START blinking over it, hashing tick for tick as `--demo`
does; a key or a pad's button, or the demo's end, goes back to the title.

`--record FILE` writes a run's input and `--replay FILE` plays it back, checking the state
hash every 60 ticks (the frame loop's; its README has the file). A run started from the
title records the arguments that start it, so its replay goes straight in. Two recorded bot
runs, Shoreline at Normal and Hard, are in `tests/replays/`; replayed from the repository
root, `build/linux-dev/bin/arcfire --replay
examples/arcfire/tests/replays/normal.kvr` must end `replay: ended at tick 14122,
matched`. A change to the
sim makes them diverge, and they are recorded again (`--autoplay --stage shoreline
--difficulty normal --record ...`).

`--again` is a check: a run
that ends starts once more from a fresh sim instead of quitting, so `--demo --again --hash`
prints the demo's hashes twice, and the two must match. Over `--cli-stdin`, `press ACTION
[TICKS] [X Y]` holds an action as a player would, `type TEXT` types, and `screenshot FILE`
takes the next frame, so a script can walk the menus.

The ways it flies itself:

- `--demo` plays the whole stage (Shoreline: about three and a half minutes) and quits at its end:
  it fires throughout, flies focused through the first formation's stream (the ring
  grazes), flies by the bot's rule (below) otherwise, bombs in the spiral at tick 1440 and
  climbs past the magnet line for the chips, and from tick 1650 stands still and holds
  fire until a bullet finds it (the egg plays once; nothing hits it after, the demo's
  `invulnerable` being 2). It wants its weapons by the stage's clock: Spread on the
  opening's power items (to level five), the Beam at the islands, Storm over the airfield
  and the carrier (a bomb in its core's phase), Lance and both options for the
  battleship; it takes an egg on the way. The end mark ends it at tick 13974, the last sim tick; it
  then reads its results, signs "DEMO" into its own table (`scores-demo.json`) and quits. `--demo --hash` prints the state hash every sim tick. The hashes match
  across runs, thread counts (`--threads 1`, `--threads 8`), I/O backends (`--io sync`,
  `--io uring`) and asset sources (loose files, `--pack` with a kpak of `bin/assets`, or
  `--pack` with a zip of it). `--demo --stage orbit` is Orbit's (about five and a half
  minutes, 19,702 ticks): no script but the rule, its own weapons by the stage's clock
  (Spread, Storm from the segments on, Lance for the station core), a bomb when the core
  opens; it dies early once, so the egg plays, and hashes as the other does.
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
  twice the enemy health, the bot flying an invulnerable ship, the stage looping, and in
  Orbit 3,000 ships in the battle.
- `--bench NAME` fires one pattern of the library from a fixed emitter at the top of the
  field, again and again, under an invulnerable bot: how a pattern is written and measured.
- `--gas` lays the nebula gas (Orbit's deep plane, a game shader) over the stage's field:
  how the shader is seen before Orbit is a stage.
- `--dial name=value` moves one dial (repeat it for more): `rank`,
  `bullet_speed`, `formation_count`, `formation_spacing`, `particles`, `plane_density`,
  `enemy_health`, `bot`, `invulnerable` (1, or 2: after the first death), `loops`,
  `weapon` and `power` (what the ship starts with: 0 Spread, 1 Beam, 2 Storm, 3 Lance; 1..5),
  `options` (0..2, the options it starts with), `chain_window` (ticks; 0 turns the chain off), `glows` (1, the bullets' halos; 0 under `--stress`),
  `shimmer` (1, the air bending around big explosions; 0 draws the frame without the
  full-frame pass), `fleet` (300, Orbit's battle's ships; 3,000 under `--stress`; 0 none),
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
- **The stages** (`stage.cc`, `stage_data.cc`) are `assets/stages/shoreline.stage.json`
  and `orbit.stage.json`, both read at startup: a speed, named paths, and marks by distance
  (a formation, a ground row, a station segment, a speed change, a boss or several, the
  end). The world row's `Stage` says which is being played; arcade's change from one to
  the next is a held tick behind a cover (`stage_change`): the field cleared, the next
  stage's map, planes and music, the run carried. `scroll` adds the speed to the distance every sim tick; `stage` runs every mark
  the distance has passed. A formation mark schedules its members as events on the ticks
  their spacing gives; each member's event carries the run's epoch, so members scheduled
  before a restart never arrive. The sim holds (`HoldLoading`) until the file is in.
- **Planes** (`planes.cc`): grids of tiles of a seamless texture in a cosmetic kind, each
  on a layer of its own, which a stage names: space's three (stars, a nebula, a faint
  cloud: the bench's), the sky's (Shoreline's), or Orbit's: the gas (one full-field sprite
  drawn by the nebula shader, its uv scrolled), the stars added over it, the faint cloud. The sky is the parallax over the map: the
  low clouds at 1.35 times the ground's speed under the air, their shadows on the sea and
  islands (the same texture darkened, down and to the right, at the same rate), and high
  wisps at 2.1 times over the air and the bosses but under the ship, its shots and the
  bullets. The tiles never move: each plane's layer is scrolled by its rate times the speed
  (`sprite/layers.h`), wrapped by a whole tile as a snap, so a plane is one value a tick.
- **The ground** (`ground.cc`, `assets/maps/`): a stage may name a Tiled map, here
  Shoreline's sea and islands, written by `tools/make_maps.lua` from Kenney's terrain tiles
  and drawn at twice their size. Its 15,000 tiles are spawned once into the tilemap
  module's still kind and scrolled by layer with the stage's distance, so they cost the
  extract nothing a tick. The map's `enemies` layer places the ground rows: turrets, tanks
  that drive their road, bunkers that launch popcorn up a stage path. Each comes into view
  where its place on the map crosses the field's top, and rides the scroll from there.
  Orbit has no map and no ground but its station segments: a segment mark puts a piece of
  station (`generated/segments.png`, a truss or a module) on the map's layer, which scrolls
  with the distance as the ground rows do, and two turrets (ground rows) on its pads. The
  picture is cosmetic (a plane row); its turrets are the sim's. Where there is no ground,
  nothing casts a shadow.
- **The ship** (`ship.cc`): `control` reads the action map and moves, clamps and fires
  its weapon.
- **Weapons** (`weapons.cc`): four types at five power levels, rows of tables: Spread (a
  fan, wider and denser), Beam (shaped by level: one beam, then two from the ship, then each
  splitting halfway up, then three with the outer two split, then all three split; each
  segment, a child row of its owner, is cut each tick by `collision_cast_all` at the first
  body it meets, passing over a boss's hull, and damages it every few ticks: the widest
  coverage and the least on any one target; focused, the shape draws in; an option's is one
  straight beam that leans after the ship's sideways movement and hardens a little with
  its level), Storm (bolts that turn toward the nearest enemy) and Lance
  (heavy shots that pass through what they kill). The level is the ship's, kept when the
  type changes and one lower after a death; weapon and power items (`items.cc`) switch and raise them. Item carriers
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
  the ship is above it. A hit flashes them by drawing their texture added. Besides the
  popcorn, the heavies (the pack's big yellow ship, in threes, forty-odd health, denser
  patterns) come just before the carrier and through the second half, and three times a
  missile boat (a midsize airship, `generated/boat.png`, a formation of one) crosses the
  upper field slowly, salvo after salvo of homing missiles (`patterns/missiles.xml`).
  Orbit adds debris (`generated/debris.png`): slow formations of wreckage that tumble,
  take a few shots and never fire.
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
  bay; then two deck hatches; then the bridge). Orbit has two more: a pair of gunships
  that come on one mark and cross each other's paths as they sway (each two turrets, then
  its bow gun; the stage waits for both), and the station core at the end, the game's last
  three phases (a ring of six shield emitters, then four weapon mounts, then the core). The art is cut by `tools/cut_art.lua` (`kormake run`)
  from the CC0 packs (ARES hulls, surt's ship parts, Commander's capital ship) into
  `assets/generated/`. The HUD shows a boss's parts' health as a
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
- **Shaders** (`shaders/`, `assets/materials/`): fragment shaders of the game's own, built
  by the shader rule into `bin/shaders/` and named by a material file's `"shader"`. The
  nebula (`arcfire_nebula.slang`) is layered noise between a dark and a bright color,
  drifting with the render time and scrolled by its plane. The dissolve
  (`arcfire_dissolve.slang`) cuts a sprite away where noise falls below its tint's alpha,
  with a hot edge along the cut: a boss's hull burns away as it dies, nine tenths gone as
  it breaks, its shadow fading with it. The shimmer (`arcfire_shimmer.slang`) is a
  full-frame pass over the world (every layer under `Z_POST`, so not the HUD): a big
  explosion (a part's or a ground row's, a boss's, its hull's break, the bomb) starts a
  ring of bent air (`fx_shimmer`, up to sixteen), and `hud.cc`'s `shimmer` hands the
  renderer each ring's place in window pixels, radius and strength every tick. A shader
  that will not load leaves its sprites drawn plainly, or the frame drawn without the pass.
- **The battle** (`battle.cc`): Orbit's background, all cosmetic (three kinds the hash
  never sees, the module's own random stream). The capital ship, four sections of
  `generated/capital.png`, drifts down the stage; its sections break away at fractions of
  the stage's distance, each burning away under the dissolve shader with bursts and a ring
  of bent air, so a restart puts it back whole. Attackers (orange) come at it in waves from
  the field's edges and strafe it pass after pass, then leave; its fighters (cyan) chase
  them, or with none near wheel after a mote round the ship; both trade tracers that end in
  flashes and bursts. Every ship steers (a seek no tighter than its acceleration) and faces
  where it goes. Once the last section is gone the fleet flies off the field before the
  station core comes. The `fleet` dial is how many ships.
- **Sound** (`sound.cc`, `assets/sound/`, `assets/music/`): a table of effects, each
  with its volume, its priority and how far its pitch wanders; the volley and the grazes
  lowest, deaths, the bomb, the egg and a boss's fall highest. The audio module starts a
  sound once a tick however often it is asked for, so a ring of bullets is one sound, and
  the effects follow the time scale in pitch (the egg's slow time drops an octave). The
  music is the stage's track, crossfading to the boss's at a boss mark and back after;
  each stage has its own two.
- **Reports** (`stats.cc`): the counters as the resource `arcfire.stats`, the log
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
| `src/battle.cc` | Orbit's battle: the fleet, the tracers, the capital ship |
| `src/hud.cc`, `src/fx.cc`, `src/pilot.cc`, `src/stats.cc` | the camera and the screens; particles; the demo and the bot; the reports |
| `src/main.cc` | the frame loop's config and the game's flags |
| `shaders/`, `assets/materials/` | the game's shaders (the nebula, the dissolve, their noise; the shimmer, a post shader); the materials that name the first two |
| `assets/stages/shoreline.stage.json`, `assets/stages/orbit.stage.json`, `assets/maps/` | the stages; Shoreline's ground map and tileset |
| `assets/ui/arcfire.ui.json` | the HUD and every screen |
| `assets/CREDITS.md`, `assets/LICENSE-*.txt` | who made what |
| `tools/make_art.lua`, `tools/cut_art.lua`, `tools/make_maps.lua`, `tools/make_sounds.lua` (`kormake run`) | copies the vendored art in and makes `assets/made/`'s cuts and plain shapes; cuts the bosses, Orbit's segments, debris and capital ship, the egg, the items, the beam and the clouds from the CC0 packs; writes the maps; copies the sounds in and resamples the music |
| `tests/` | paths; the stage file, the boss files and the parsers' rules |
| `docs/` | the PRDs and the performance notebook |

What the game asks of the engine, found as it is built, is in
`docs/design/arcfire-improvements.md` at the repository's root.
