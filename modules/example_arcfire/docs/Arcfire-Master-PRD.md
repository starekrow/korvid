# Arcfire — Master PRD

Nov 9, 2026 · @starekrow · revised after the first discussion; Nov 10: Fortress cut (slice 5 PRD)

## What it is

`example_arcfire` is a vertical bullet-hell shoot-'em-up on Korvid, and the engine's
first stress test that is also a game. Architecturally it is *Blazing Lazers*: a fixed
upward scroll over a stage laid out as a timeline, air and ground enemies of every size,
weapons swapped and powered by pickups, a mid-boss and a boss per stage. On top of that
shape it takes the bullet-hell staples: thousands of slow, patterned bullets, a hitbox far
smaller than the ship, grazing for score, a bomb that clears the screen, one hit and the
ship is gone. The visuals go deeper than the model: four to six parallax planes, an
animated ground that enemies sit on, beams, additive glows, hit flashes, debris and sparks
in the thousands, a screen that shakes, and a last stage whose background is a battle.

**It looks dangerous and is not.** The point of the game is to watch it work: to see the
patterns bloom, the planes slide past each other, the bosses come apart. Difficulty is
moderate at most. The screen is full of bullets because that is the sight; the patterns
leave lanes, the aimed shots lead the player a little less than they could, and the hitbox
is a few pixels. A player who stops paying attention dies; a player who is paying
attention sees the whole game in a sitting. Death is a spectacle (the egg, below), not a
setback: it costs the options, the bomb count and a level of the weapon's power (revised
in the tuning after slice 5: a death should hurt a little).

Everything that hits is in one plane: a player shot kills a ground turret as it kills a
fighter, an enemy bullet comes from a tank as it comes from a ship, and the ship flies over
ground enemies without touching them. Ground and air differ in how they draw (which plane,
under or over the clouds), how they move (with the scroll, or freely) and how they shoot,
not in what they collide with. That stays so until a slice has a reason to change it.

It is a stress test by design: every count is a dial, the game plays itself from a script
and hashes, and a `--stress` mode turns the dials past what the game needs so the renderer,
the broadphase, the extract and the schedule can be measured and the profile read. What
the engine cannot do, or does badly, goes in `docs/design/arcfire-improvements.md` as it
is found; each slice fixes what it must and leaves the rest listed. The numbers go in
`performance-notes.md` beside this file, a notebook, not a scoreboard.

## Goals

1. **A real game.** Two stages, each with its own ground or backdrop, parallax set,
   enemy roster, mid-boss and boss; a title, a results screen, high scores with name
   entry, a pause menu, options; keyboard and pad; music and sound. Finishable in a
   sitting by someone who is watching.
2. **Legible.** Someone reading the source and the data sees how a shmup is built: a stage
   is a timeline, an enemy is a row with a behaviour, a pattern is a BulletML document, a
   boss is a tree of parts. Nothing is special-cased that could be data.
3. **A stress test that reports.** The dials, the modes that turn them, the profile; the
   observations in the notebook; the engine findings in the improvements note.
4. **Reproducible.** `arcfire --demo --hash` plays a scripted run through stage one and
   matches across runs, thread counts, I/O backends and pack, zip and loose files, as the
   other examples do. Patterns draw from the sim's random stream, stage events are keyed
   to scroll distance, nothing is keyed to a frame.
5. **Clean attribution.** Every asset CC0 or CC-BY, every file's origin in a credits
   table, every license text vendored, as `example_arcanum/assets/CREDITS.md` does it.
   Generated art is kept apart and labelled as such.

Not goals: a level editor, networking, a second bullet plane, cross-platform determinism,
a scoring system deeper than score, graze and a chain multiplier, a difficulty that
anyone would call hard.

## Frame

The window is the frame loop's default (1280 × 720). The playfield is 540 × 720 world
units at zoom 1 (3:4, the arcade shape), centered; the side panels are the HUD. The
camera is fixed; the world scrolls under it. Sim at 60 Hz; render at the display's rate,
interpolated. `--tate` is not a goal but nothing should make it hard: the playfield's size
is the sim's and only the camera zoom and the HUD anchors know the window.

**Dials.** One `Dials` record, published as a resource, read by everything that counts:
bullet density and speed scale (BulletML's `$rank` maps onto it directly), formation
sizes, particle counts per event, plane tile density, enemy health scale, the bullet and
particle capacities. Difficulty is a position of the dials (Easy, Normal, Hard; Normal is
the moderate default), `--stress` is another (every count up, the ship invulnerable under
the bot, the stage looping), and a slice's measurement is a third it names in the notebook.
Hashing covers the dials, so a difficulty is a different run and the same difficulty is
the same run.

## The game

**Scroll and planes.** The stage scrolls upward at a speed the stage script sets and
changes. Scroll distance is the stage's clock. From back to front: a deep plane (stars or
sky, slowest), a high plane (nebula or cloud), the ground (a Tiled map at the stage's
speed), ground enemies and the shadows of air enemies (offset by altitude), a low cloud
plane over the ground (faster than the ground, translucent), air enemies, the player,
bullets, particles, beams and flashes, the HUD. Each plane is a layer range and a parallax
rate. Slice 1 scrolls the planes by moving their sprites and measures it; the improvements
note holds the renderer-side alternative.

