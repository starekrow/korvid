# Arcfire Slice 4 — Arsenal

Nov 9, 2026 · @starekrow

## Goal and exit criteria

Slice 3 finished Shoreline: its ground, its two bosses, its sound and its sky. The ship
flying over it is still slice 1's: Spread at its first level, two shots and no choices.
Slice 4 gives the player the arsenal the master PRD promises and the game the shape of a
game: four weapons at five power levels, switched and powered by items that enemies drop;
options that fly with the ship, fire with it and block bullets; the chain, extends and the
stage bonus; difficulty as positions of the dials; arcade mode and stage select. The bot
learns to use the arsenal (pick up, switch, bomb to live), as the demo and the dev modes
need; making it clear Shoreline, and the game's numbers in general, wait for a tuning pass
at the end of the slices, unless a number in the way now is a problem now.

The engine's part is small and named: the beam wants every body along a segment, nearest
first, each tick, which `collision_cast` (the first body only) does not give.

It is done when:

- The ship has four weapons, Spread, Beam, Storm and Lance, each at five power levels, as
  the tables below give them; switching is picking up a weapon item of another type,
  powering is picking up one of the same type or a power item.
- The Beam is a continuous beam locked to the ship, from a thin line at level one to a
  pillar at five: one sprite stretched along it with its uv repeating and scrolling, its
  length cut by a segment cast each tick at the first thing that stops it, damaging it.
- Options: up to two, children of the ship, orbiting it (holding beside it while focused),
  firing the weapon at a lower power, each a body that absorbs bullets. A death costs them.
- Items: weapon items that cycle through the four types while they fall, power, option,
  bomb, and egg items; dropped by marked enemies (the stage's and the map's item carriers,
  and boss parts), with the magnet as now.
- Scoring: the chain (a multiplier that grows while kills come fast and resets when they
  stop), extends at score thresholds and from egg items, the stage bonus for eggs and bombs
  in hand; the HUD shows the weapon, its power, the options and the chain.
- The dials have Easy, Normal and Hard positions; arcade mode (the stages in order) and
  stage select (one stage at a difficulty) exist as the game's flow, chosen by flags until
  slice 5 builds the title and its menus.
- The bot picks up items, switches to the weapon it wants, bombs to save itself and keeps a
  bomb for a stomp; the notebook records how it fares at Easy, Normal and Hard (no clear is
  required: that is the closing tuning pass's).
- `collision_cast_all` (every body a moving box meets, by layer, nearest first) is in the
  collision module with a test, and its cost at stress is in the notebook.
- The demo is re-scripted to show the arsenal: each weapon picked up and powered, the
  options, the beam on a boss; it hashes as the others do.
- The README, the credits and the notebook are current; validation is clean; the leak
  report is empty.

## Where slice 3 left it

- **The ship** (`ship.cc`): Spread at level one, two shots every six ticks from either side
  of the nose, 6 degrees out (2 focused), a damage of one each: 20 damage a second at
  best. `kMuzzle`, `kMuzzleUp` and the spread are in `arcfire.h`; the pilots aim by them.
- **Shots** (kind `shot`, 2,048): `Shot{damage, spent}`; a shot that hits is destroyed and
  hurts the first thing it met that tick.
- **Items** (kind `item`, 4,096): `ItemChip` (score, from cancelled bullets) and `ItemBomb`
  (from a formation shot down whole); they pop up and fall, and come to the ship above the
  magnet line or within reach of the graze ring.
- **The run** (`Run`): score, stage score, eggs (three), restarts, epoch, kills, grazes,
  deaths, bombs (two after a death, at most six), chips. No chain, no extend, no bonus.
- **The dials**: Normal, Stress and Demo positions; `invulnerable` 2 is the demo's (it
  dies once, then nothing hits).
