# Arcfire Slice 1 — Skeleton

Nov 9, 2026 · @starekrow

## Goal and exit criteria

The first slice makes `example_arcfire` exist and play for a minute: a 3:4 playfield in
side panels, three parallax planes scrolling, a ship that flies and fires Spread at its
first power level, popcorn formations and one turret from a stage script keyed to scroll
distance, aimed bullets, kills, grazes, the egg, a HUD, the dials, and the three ways the
game runs itself (`--demo`, `--autoplay`, `--stress`). It also makes the first ask of the
engine: time scale on the tick clock. Nothing in it is final art or final feel; everything
in it is the structure every later slice fills.

It is done when:

- `arcfire` opens on a 540 × 720 playfield centered in the window, dark side panels either
  side drawn by the UI module, and the camera's zoom fits the field's height to any window
  so that a run hashes the same on any screen.
- Three planes scroll under the field at their own rates (deep stars, high nebula, low
  cloud) as tiled cosmetic rows that wrap, and the notebook records what they cost in Set
  deltas, extract time and upload bytes per tick at play density and at stress density.
- The ship moves in eight directions at two speeds, fires a two-shot Spread at a fixed
  cadence while the button is held, and dies to an aimed bullet at a hitbox of a few
  world units. Grazes score and spark.
- A stage script (`shoreline.stage.json`, a minute of it) spawns popcorn formations and a
  turret as the scroll crosses its marks, with speed changes; a formation is one record
  (path, count, spacing, sprite, health, score, when it fires); every member fires one
  aimed bullet; the turret fires aimed bullets on a cadence while the ship is ahead of it.
- A death runs the egg: time to a quarter speed over a few ticks, the ship's four pieces
  tumbling, bullets near the death point cancelled into sparks, an egg that falls and
  rests and blooms into the new ship, time ramping back, a shell around the new ship that
  eats bullets for two seconds and fades. Three eggs; out of eggs restarts the stage with
  eggs refilled, the score as it was at the start, the restart counted.
- The HUD (a UI document) shows score, eggs as pips, restarts, and in dev builds the live
  counts (sprites, bullets, Set deltas, pairs tested) that the stress runs read.
- `arcfire --demo --hash` plays a tick-scripted minute that fires, kills, grazes, dies
  once, bombs nothing, and quits on the stage's end event, and matches across runs,
  `--threads 1` and `8`, `--io sync` and `uring`, and pack, zip and loose files.
- `--autoplay` puts the bot on the stick at Normal; `--stress` turns the dials to their
  stress position with the ship invulnerable and the stage looping; both log the game's
  counters every report interval and a summary at exit, and the notebook has its first
  entries from them.
- `engine_time_scale` exists, is a tick-clock feature with a test, and a demo that dies
  hashes exactly as one that does not would at the same ticks.
- Every asset the slice uses is vendored with its license text and a row in
  `assets/CREDITS.md`; validation stays clean; the leak report is empty; the module has a
  README in the shape of rocks' and arcanum's.

## The frame

**The field (R1).** The sim's field is 540 × 720 world units centered on the origin, +y up.
Nothing in the sim reads the window: a `camera` system (PostSim) sets the camera's zoom to
`window.height / 720` so the field fills the window's height, and the UI anchors the side
panels to the window's left and right edges, filling the height, wide enough to cover
whatever of the world shows past the field at any aspect. A window narrower than 3:4 shows
less of the panels, never less of the field. The field's bounds are constants in one
header (`arcfire/field.h`) and every system that clamps or culls reads them from there.

**Planes (R2).** Kind `plane` (cosmetic: `Transform2`, `Sprite`, `Plane{rate, row, col}`),
one row per tile. A plane is a texture (512 × 512 from the backgrounds pack, drawn at 2 ×
so a tile is 1024 units), a layer and a rate; the game lays tiles across the field's width
plus a margin and down its height plus one tile. Each tick `scroll` moves every tile by
`speed * rate * dt` downward and wraps a tile that has left the bottom to the top, by a
whole tile height, as a teleport (writing the previous copy so the renderer snaps). Three
planes: deep stars (rate 0.15, layer -30, opaque), high nebula (rate 0.4, layer -20,
alpha, tinted down), low cloud (rate 1.6, layer 10, alpha, translucent, above the enemies
that will be ground in slice 3 and below the air). The stress dial multiplies the tile
density (smaller tiles, more rows) to make the extract's share visible. The notebook gets
the measurement, the improvements note's parallax item gets the conclusion.

**The dials (R3).** `Dials` is a resource the game publishes at startup and never changes
during a run (a change is a restart): bullet density and speed scales, formation count and
spacing scales, particles per event, plane tile scale, enemy health scale, bot on or off,
ship invulnerable or not, stage loops or ends. Three positions in slice 1: `Normal`,
`Stress`, and `Demo` (Normal with the script on the stick). `--dial name=value` overrides
one for a measurement. The dials are a sim table of one row so the hash covers them, and
the log prints them at startup.