**The ship.** Eight directions at two speeds (focus slows it, shows the hitbox and
narrows the shot). The hitbox is a few units at the ship's center. Grazing: a ring around
the hitbox; a bullet that crosses the ring and not the box scores and sparks.

**The egg.** One hit kills the ship, and that is the game's set piece:

1. Time slows (the sim runs at a quarter speed; the engine's clock scales, see the
   improvements note). The ship breaks up over a second of slow time: parts tumble, the
   bullets around it cancel into score chips, the screen flashes and shakes.
2. As it breaks, it drops an egg: a small bright thing that falls a little and comes to
   rest where the ship was.
3. The egg blooms: the shell opens into the new ship, which rises into place as time ramps
   back to full over the next second.
4. The shell stays as a translucent bubble around the new ship for about two seconds of
   full-speed time, absorbing bullets (each one a spark on the shell), then fades.

Eggs are lives. The HUD shows them. A death costs the options and resets the bomb count
to its base and drops the weapon a power level (not below one); the weapon stays. An
extend is an egg. Out of eggs returns the
player to the start of the stage with the eggs refilled and the stage's score as it was at
the stage's start; the results screen counts the restarts. Nothing is lost that cannot be
regained in a minute, which is the difficulty the game wants.

**Weapons.** Four types, each with five power levels, switched by picking up its item and
powered by picking up the same one again:

- **Spread**: a fan, wider and denser with power. The default.
- **Beam**: a continuous beam locked to the ship, thin at level one, a pillar at five; one
  entity stretched by scale with the uv repeating along it; a segment cast each tick
  finds what it hits (improvements note).
- **Storm**: homing bolts that seek the nearest enemy, more at a time with power.
- **Lance**: heavy, slow, piercing shots that pass through what they kill.

**Options**: up to two satellites that orbit or trail the ship (children through the scene
module), fire a copy of the weapon at lower power and block bullets. **The bomb**: clears
every bullet on screen into chips, damages everything, briefly makes the ship invulnerable;
two per egg, more from items.

**Enemies.** By plane and size:

- **Popcorn** (air): fighters in formations of five to twelve on scripted paths, a shot
  each, a point each, a spark to die in. A formation is one data record: a path, a count,
  a spacing, a pattern.
- **Gunships** (air): medium, two to four turrets as child parts with their own aim, a
  pattern each from the library, a weapon item on death.
- **Ground**: turrets (aimed shots, rings), tanks (move along the ground, fire while the
  ship is ahead), bunkers (spawn popcorn until destroyed), silos (a burst when overflown).
  They move with the scroll.
- **Carriers** (air, large): a hull that takes damage only at its parts, launches popcorn
  from bays, five or six turrets, a core that opens; the mid-boss shape.
- **Bosses**: one per stage, a tree of parts under a hull, each part a body with health;
  phases keyed on which parts live; patterns from the library plus one or two bespoke; a
  core that is the last thing; a timeout that makes it leave.

Every enemy is a row of one of a few kinds (`air`, `ground`, `part`, `boss`), differing by
data: a definition table (sprite, size, health, score, hitbox, patterns, drop) in JSON and
a behaviour from a small set (path follower, scroller, turret, orbiter), so a hundred
enemies on screen are a walk over columns.

**Bullets and patterns.** One kind, `bullet`, with a large capacity, every enemy bullet
in it: position, velocity, a sprite from the bullet sheet, a lifetime, a graze flag, an
owner. Bullets that leave the field die. Patterns are **BulletML** documents, read through the
`xml` module (a vendored, hardened, non-validating parser with documented edges; see
Decided) into a compiled program per pattern: `action`,
`fire`, `bullet`, `repeat`, `wait`, `vanish`, `changeDirection`, `changeSpeed`, `accel`,
references with parameters, and the expression language (`$1`, `$rank`, `$rand`, the
arithmetic). `$rank` is the density dial; `$rand` is the sim's stream. An emitter steps
its program each tick; every running action is a small frame in a flat array, so ten
thousand emitters are a `parallel_for`. The library is written here (the public BulletML
samples are checked for license before any is imported). The player's shots are a second
kind, `shot`, because their masks differ.

**Scoring.** Points per kill, graze, a chain multiplier that grows while kills come fast
and resets when they stop, cancels into chips, a stage bonus for eggs and bombs in hand, a
boss bonus scaled by how fast it died. High scores (ten, with name, stage reached and
restarts) persist through the save module; name entry is a UI field.

