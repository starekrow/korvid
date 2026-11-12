# Arcfire Slice 2 — Bullet hell

Nov 9, 2026 · @starekrow

## Goal and exit criteria

Slice 1 made the game exist; slice 2 makes it the game the master PRD names. Enemies stop
firing one aimed volley and start running BulletML: patterns as documents, read through a
new `xml` module, compiled once and stepped every tick by every emitter and every bullet
that has a program of its own. A library of ten patterns, written here, fills the stage.
The bullets go from dozens to tens of thousands, and the engine work that makes that
possible lands with them: the broadphase stops testing pairs that can never meet, the
bullet kind holds what the stress dial asks for, the renderer stops discovering its table
size by doubling. The ship gets its bomb, the bullets it cancels become score chips, and
items fall and are drawn in by the magnet. The notebook gets its first entries at a
density worth the name.

It is done when:

- An `xml` engine module exists, wrapping a vendored, small, non-validating parser behind
  a tree API in the shape of the JSON reader's, its edges stated in its README and pinned
  by tests, and a fuzz test that mutates documents and never crashes, leaks or reads past
  its input.
- BulletML documents (`assets/patterns/*.xml`) compile at load into programs, and a runner
  steps them: `action`, `fire`, `bullet`, `repeat`, `wait`, `vanish`, `changeDirection`,
  `changeSpeed`, `accel`, `bulletRef`/`actionRef`/`fireRef` with `param`, every
  `direction` and `speed` type, and the expression language (`$1`…, `$rand`, `$rank`,
  `+ - * / %`, parentheses). A document the compiler refuses says why, with the element.
- The library has ten patterns, each a document with a sentence of what it is for, and
  `--bench NAME` shows any one of them firing from a fixed emitter, looping.
- Every enemy in `shoreline.stage.json` fires a pattern by name; the slice-1 one-rule
  emitter is gone and the `air` and `ground` kinds have the same components as before.
- At `--stress` the field holds 20,000 bullets for a sustained stretch, and with
  `--dial rank=…` past 1 it reaches 60,000 without a kind filling up; the notebook has the
  tick, the broadphase, the runner, the extract and the upload at 1, 8 and 16 threads.
- The broadphase never tests a pair whose layers and masks cannot meet. Slice 1's heavy
  run (6,470 colliders, 1.65M pairs tested at peak, 7.3 ms worst) tests under 1% of that
  and its worst tick is under 1 ms; arcanum's and rocks' demos hash exactly as before.
- The bomb clears every bullet into chips, damages every enemy on the field, and shelters
  the ship; two per egg, shown on the HUD. Chips and the bomb item fall, are drawn in by
  the magnet above its line, and are picked up by the graze ring.
- `arcfire --demo --hash` flies the patterned stage, grazes, bombs once, dies once, picks
  up chips, quits on the end mark, and matches across runs, `--threads 1`, `8` and `16`,
  `--io sync` and `uring`, and pack, zip and loose files.
- The engine findings slice 1 recorded for this slice are fixed or carried with a reason;
  validation stays clean; the leak report is empty; the README and the credits are current.

## Where slice 1 left it

What this slice changes, as the code stands (`examples/arcfire/src`):

- **Emitters** are `Emitter{at, count, cadence, timer, fired, speed}` on the `air` and
  `ground` kinds; `enemies.cc` fires `bullets_fire_aimed` (a fan of `count × bullet_density`
  aimed bullets) once along an air row's path or every `cadence` ticks from a ground row.
  The stage file's formation records carry `fire_at` and `fire_count`, its ground records
  `cadence` and `bullet_speed`.
- **Bullets** are `Transform2, Sprite, Velocity, Collider2, Bullet{life, grazed, owner}`
  in a kind of 16,384; `bullets.cc` moves and ages them in a `parallel_for`.
- **The dials** have `bullet_density` (a count multiplier), `bullet_speed`, and the rest;
  there is no rank.
- **The broadphase** (`modules/collision/src/module.cc`) files every collider into one
  grid whose cell is the largest collider's diameter and tests every pair in neighbouring
  cells before looking at layers. Slice 1 added `CollisionStats::tested`.
- **Kind capacity** turned out to cost address space only: a table reserves `capacity`
  rows and commits pages as they are used, and the sprite extract's side tables follow a
  kind's high-water row count, not its capacity. The renderer's sprite tables start at
  4,096 rows and double, remarking every row dirty (seen once in slice 1, at ~2,700 live).
- **The bomb** action is bound (X, pad East) and does nothing. A death cancels nearby
  bullets into sparks. There are no items.