- **Modes**: the game boots straight into Shoreline; `--demo`, `--autoplay`, `--stress` and
  `--bench NAME` are the frame loop's flags. Out of eggs restarts the stage. There is no
  title, menu or results screen (slice 5's).
- **The bot** (`pilot.cc`): one steering rule shared with the demo; it dodges each
  bullet's closest pass, keeps off air enemies and a boss's hull, lines up under the
  nearest enemy (beside a boss's lowest phase-ending part, by the spread), and bombs at 40
  close bullets (25 at a boss). At Normal, vulnerable, it dies three times an attempt
  between 1,500 and 2,400 ticks in and never reaches a boss; it has never bombed to save
  itself. At the bosses, invulnerable, it does about a third of a volley's damage.
- **Bosses**: health cut by a third in slice 3 so the rule could kill the carrier with
  Spread at level one; the battleship stomps and leaves in the demo.
- **Collision**: `collision_query` (bodies in a box) and `collision_cast` (the first body a
  moving box enters), each a walk over every collider of every kind, filtered by layer.

## Weapons

A weapon is a type and a power level (1 to 5). The ship has one at a time; the level is the
ship's, kept when the type changes (decided), and a death keeps both. Each type is a row
of a table in the game (`weapons.cc`), so the numbers below are a starting point for the
closing tuning pass.

**Spread** (the default): a fan of shots from the nose, more and wider with power, narrowed
when focused.

| level | shots | fan (degrees) | every (ticks) | damage |
|---|---|---|---|---|
| 1 | 2 | 6 | 6 | 1 |
| 2 | 3 | 10 | 6 | 1 |
| 3 | 4 | 14 | 5 | 1 |
| 4 | 5 | 18 | 5 | 1 |
| 5 | 7 | 24 | 5 | 1 |

**Beam**: continuous, from the nose straight up the field, locked to the ship. Its width
and its damage grow with power; it stops at the first body it meets on the enemy layers (a
boss's hull too: the beam is the one weapon the hull blocks, so it cannot reach a part set
back from the hull's edge without the ship moving under it) and damages that body every few
ticks. Drawn as one sprite whose height is the cast's length, its uv running past one and
scrolling (the renderer's sampler repeats), with a flare where it ends. Focused, it narrows
to half its width and does a third more damage.

| level | width (units) | damage | every (ticks) |
|---|---|---|---|
| 1 | 6 | 1 | 4 |
| 2 | 10 | 1 | 3 |
| 3 | 16 | 2 | 4 |
| 4 | 24 | 2 | 3 |
| 5 | 36 | 3 | 3 |

**Storm**: homing bolts that seek the nearest enemy (a part counts, an armored part does
not), turning at a limited rate, more at a time with power; weak each, never wasted.

| level | bolts a volley | every (ticks) | turn (degrees a tick) | damage |
|---|---|---|---|---|
| 1 | 1 | 8 | 6 | 1 |
| 2 | 2 | 8 | 7 | 1 |
| 3 | 2 | 6 | 8 | 1 |
| 4 | 3 | 6 | 9 | 1 |
| 5 | 4 | 6 | 10 | 1 |

**Lance**: a heavy, slow shot that pierces: it passes through what it kills and stops at
what it does not, so a line of popcorn falls to one lance and a turret takes its full
weight. Larger with power.

| level | size (units) | every (ticks) | speed | damage |
|---|---|---|---|---|
| 1 | 20 | 16 | 9 | 5 |
| 2 | 24 | 15 | 9 | 7 |
| 3 | 28 | 14 | 10 | 9 |
| 4 | 34 | 13 | 10 | 12 |
| 5 | 40 | 12 | 11 | 16 |

Shots stay one kind (`shot`), the type and the piercing in its component; the beam is a
kind of its own (`beam`, 3: the ship's and its options'), cosmetic for its sprite and sim
for its damage, which is dealt by the system that casts it.

## Options

Up to two, kind `option` (children of the ship: `Parent`, `Local2`, a collider). Unfocused
they orbit the ship (opposite each other, a turn every two seconds); focused they hold
beside it, left and right, so the player aims them. Each fires the ship's weapon at two
levels lower (at least one), on the ship's cadence; an option's beam is its own cast. Each
is a body on the shell's layer that eats the bullets it meets, as the egg's shell does. An
option item adds one (at two, it pays score instead). A death costs both; they do not drop.

## Items and where they come from

`ItemKind` grows: `ItemWeapon` (it shows a type, which cycles Spread, Beam, Storm, Lance
every 90 ticks while it falls; taking it takes the type it shows), `ItemPower`,
`ItemOption`, `ItemEgg`, beside `ItemChip` and `ItemBomb`. A weapon of the type the ship has
powers it; another type switches to it. Past level five, a power item pays score.

Shoreline has no gunships (slice 6's), so items come from **item carriers**: a formation
mark or a map enemy may carry a `drop` (`"drop": "weapon"`, `"power"`, `"option"`, `"egg"`),
given to the formation's last member shot down or to the ground row when it dies; a boss
part may carry one too (the carrier's bays drop a power item each; the battleship's main
guns an option). The stage script and the map place them by hand, so a run meets the same
items at the same places, and the demo can be written against them. A formation shot down
whole keeps dropping its bomb.

## Scoring

**The chain.** Every kill within `chain_window` ticks (90) of the last adds one to the
chain; the multiplier is a step of it (x1 below 10, x2 to 24, x3 to 49, x4 to 99, x5 from
100); a gap longer than the window resets it. A kill pays its score times the multiplier.
Grazes do not count, but each one holds the window open for 20 more ticks, so a player
weaving through a pattern keeps a chain alive between kills. The HUD shows the chain and
its multiplier, and the window running out as a bar.

**Extends.** An egg at 200,000 and 600,000 points, then every 1,000,000; and an egg item.
At most five eggs; an extend past five pays 50,000.

**The stage bonus.** At a stage's end: 20,000 for each egg in hand and 5,000 for each bomb,
added with the stage-clear banner and logged.

The numbers are a start: slice 3's demo scored 80,000 at x1; the chain and the stronger
weapons put a good Normal run near half a million, which the extends are placed for. The
notebook records a Normal run's score with and without the chain.

## Difficulty, modes

**Positions.** Easy, Normal and Hard are positions of the dials:

| dial | Easy | Normal | Hard |
|---|---|---|---|
| rank | 0.25 | 0.5 | 0.9 |
| bullet_speed | 0.85 | 1.0 | 1.15 |
| enemy_health | 0.8 | 1.0 | 1.3 |
| formation_count | 1.0 | 1.0 | 1.25 |
| chain_window | 110 | 90 | 75 |

**Arcade** plays the stages in order (Shoreline alone until slice 6) at a difficulty, the
run's score carried from stage to stage; **stage select** plays one stage at a difficulty.
Until slice 5's title screen, both are flags: `--arcade` (the default when the game is run
plainly), `--stage NAME`, `--difficulty easy|normal|hard`. Out of eggs restarts the stage
everywhere, as now (decided). A run's end logs a results line: the score, the stage
reached, the restarts, the eggs lost, the bombs used, the longest chain. Then (decided):
arcade shows the stage-clear banner and goes round again at the next difficulty up, the
score carried, and ends after Hard (a run started at Hard ends after it); `--stage` quits
after its one stage. The loops are arcade's second and third rounds, as arcades did.

## The bot uses the arsenal

Slice 3's rule, measured, has two holes: it never bombs to save itself, and it dodges more
than it shoots. The demo and the dev modes need a bot that uses what this slice adds; a
bot that clears the stage is the closing tuning pass's (decided). This slice's bot:

- **Bombs to live.** When a bullet's closest pass in the next few ticks is inside the
  hitbox's reach and the rule's push cannot clear it in time, it bombs (if it has one);
  the crowd rule stays for the rest.
- **Picks up.** It goes for items when the field allows (under the magnet line it lets them
  come), and prefers the weapon it is good with: Lance and Beam at bosses, Spread or Storm
  against popcorn (a weapon item is taken when its cycle shows the type the bot wants).
- **Shoots more.** It holds focus when lined up under a part with nothing close, and with
  the Beam it stays under its target, the beam doing the rest.
- **Knows the stomp.** At a boss's last ten seconds it keeps a bomb for the stomp.

The notebook records, at Easy, Normal and Hard: eggs lost and where, restarts, bombs used,
the boss times, the score.

## The demo

Re-scripted for the arsenal, keyed to ticks and to the items' places: Spread to level three
on the opening's items, the Beam at the islands, an option, Storm over the airfield, Lance
and both options for the battleship, a bomb at the carrier's core, the egg once (it keeps
`invulnerable` 2). It flies the bot's rule otherwise. It hashes across thread counts, I/O
backends and asset sources as now.

## The engine's part

- `collision`: `collision_cast_all(ctx, box, delta, layers, out)`: every collider on the
  layers that the moving box meets, with the fraction of the move at which it does, nearest
  first (ties by kind and slot). A test against a hand-placed scene. Measured at stress: the
  walk covers every collider of every kind, so at 50,000 bullets on layers the beam does not
  ask for, its cost is a read of each one's layer; if the notebook finds that worth a
  millisecond, the cast walks the broadphase's (layer, mask) groups instead.

Nothing else is expected. The sort's cost following the table's high-water rows (the GPU
note) is not this slice's.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Spread at five levels | P0 |
| R2 | The Beam: the cast, damage, the stretched scrolling sprite, focus | P0 |
| R3 | Storm: homing bolts | P0 |
| R4 | Lance: piercing shots | P0 |
| R5 | `collision_cast_all` with its test; its cost at stress | P0 |
| R6 | Options: orbit and hold, fire, block, lost at death | P0 |
| R7 | Items: weapon (cycling), power, option, egg; item carriers in the stage and the map; boss part drops | P0 |
| R8 | The chain | P0 |
| R9 | Extends and the stage bonus | P0 |
| R10 | The HUD: weapon, power, options, chain | P0 |
| R11 | Easy, Normal and Hard positions | P0 |
| R12 | Arcade and stage select by flags; the results line | P0 |
| R13 | The bot: bombs to live, picks up, shoots more, keeps a bomb for the stomp | P0 |
| R14 | The bot at Easy, Normal and Hard, recorded (no clear required) | P1 |
| R15 | The demo through the arsenal, hashing | P0 |
| R16 | Boss health or timeouts changed only where the arsenal breaks a fight (its phases never show, or it cannot be finished) | P2 |
| R17 | The notebook's slice 4 entries; README, credits | P0 |

## Milestones

Each leaves the game building, running and hashing.

1. **Weapons** (R1, R2, R3, R4, R5, R10 for the weapon): the four types at five levels,
   `collision_cast_all` and the beam, weapon and power items (dropped by hand-placed
   carriers in the stage file to try them), the HUD's weapon and power.
2. **Options and items** (R6, R7, R10): the options, the item kinds, item carriers in the
   stage and the map, boss part drops, death's cost.
3. **Scoring and modes** (R8, R9, R10, R11, R12): the chain, extends, the stage bonus, the
   difficulty positions, arcade and stage select, the results line.
4. **The bot and the demo** (R13, R14, R15, R16, R17): the bot's new rules and its record
   at the three difficulties, the demo re-scripted, the notebook, README and credits.

## Decided

- **A run's end, before there is a title:** arcade shows the stage-clear banner, logs the
  results line, and goes round again at the next difficulty up (score carried), ending
  after Hard; `--stage` quits after its one stage.
- **Out of eggs:** the stage restarts everywhere, arcade included, as the master PRD has it.
- **The bot and tuning:** no clear at Normal is required here. The whole game gets a tuning
  pass at the end of the slices; until then a number changes only when it is a problem
  now. The bot learns the arsenal for the demo and the dev modes, and its record at the
  three difficulties goes in the notebook.
- **Power:** the level is the ship's, kept when the type changes.