**Items.** Weapon items (four colors, cycling through the types while on screen), power,
option, bomb, egg, and the score chips cancels make; items fall to the player when the ship
is above a line (a magnet).

**Stages.** Two, each a stage script (spawns, speed changes, mid-boss and boss triggers,
keyed to scroll distance), a parallax set and music. (A third, Fortress, was cut in slice
5's PRD: a canyon and a base, bunkers, silos, walls that squeeze the field, a gunship pair
and a gate; a fair amount of content that asks nothing new of the engine.)

1. **Shoreline**: sea and islands as a Tiled ground, airfields; popcorn and turrets to learn
   on; a carrier as mid-boss, a battleship as boss. The slice-one stage and the demo's.
2. **Orbit**: no ground. The ground plane's place is taken by a battle: a fleet engagement
   half an orbit away, hundreds of tiny ships in two colors trading tracer fire, flashes,
   a capital ship breaking up over the course of the stage, all cosmetic rows on the
   deep planes, scrolling slowly. Under it, a procedural nebula: generated on the CPU at
   load (layered noise into a texture per plane, so the stage never looks the same twice
   but the demo's seed makes it look the same every run), uploaded once. A shader nebula
   is the stretch, when the renderer takes a game's fragment shader. Debris fields and
   station segments drift through the middle planes as the stage's "ground" enemies; the
   densest patterns; a station core as boss with the game's last three phases. It is the
   stress test in costume: the battle is a few thousand sprites doing nothing but being
   drawn.

**Modes.** Arcade (the stages in order, the egg rule), stage select (any stage, any difficulty),
and the dev modes: `--demo` (the scripted stage one), `--stress`, `--autoplay` (the bot,
dials unchanged). All report to the log at exit.

**Presentation.** Explosions as sheets at three sizes, debris and sparks as particles (a
cosmetic kind with velocity, drag, a lifetime and a fade), additive glows under bullets, a
white hit flash on anything hit, a shake on big deaths, slow time at a boss kill as at the
egg, beams with scrolling uv, a vignette and the side panels as UI. Enemy bullets are the
brightest things on screen and read against everything behind them; the stress mode is
where that is checked.

## Assets

**Style A, pixel.** Kenney's *Pixel Shmup* (CC0: ships, terrain tiles, UI pieces; 16 × 16,
drawn at an integer scale) for ships, ground and HUD frames; M484's *Bullet Collection 1*
and *2* (CC0) for enemy bullets; Screaming Brain Studios' *Seamless Space Backgrounds*
(CC0: tiling starfields and nebulas) for the deep planes of Shoreline; GGBotNet's
*Public Pixel* font (CC0) for scores and the HUD. Licenses confirmed against each archive's
own license file at download, every file taken recorded in `assets/CREDITS.md` with the
archive's hash, as arcanum's credits do.

**Generated art.** The pack's ships are small; carriers, bosses and Orbit's
capital ships are generated in the pack's style (image generation from the pack's sprites
as style reference, then cleaned to the palette and the grid by hand or script). These
live apart in `assets/generated/`, with their own section in the credits: the tool, the
date, the reference images, the prompt or the script, and the statement that they are
Korvid's and under the repository's license. Nothing generated is mixed into a vendored
directory, and nothing vendored is edited in place.

**Audio.** Kenney's *Sci-Fi Sounds* and *Impact Sounds* (CC0) for shots, hits and
explosions; Juhani Junkala's *5 Chiptunes (Action)* (SubspaceAudio; the page's license to
confirm, reported CC0) for the stage and boss music, or Kenney's *Music Loops* if not.

**Made here.** The ground maps (Tiled, from the pack's tiles, with a generator script as
arcanum has), the BulletML library, the enemy and stage tables, the UI documents, the
hitbox and graze-ring sprites, the egg and the shell, the beam strip, the particles, the
nebula generator, the vignette.

## Engine use, and what it will ask for