- **Findings** in `docs/design/arcfire-improvements.md` for this slice: the broadphase,
  kind capacity, the renderer's growth, the extract's cost at volume; and from slice 1's
  own work, events silently dropped for a stale handle and a quit that runs out its frame.

## The xml module

**The parser (R1).** An engine module `xml`, present only in an executable whose modules
depend on it, as `inflate` is. It vendors a small streaming parser in
`modules/xml/src/vendor/` behind a `.cc` wrapper (kormake compiles only `src/*.cc`), with
its license in the module's `licenses`. The master PRD names the candidates: yxml (MIT,
byte-fed, non-validating, skips the DTD, one root, predefined entities and character
references) and hoxml (C99, header-only, the same shape). M1 reads both, picks one for
its clarity and its edges, and writes down why in the module's README; one is written here
only if neither holds up.

**The tree (R2).** `xml_parse(text, len, alloc, XmlDoc&)` builds a flat tree in the
caller's allocator, the way `json_parse` does: nodes (element or text) with a name, the
first child, the next sibling, a run of attributes, and text unescaped and NUL-terminated;
errors name a byte offset and a line. Walking is `xml_child(doc, node, "name")`,
`xml_next_named`, `xml_attr(doc, node, "name")`, `xml_text(doc, node)` (the concatenated
character data of an element, trimmed), all null-safe. Nothing is written; tools write.

**The edges (R3).** Stated in the README and each pinned by a test: UTF-8 only (a BOM
skipped, anything else refused); the DTD and the internal subset skipped, never applied;
the five predefined entities and numeric character references, nothing else (an unknown
entity is an error, never an expansion); comments, processing instructions and the XML
declaration skipped; CDATA kept as text; namespaces not processed (`a:b` is a name);
one root; depth at most 64; attribute values unescaped, not normalized beyond that; no
size limit but the input's, no recursion in the walk. A fuzz test mutates the library's
documents and slice 1's stage file by bytes (deterministic seeds, a few thousand cases a
run) and requires a clean result or a clean error, no allocation left, every read inside
the input. The tilemap module's TMX reader is a later user; nothing here changes it.

## BulletML

**Where it lives (R4).** The compiler and the runner are a library module, `bulletml`
(no kinds, no systems, no engine state): it turns a document into a `BmlProgram` and
steps a `BmlRunner` given a host that answers where the runner is, where the target is,
the rank and a random number, and that takes `fire` and `vanish` as calls. The game owns
the kinds, the systems and the bullets. The format is a published specification that any
shooter on the engine would read the same way, and keeping it apart keeps its tests away
from the game's. Its limits (frame depth, modifier slots) are constants a game can read;
a second user changes them in one place.

**The compiler (R5).** At load: the document through `xml`, the labels resolved, the
references checked (an unknown label, a reference cycle or a nesting deeper than the
runner holds is a load error naming the element), the expressions compiled to a small
stack program (constants, `$n`, `$rand`, `$rank`, the four operators and `%`, unary
minus), each element to an op: `FIRE`, `WAIT`, `REPEAT`, `CALL` (an action, with its
parameters evaluated at the call), `CHANGE_DIRECTION`, `CHANGE_SPEED`, `ACCEL`, `VANISH`,
`END`. A bullet definition is its direction, speed and actions. The top-level actions
whose labels begin with `top` start together, as the reference says. Programs are
immutable after load and live in the game's module state; nothing in the sim refers to
them but by index and program counter, so the hash covers where every runner is, not what
the program says.

**Meaning (R6).** The reference's semantics, with these choices written down: units are
world units and ticks (a `speed` of 2 is 2 units a tick, a `wait` of 30 is 30 ticks), so
the library is written for a 540 × 720 field; directions are degrees in the reference's
convention for `type="vertical"`, which M2's first test pins against the reference's own
examples before anything else is written; `aim` is toward the ship's position as the tick
began; `sequence` is relative to the runner's last fire (for `changeDirection`, per tick);
`$rank` is the `rank` dial (below); `$rand` is uniform in [0, 1) from the runner's own
stream. `changeDirection`, `changeSpeed` and `accel` run beside the action for their
`term`, one slot each in the runner. A `bullet` with no direction aims; with no speed it
is 1.