## The ship

**Flying (R4).** Kind `ship` (1 row: `Transform2`, `Sprite`, `Collider2`, `Ship`). Actions:
`move` (Axis2: WASD, arrows, left stick, d-pad), `shot` (Z, Space, pad South), `focus`
(Shift, pad West: half speed, hitbox shown), `bomb` (X, pad East: nothing yet), `pause`
(Escape, Start), `quit` (Q). `control` (Sim) reads the actions and writes the ship's
position, clamped to the field less the sprite's half size, at 6 or 3 units a tick. The
ship's sprite is a pack fighter at 2 ×; the hitbox is a circle of radius 3 at its center,
on layer `ship`, meeting `bullet` and `air`; a child sprite shows it while focused.

**Spread, level one (R5).** While `shot` is held, every 6 ticks `control` spawns two
`shot` rows (kind `shot`, 2048: `Transform2`, `Sprite`, `Velocity`, `Collider2`,
`Shot{damage}`) angled 6° either side of up at 14 units a tick, layer `shot` meeting
`air` and `ground`. `shots` (Sim, `parallel_for`) integrates and destroys what leaves the
field. A shot hitting an enemy is `ShotHit`; the enemy takes `damage`, flashes white for
two ticks (its tint), and dies at zero.

**Graze (R6).** A second collider on the ship is not possible (one `Collider2` per row), so
the graze ring is a child row of kind `ring` (1: `Transform2`, `Local2`, `Parent`,
`Sprite`, `Collider2`), radius 18, layer `graze` meeting `bullet`, sprite invisible unless
focused. `OverlapBegin` between the ring and a bullet is a graze: +10 score, a spark, the
bullet marked grazed (one graze per bullet). A bullet that hits the hitbox cancels its graze
(no points for dying).

## Enemies and bullets

**Popcorn (R7).** Kind `air` (256: `Transform2`, `Sprite`, `Collider2`, `Health`,
`Mover`, `Emitter`). A `Mover` follows a path: a list of up to eight points in field
units with a speed, interpolated as a Catmull-Rom spline by arc length so members of one
formation keep their spacing; past the last point the row is destroyed. `enemies` (Sim)
advances every mover. An `Emitter` in slice 1 is one rule: fire `n` aimed bullets at path
parameter `t` (the BulletML runtime replaces it in slice 2 without changing the kind). A
popcorn's death is `Kill`: score, a spark burst, nothing dropped yet.

**Formations (R8).** A formation record in the stage script: `path`, `count`, `spacing`
(ticks between members), `sprite`, `health`, `score`, `fire_at`, `fire_count`. Spawning a
formation schedules its members by `ctx.schedule` on the ticks the spacing gives, from the
spawn tick, so a formation is one event and its members arrive on time whatever the frame
rate. The count and spacing scale by the dials.

**The turret (R9).** Kind `ground` (256: `Transform2`, `Sprite`, `Collider2`, `Health`,
`Emitter`, `Ground`). A ground row moves with the scroll at rate 1.0 (so in slice 3 it
rides the map) and dies at the bottom of the field. The turret fires one aimed bullet every
`cadence` ticks while the ship is above it, and nothing while the ship is below (it does
not turn around). Its sprite is a pack turret at 2 × on the ground plane's layer; it has no
shadow. The ship flies over it: its collider is on layer `ground`, which the ship's mask
does not include.