Kinds: `ship` (1), `option` (2, children), `shot` (2048), `bullet` (16,384; more in
stress), `air` (256), `ground` (256), `part` (512, children), `boss` (1), `item` (256),
`particle` (16,384, cosmetic), `plane` (the parallax tiles, cosmetic), `beam` (2),
`shadow` (256, cosmetic children), `fleet` (Orbit's battle, cosmetic), `egg` (1).
Systems: `scroll`, `control`, `patterns` (a `parallel_for` over emitters), `bullets`
(move, lifetime, bounds, `parallel_for`), `shots`, `enemies` (behaviours), `items`,
`particles` (`parallel_for`), `fleet`, `egg`, `camera`, `score`, `hud`. Handlers over
`OverlapBegin` sort pairs into `BulletHit`, `ShotHit`, `Graze`, `Pickup`, `ShellHit`.
Triggers: a part's health crossing zero, the boss's phase thresholds. Scheduled events for
the stage script, filed as the scroll crosses each mark. The UI module for every screen and
the HUD; the level module for stages (a stage is a level, its map the ground or nothing,
its scope the stage's assets); the save module for scores and options; the audio module
for everything heard.

Known at PRD time, each a line in the improvements note with its slice:

- **Time scale.** The egg and the boss kill want the sim at a quarter speed for a second:
  the tick clock accumulating scaled frame time, ticks still counted, so a scripted demo
  is unaffected. (slice 1)
- **The broadphase cell is the largest collider's diameter.** A boss hull puts every
  bullet in a few cells and the pair walk degenerates toward n² for bodies that, by mask,
  never meet. (slice 2)
- **Parallax.** Moving plane tiles is a Set delta per tile per tick for what is one
  offset per layer. Slice 1 measures the game-side version; slice 3 decides.
- **An `xml` module.** For BulletML here, and for Tiled's TMX later: a vendored small
  parser behind a Korvid tree API, present only where a game depends on it. (slice 2)
- **Kind capacity from outside**, and **the renderer's doubling growth** that remarks every
  row dirty. (slice 2)
- **Audio under load.** Voice priority and stealing, one start per sound per tick. (slice 3)
- **Beams.** A segment cast against bodies by mask. (slice 4)
- **Particles at render rate.** Measure first. (slice 5)
- **Input record and replay.** (slice 5)
- **A game's fragment shader.** For the shader nebula, a stretch. (slice 6)

## Slices and milestones

Each slice is a PRD of its own in this directory, with internal milestones; each leaves
the game building, running and hashing, and adds its observations to the notebook. The
stress work comes early so the engine findings are found in slice 2 and not slice 6.

1. **Skeleton** — the 3:4 playfield and side panels, the planes scrolling (game-side), the
   ship with Spread at one power level, popcorn formations from a stage script keyed to
   distance, one turret, score and eggs on a HUD, the egg sequence with the time scale,
   `--demo --hash` through a minute of play, the dials and `--stress` and `--autoplay` as
   flags that report. The first assets vendored with credits.
2. **Bullet hell** — the `xml` module, the BulletML compiler and runtime, the library's first
   ten patterns, bullets at scale, the hitbox and graze, the bomb, the broadphase and
   capacity work in the engine, items and the magnet, the first stress observations.
3. **Shoreline** — stage one complete: the ground map and ground enemies, shadows, the
   carrier and the battleship as bosses built from parts, phases on triggers, the stage
   script in full, music and sound with the audio work, the first generated ships, the
   demo plays it to the boss.
4. **Arsenal** — four weapons at five levels, options, the beam and its cast, the chain,
   extends, arcade mode and stage select, the difficulty positions of the dials; the bot
   clears stage one on Normal.
5. **Presentation** — title, pause, options, results, game over, name entry and high
   scores through the UI module and the save module; explosions, particles (and the
   render-rate question), flashes, shake; the record-and-replay question.
6. **Orbit and the battle** — the second and last stage, the fleet engagement, the
   nebula generator (and the shader stretch), the remaining patterns, the notebook's
   closing entry, a README that tells someone else how to watch it work. (Fortress, once
   stage two, is cut.)

## Decided

- Style A; larger ships generated in its style, kept in `assets/generated/`.
- 3:4 playfield, 540 × 720.
- Respawn in place through the egg; three eggs to start; out of eggs restarts the stage.
- BulletML. The XML parser is an engine module of its own (`xml`), present only in a
  game that needs it. Slice 2 looks first for a small, hardened, stb-style parser with
  clear edges on what it does and does not support (yxml, MIT, 6 KiB, non-validating,
  skips the DTD, one root; hoxml, C99 header-only, the same shape) and vendors it as
  `inflate` vendors miniz; one is written here only as a last resort.
- Orbit's background is a battle over a procedural nebula.
- Measurements go in `performance-notes.md` here, as a notebook by slice and milestone;
  the improvements note holds engine findings only.

## Open

- **Out of eggs, exactly.** Written above as: restart the stage, eggs refilled, score back
  to the stage's start, restarts counted on the results screen and in the high-score row.
  Does arcade mode end at all, or is every run finishable? (Suggested: every run is
  finishable; the restart count is the shame.)
- **BulletML samples.** Kenta Cho's sample documents are the obvious seed for the library.
  Their license has to be read first; if it does not allow it, the library is ours alone.
- **Time scale and the demo.** A scripted demo counts ticks, so scaled time changes
  nothing it hashes; but the script's *wall* duration grows with every death. Fine, unless
  `--run-seconds` is how the demo ends. (Suggested: the demo ends on an event, not a timer.)