**The runner (R7).** `Runner`, a component of every kind that runs a program: the
`air` and `ground` kinds, where it replaces the contents of `Emitter` (the component keeps
its name and becomes the binding: the program, where along the path it starts, and the
runner state), and the `bullet` kind, where a bullet fired with actions carries its own
and a plain bullet has program 0 and costs one test. Inline and fixed: a frame stack six
deep (program counter, repeat count left, wait left, the frame's parameters), the three
modifier slots, the last fire's direction and speed for `sequence`, its random stream
(PCG32 state, seeded from its emitter's stream at the fire). The notebook records its
size; the budget is 160 bytes.

**Stepping (R8).** A system `patterns` (Sim, after `control` and the movers, before
`bullets`) steps every runner of every kind in a `parallel_for`. A fire spawns by the
chunk's command buffer, so bullets appear in chunk order whatever ran where, and a
fired bullet's runner is initialized in the spawn (its program, a stream drawn from the
firer's), so nothing needs a handle the chunk cannot have. `vanish` destroys the runner's
own row. A bullet's direction and speed live in `Bullet`, which grows `dir` and `speed`;
the runner writes `Velocity` from them when they change, and `bullets` keeps integrating
`Velocity` alone. Nothing in a step reads another runner.

**Determinism (R9).** Every source of difference is closed: the runner's stream is its
own, expressions evaluate in the same order on every thread, spawns merge in chunk order,
the target is the ship as the tick began (an early read). The demo hashes the same at 1,
8 and 16 threads; a test steps a program through a hand-written host and compares the
fires against a trace computed by hand for `repeat`, `wait`, `sequence` and `$rand` with
a fixed seed.

## The library

**Ten patterns (R10).** Written here, one document each, under `assets/patterns/`, with an
index `patterns.json` naming them; each has a header comment saying what it is for and
which stage uses it, and is shown in the bench. Rank scales counts and speeds; at rank 0
each is sparse enough to read, at 1 it is the Normal density, past 1 it keeps growing
(the stress dial's room).

| name | what |
|---|---|
| `aimed` | a 3-way aimed shot, `$rank` adds ways: popcorn's default |
| `aimed_burst` | five aimed shots a few ticks apart, then a pause, repeated |
| `ring` | a ring of bullets around the emitter, the count from `$rank` |
| `spiral` | a turning stream (`sequence` direction), two arms |
| `rain` | a slow fall of bullets spread by `$rand` across a cone |
| `needles` | slow needles that accelerate after a beat (`changeSpeed`) |
| `wave` | a fan whose bullets sway (`changeDirection`, `sequence`, repeated) |
| `split` | bullets that travel, stop, and burst into small rings (active bullets) |
| `cross` | a rotating cross of four streams with a turn per volley |
| `homing` | slow bullets that turn toward the ship for a while (`changeDirection` aim, `term`) |

**Kenta Cho's samples.** Read for reference, not copied, until their license is read from
the source (open question 1); if it allows, the credits list what was adapted.

## The stage, the dials and the bench

**The stage file (R11).** A formation record names `pattern` (and keeps `fire_at`, now
where along the path the emitter's program starts); a ground record names `pattern` (its
program runs while the ship is above it, as the turret did, and restarts when it comes
back). `fire_count`, `cadence` and `bullet_speed` go. Shoreline is rewritten across the
library: popcorn on `aimed` and `aimed_burst`, the turrets on `ring` and `needles`, a
mid-stage stretch of `spiral` and `wave` emitters, `split` and `homing` near the end. The
stage stays a minute and keeps its two speed changes.

**Rank (R12).** A dial `rank` (Normal 0.5, Demo 0.5, Stress 1.0, any value by `--dial`)
is `$rank`. `bullet_density` goes; `bullet_speed` stays as a multiplier on every fire's
speed. Stress keeps its formation count and spacing and adds rank; the measurement entries
name their rank.

**The bench (R13).** `--bench NAME` runs a stage of its own (`bench.stage.json`): one
emitter at the top of the field running `NAME` every two seconds, the ship invulnerable at
the bottom under the bot, the HUD showing the pattern's name, its live bullets and its
runner count. It is how the library is written and how a pattern's cost is measured alone.

## Bullets at scale

**Capacity (R14).** The bullet kind holds 65,536 and the particle kind 32,768, since
capacity is address space (a reserve, committed by use). `--capacity kind=N` (a frame-loop
option, repeatable, applied at registration through `EngineConfig::capacities`) sets any
kind's for a measurement past that; a kind that fills logs once and counts what it
refused. Tables that grow are not built: nothing measured asks for them.

**The broadphase (R15).** `collision.broadphase` files colliders by their (layer, mask)
signature into groups, gives each group its own grid (its cell the group's largest
diameter), and tests a pair of groups only if their masks meet, and a group against
itself only if it meets itself: bullets, which never meet bullets, are never paired with
each other. Across two groups it walks the smaller one and looks up the larger one's grid
over each item's box, so a few bodies against 60,000 bullets is a few lookups. A large
collider is in a group of its own and costs its own lookups, not everyone's cell size (the
boss hull of slice 3). The pairs it reports are the same set, in the same canonical order,
as before: a test compares it against a brute-force pass on random scenes of mixed sizes
and masks, and arcanum's and rocks' demos hash exactly as they did. `CollisionStats`
gains the group count.

**The renderer's tables (R16).** The renderer sizes its sprite tables from a hint at
startup, `FrameLoopConfig::sprite_rows` (arcfire asks for 98,304), and grows past it as
before; a growth logs the frame it took. The notebook has the growth's cost at 16k → 32k
rows from a run started with a small hint, so the hint's worth is a number.

**The extract (R17).** Measured, not changed, unless the measurement says so: the
extract's compare and the upload at 20,000 and 60,000 moving rows go in the notebook with
the tick's share of each. If the extract passes a third of the tick at 20,000, the note's
"Many Sets per tick" item gets a design for slice 3.

## The bomb, chips and items

**The bomb (R18).** `Run` gains `bombs` (2 at the start and after every death, at most
6). Pressing the bomb with one in hand and the ship live: every bullet on the field
becomes a chip (below), every enemy on the field takes 20 damage (a kill pays as usual),
the ship is invulnerable for 120 ticks (the hitbox's mask is 0, as under the shell; the
ring keeps grazing and picking up), the screen flashes white for a few ticks (a UI cover with no hold) and
the camera shakes. The HUD shows bombs as pips beside the eggs.

**Chips (R19).** A cancelled bullet (by the bomb, or near a death as in slice 1) becomes a
chip: kind `item` (4,096: `Transform2, Sprite, Velocity, Collider2, Item{kind, value,
life}`), small, worth 10, drifting up a moment and then falling. A cancel of more than
256 bullets makes 256 chips and pays the rest's worth at once, so 60,000 bullets never
become 60,000 rows; the spark count per cancel is capped the same way.

**Items and the magnet (R20).** Items fall at a speed that slows near the bottom and die
off the field. While the ship is above the magnet line (`kMagnetY`, 120 units under the
field's top, in `field.h`), every item on the field homes on it; any item within 40 units
of the ship homes anyway. The graze ring meets `item` and a `Pickup` handler pays it.
Slice 2's items: chips, and the bomb item, which the last member of a formation drops when
the whole formation is shot down (a `Formation` record counted by members killed and by
members gone, keyed by the formation's spawn, so a formation that leaves the field drops
nothing). Weapon, power, option and egg items come with what they do (slice 4).

## Running itself, reporting

**The demo (R21).** Re-scripted on the patterned stage: fire throughout, focus through a
`wave` to graze, bomb once inside a `spiral` (a known tick), stand in a stream once (the
egg), collect the chips the bomb made, and quit on the end mark, holding as it quits. The
bot learns to bomb when more than 40 bullets close within 80 units with a bomb in hand,
which `--autoplay` uses and `--stress` does not (the ship cannot die there).

**Counters (R22).** `example_arcfire.stats` gains live runners, bullets with programs,
fires this tick, items, chips made and refused, broadphase groups; the report line and the
summary carry them, and the dev panel shows runners beside bullets.

**The notebook (R23).** Entries, each with its dials and thread counts: the bench, each
pattern alone at rank 1 (bullets at peak, runner time a tick); Normal under the bot for
the minute; `--stress` at rank 1, 2 and 4 (bullets sustained and at peak; tick, runner,
broadphase before and after R15, extract, upload, at 1, 8 and 16 threads); the renderer's
growth with and without the hint; the size of `Runner`.

## The engine's part

What the slice adds or changes in the engine, in one list:

- `xml`: a new module (R1–R3).
- `bulletml`: a new library module (R4–R9).
- `collision`: the grouped broadphase (R15), the group count in `CollisionStats`.
- `core`: `EngineConfig::capacities` and the kind-full count (R14).
- `frameloop`: `--capacity kind=N`, `FrameLoopConfig::sprite_rows` (R14, R16).
- `renderer-vulkan`: tables sized from the hint, growth logged (R16).
- From slice 1's findings (R24): in dev builds the bus warns once per event type when it
  drops an event for a stale handle, naming the field; and `engine_frame` runs no more
  ticks once a quit is asked for, so a game that quits on an event needs no hold (the
  demo's hold is removed). The children's one-tick lag in Sim is slice 3's, with the boss
  parts it would hurt.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | `xml` module: a vendored parser, chosen and justified | P0 |
| R2 | The tree API and its errors | P0 |
| R3 | The edges, stated and tested; the fuzz test | P0 |
| R4 | `bulletml` as a library module with a host interface | P0 |
| R5 | The compiler: labels, references, expressions, ops, load errors | P0 |
| R6 | Semantics: units, the direction convention pinned, sequence, modifiers | P0 |
| R7 | `Runner` in `air`, `ground` and `bullet`; `Emitter` as the binding | P0 |
| R8 | The `patterns` system: parallel steps, fires by chunk, vanish | P0 |
| R9 | Determinism across threads; the traced runner test | P0 |
| R10 | The library of ten patterns | P0 |
| R11 | The stage file on patterns; Shoreline rewritten | P0 |
| R12 | The `rank` dial; `bullet_density` retired | P0 |
| R13 | `--bench NAME` | P0 |
| R14 | Capacities raised; `--capacity`; a full kind counted | P0 |
| R15 | The grouped broadphase, same pairs, same hashes | P0 |
| R16 | The renderer's sprite tables from a hint; growth measured | P1 |
| R17 | The extract and the upload measured at 20k and 60k | P1 |
| R18 | The bomb | P0 |
| R19 | Chips from cancels, capped | P0 |
| R20 | Items, the magnet, pickups, the formation's bomb drop | P0 |
| R21 | The demo re-scripted; the bot's bomb; `--demo --hash` at 1, 8, 16 threads | P0 |
| R22 | The new counters | P0 |
| R23 | The notebook's slice 2 entries | P0 |
| R24 | Stale-event warnings; a quit stops the frame's ticks | P1 |

## Milestones

Each leaves the game building, running and hashing.

1. **xml** (R1, R2, R3): the module, the vendored parser, the tree, the edges and the
   fuzz test; nothing in the game uses it yet.
2. **BulletML** (R4, R5, R6, R9, R13): the library module, the compiler and the runner
   against a test host, the direction convention pinned first, the traced test; the
   game's `Runner`, the `patterns` system and the bench with the first two patterns. The
   stage still fires the old way.
3. **Patterns everywhere** (R7, R8, R10, R11, R12): the remaining eight patterns, the
   stage file on patterns, `Emitter` as the binding, bullets with programs, rank; the
   one-rule emitter and `bullets_fire_aimed` gone; the demo re-hashed.
4. **Scale** (R14, R15, R16, R17, R22, R24): capacities, `--capacity`, the grouped
   broadphase with its test and the other demos' hashes, the renderer's hint, the counters,
   the engine findings; the stress entries in the notebook at 1, 8 and 16 threads.
5. **Bomb and items** (R18, R19, R20, R21, R23): the bomb, chips, items, the magnet, the
   formation's drop, the demo and the bot's bomb, the remaining notebook entries, the
   README and the credits.

## Open

1. **Kenta Cho's sample documents.** Their license is read from the source they ship in
   (the BulletML distribution and the games that carry them) before anything is adapted.
   Until then the library is ours alone, which this PRD assumes.
2. **The direction convention.** Pinned by M2's first test against the reference's
   examples; if the reference and libbulletml disagree, which one the library follows
   (suggested: the reference, with a sentence in the module README).
3. **How far past rank 1.** The stress dial wants the field full, the patterns want to stay
   legible at their design rank. (Suggested: patterns are written so rank past 1 adds
   bullets without changing shape, and the stress entries name rank 1, 2 and 4.)

## Decided

- BulletML is an engine library module, `bulletml` (R4): compiler and runner, no kinds;
  the game owns everything that lives in the sim.
- (M2, in the building.) A runner is 288 bytes, so it is not a column of every bullet:
  bullets that run programs are a kind of their own, `active` (the `bullet` kind's
  components and a `Runner`), and plain bullets stay as small as they were. The enemies
  gain a `Runner` beside `Emitter`, which keeps its name as the binding (which pattern,
  where it starts, its state). R7's "same components" is that much different. A runner's
  seed is its entity and the tick, not the world's stream, so runners need nothing shared.
- (M4, measured.) The library at rank 1 is a few thousand bullets at the densest; twenty
  thousand sustained needs rank 24 (the cross turret's stretch holds 20,000 to 36,000 for
  seven seconds at rank 32). R12's Stress position is rank 24, not 1.