**Bullets (R10).** Kind `bullet` (16,384; the stress dial asks for 65,536 and gets what the
capacity allows until the engine's capacity work in slice 2): `Transform2`, `Sprite`,
`Velocity`, `Collider2`, `Bullet{life, grazed, owner}`. An aimed bullet is spawned at the
emitter's position with a velocity toward the ship's position at that tick (leading it by
nothing: moderate), at a speed the dials scale, with a sprite from the bullet sheet and an
additive glow (a second material on the same sheet) under it as the bullet's own row is
one quad, so in slice 1 the glow is the sprite and the core is the tint; a second row per
bullet is slice 5's call. `bullets` (Sim, `parallel_for`) integrates every bullet, counts
its life down, and destroys what leaves the field or runs out. Bullets are on layer
`bullet`, meeting `ship`, `graze` and `shell`; they never meet each other or anything else,
and the notebook records how many pairs the broadphase tests anyway.

**Hits (R11).** Handlers over `OverlapBegin` sort pairs by kind into `ShotHit`, `BulletHit`
(bullet on hitbox), `Graze`, `ShellHit` (bullet on shell: the bullet dies in a spark on the
shell). A `BulletHit` while the ship is invulnerable (dials, or the shell's grace) does
nothing. Otherwise it is `ShipDied`.

## The egg

**The sequence (R12).** `ShipDied` starts the egg state machine, a resource and a system
(`egg`, Sim, `Always`, so it runs through the slow time). Ticks below are sim ticks; the
wall time is the engine's time scale's business.

1. Tick 0: `engine_time_scale` ramps to 0.25 over 6 ticks (the game sets it each tick).
   The ship's row hides (tint alpha 0, collider mask 0). Four `particle` rows take the
   ship's four quarter uvs and tumble out with spin and drag. Every bullet within 120
   units of the death point is destroyed into a spark. The camera shakes.
2. Tick 10: the egg (kind `egg`, 1: `Transform2`, `Sprite`, `Egg`) spawns at the death
   point, falls at 1 unit a tick with a little drift, and rests at tick 40.
3. Tick 40: the egg blooms: its scale grows over 20 ticks, the shell halves split as two
   particles, and the ship's row is placed at the egg's position, tint rising to full,
   rising 20 units into place; time scale ramps back to 1 over the same 20 ticks.
4. Tick 60: the ship is live; the shell (kind `shell`, 1: a child of the ship with a
   `Collider2` of radius 24 on layer `shell` meeting `bullet`, a translucent additive
   sprite) is up for 120 ticks and fades over its last 20; the ship's own mask is restored
   when the shell goes.

Eggs start at 3 (one row in the `Game` resource with score, eggs, restarts, stage score at
start). A death costs an egg at tick 0. A death at zero eggs skips to the restart (R13).
`focus`, `shot` and `move` do nothing during steps 1–3 (the sequence is watched, not
played); `pause` still works.

**Restart (R13).** Out of eggs: a fade to black through the level module's quad is slice
3's; here a UI screen covers the field (a cut), every row of every game kind is destroyed,
the scroll distance returns to 0, the stage script's cursor to its first mark, eggs to 3,
the score to the stage's starting score, `restarts` + 1, and the screen uncovers after 30
ticks. The planes do not reset (they are cosmetic and continuous).

## The stage script

**Marks (R14).** `shoreline.stage.json`: `speed` (the starting scroll speed in units a
tick) and `marks`, each a `distance` and an action: `formation {record}`, `ground {kind,
x}`, `speed {value, over}` (a ramp over a distance), `end`. The `stage` system (Sim) keeps
a cursor over the marks sorted by distance and runs each as the distance crosses it, which
is the one place the game turns distance into events. `end` emits `StageEnd`: the demo
quits on it, the stress mode loops (the cursor returns to the first mark and the distance
keeps counting from where it is, the marks offset by the stage's length), play mode shows
a "stage clear" label and loops too, since there is no second stage yet. The minute of
script has eight formations, two turrets, two speed changes.

**Distance (R15).** `scroll` (Sim) adds `speed * dt` to `Stage.distance` every unpaused
tick and moves the planes and the ground rows by it. Distance is a field of a sim table, so
the hash covers it, and nothing reads a tick count to know where the stage is.

## The HUD and the panels

**Documents (R16).** `hud.ui.json`: two panels anchored left and right, fill height, a
fixed width that covers past the field at 16:9 (the panels' inner edges are the field's
edges at the design height); in the left: `SCORE` and `{=score}`, `EGGS` and a meter of
pips bound to `eggs` with the egg icon, `RESTARTS {=restarts}`; in the right: the stage
name, and under a `dev` style flag the counters (`{=sprites}`, `{=bullets}`, `{=deltas}`,
`{=pairs}`), which `hud` (PostSim, `Always`) writes into the model each tick. The font is
Public Pixel at 16 px, pixel-scaled. A `cover` screen for the restart (R13) and a `pause`
screen (a dimmer and the word, no menu yet; slice 5 brings the menu).

## Running itself

**The demo (R17).** `--demo` is a script in code, as rocks does it: a table of (tick,
action, value) applied through `input_action_override` by a system that runs first in
Input, driving the ship across the field, holding fire, grazing the first formation's
bullets on purpose, standing in a bullet at a known tick (the egg plays once), and
quitting on `StageEnd`. `--hash` checks it as the other examples are checked. The demo's
dials are `Demo`.

**The bot (R18).** `--autoplay` and `--stress` put a bot on the stick: a system (Sim,
before `control`) that overrides the actions from sim state only: it holds fire, and each
tick looks at the bullets within 80 units, sums their approach vectors weighted by
closeness, and moves away from the sum, clamped to the field, drifting back to the field's
center column when nothing threatens. It is deterministic and it is not good, which is
what the stress mode wants (the ship is invulnerable there) and enough for autoplay to
survive a minute of Normal most of the time.

**Reporting (R19).** The game publishes its counters as a resource `arcfire.stats`: live
rows per kind, bullets spawned and destroyed this tick, Set deltas this tick (from the
sprite module's delta array length), pairs tested (the collision module exposes a count if
it has one; if not, that is the first line of the notebook and an item for the note). At
every report interval it logs them beside the frame loop's fps line, and at exit a summary
with peaks and means. `--prof-report` is the engine's half. The notebook's first entries
are: Normal autoplay for the minute; `--stress` for the minute; and the plane-density
measurement at three tile scales.

## The engine's part

**Time scale (R20).** `engine_time_scale(e, f32 scale)` and `engine_time_scale_get(e)`.
The tick clock accumulates `frame_dt * scale` instead of `frame_dt`; `tick_dt` is
unchanged, so a tick is still a sixtieth of sim time and systems see the same `dt`; the
alpha the renderer interpolates by is unchanged in meaning. Scale 0 is a hold that still
counts as held ticks do not, which is to say it is not a pause: nothing runs. The CLI gets
`time scale N`. A test: a clock fed ten seconds at scale 0.25 runs 150 ticks. The frame
loop's fps line shows the scale when it is not 1. The audio mixer is untouched in this
slice (voices run at their rate while the sim crawls; whether `pause_with_sim` voices
should slow is slice 3's question when the game has sound).

**Measured, not changed (R21).** The extract's compare cost at 20,000 moving rows, the
broadphase's pair count against masked-out bullets, the renderer's table growth at the
first stress run: each gets a notebook line and, where it confirms an item in the
improvements note, a sentence there with the number.

## Assets

Taken on the slice's first day, each archive's license read from its own file, the hash
recorded, every file in the credits table, as arcanum's:

- Kenney *Pixel Shmup* (CC0): the player's fighter, two popcorn ships, one turret, the
  egg (a pack item recolored is fine if it is noted; otherwise drawn here), UI frame pieces.
- M484 *Bullet Collection 1* (CC0): one round bullet and one needle, cut into a sheet here.
- Screaming Brain Studios *Seamless Space Backgrounds* (CC0): one starfield, one nebula;
  the cloud plane is a nebula tinted and faded.
- GGBotNet *Public Pixel* (CC0): the HUD font.

Made here: the hitbox and ring sprites (circles), the shell (a soft circle), the sparks (a
4 × 4 white square), the ship's quarter uvs are the fighter's own texture. The ship pieces
and the egg are the only art decisions the slice makes; both may be replaced.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | The 540 × 720 field, camera fit to height, panels cover the rest | P0 |
| R2 | Three planes as wrapping cosmetic tiles; the cost measured | P0 |
| R3 | `Dials`: a hashed resource; Normal, Stress, Demo; `--dial` | P0 |
| R4 | The ship: actions, two speeds, clamp, hitbox, focus | P0 |
| R5 | Spread level one; `shot` kind; `ShotHit`; flash and death | P0 |
| R6 | The graze ring as a child; `Graze` | P0 |
| R7 | `air` kind: spline movers, the one-rule emitter, `Kill` | P0 |
| R8 | Formations as records; members by scheduled events | P0 |
| R9 | `ground` kind: rides the scroll; the turret | P0 |
| R10 | `bullet` kind: aimed bullets, life, bounds, masks | P0 |
| R11 | Pair sorting handlers; invulnerability | P0 |
| R12 | The egg sequence | P0 |
| R13 | Out of eggs: the restart | P0 |
| R14 | The stage script: marks, actions, `StageEnd`, looping | P0 |
| R15 | Distance as the stage's clock | P0 |
| R16 | HUD, cover and pause documents; the dev counters | P0 |
| R17 | `--demo --hash` | P0 |
| R18 | The bot; `--autoplay`, `--stress` | P0 |
| R19 | `arcfire.stats`, the report lines, the first notebook entries | P0 |
| R20 | `engine_time_scale` in the tick clock, CLI, test | P0 |
| R21 | Extract, broadphase and growth measured under stress | P1 |

## Milestones

Each leaves the game building, running and (from M3) hashing.

1. **Field and planes** (R1, R2, R3): the module, the assets and credits, the window, the
   panels, three planes scrolling at Normal and Stress density, the first notebook entry.
2. **Ship and shots** (R4, R5): flying, firing, a hand-placed row of popcorn to shoot.
3. **Stage** (R7, R8, R9, R10, R11, R14, R15, R6): formations and the turret from the script,
   aimed bullets, hits, grazes, the ship dying with no sequence yet (a respawn cut), the
   demo script and `--hash` matching.
4. **The egg** (R20, R12, R13): time scale in the engine, the sequence, the restart.
5. **HUD and modes** (R16, R17, R18, R19, R21): the documents, the bot, the modes, the
   reports, the notebook entries, the README.
