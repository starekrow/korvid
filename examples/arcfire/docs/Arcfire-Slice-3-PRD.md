# Arcfire Slice 3 — Shoreline

Nov 9, 2026 · @starekrow

## Goal and exit criteria

Slices 1 and 2 built the machine: a field, a ship, patterns, bullets by the tens of
thousands, the bomb. Slice 3 builds the first whole stage on it. Shoreline gets its ground
(sea and islands as a Tiled map, scrolling under the field), the ground enemies that sit on
it, shadows under everything that flies, two bosses built from parts (a carrier at the
middle of the stage, a battleship at its end) whose phases turn on which parts still live,
the stage script at its full length, and sound: shots, hits, explosions, the bomb, the egg,
and music that changes for the bosses. The demo plays the stage through both bosses.

The engine's part follows from that. A map that scrolls is a few thousand tiles, and moving
each one every tick is the cost the parallax note predicted, so the renderer learns to
scroll a layer. A boss is a tree of children with colliders, and children placed a tick
late would be hit where they were, so the hierarchy is composed before collision. The
audio module meets a shooter's volume of sound for the first time and gets priorities. And
the extract's worst windows, deferred from slice 2, are looked into.

It is done when:

- Shoreline's ground is a Tiled map made by a generator script from Kenney's terrain
  tiles (sea, islands, beaches, an airfield), drawn under the field and scrolling with the
  stage at the ground's rate, at a cost per tick that does not depend on the map's size: the
  notebook has the Set deltas a tick for the game-side version and the renderer-side one.
- Ground enemies come from the map's object layer (turrets, tanks that drive along roads,
  bunkers that launch popcorn), spawned as their row comes into view and riding the map.
- Every air enemy and every boss part that flies has a shadow on the ground plane, offset
  by its altitude, a cosmetic child.
- The carrier and the battleship are built from definition files (`assets/bosses/*.boss.json`):
  a hull and a tree of parts, each part a body with health, a sprite, a pattern and the
  phase it belongs to. The hull takes no damage; parts do. Phases advance when the parts
  they name are destroyed (state triggers on the parts' health); the last phase's core is
  the last thing standing. A boss that outlives its timeout stomps the player (its finale,
  meant to cost an egg) and then leaves. A boss kill slows time as a death does, scores a
  bonus scaled by how fast it died, and lets the stage go on.
- The stage script is Shoreline at full length (about three minutes at Normal), with the
  carrier at its middle and the battleship at its end; the stage's clock waits at a boss
  mark until the boss is gone.
- The boss ships are made here in the pack's style, kept in `assets/generated/` with the
  credits section the master PRD asks for (open question 1 decides how).
- Sound: CC0 effects and music vendored with their licenses; the game plays them through
  the audio module, and the module has voice priorities, stealing, and a limit of one start
  per sound a tick, so the stress dial cannot drown it; the notebook has voices, steals and
  drops at stress.
- A child with a collider is where its parent is this tick when the broadphase runs; a test
  shows a part hit on the tick its boss moved into the shot.
- The extract's worst windows at stress are explained and, if the cause is the one the
  improvements note guesses, gone; the notebook has before and after.
- `arcfire --demo --hash` flies the full stage through both bosses and matches across runs,
  `--threads 1`, `8` and `16`, `--io sync` and `uring`, and pack, zip and loose files.
- The README and the credits are current; validation is clean; the leak report is empty.

## Where slice 2 left it

What this slice builds on, as the code stands:

- **Planes** (`planes.cc`): three cosmetic grids of space backgrounds moved every tick by
  the game, a Set delta per tile per tick. Fine at six tiles; the slice-1 notebook measured
  1,259 deltas a tick at 1,224 tiles.
- **Ground rows** (`enemies.cc`, kind `ground`): spawned by stage marks at the field's top,
  moved down by the scroll's speed every tick, running their pattern while the ship is
  above. There is no ground to stand on; the turret sprite floats over space.
- **Children** (`scene` module): `Parent` and `Local2` composed in PostSim
  (`scene.compose`). The graze ring and the shell are children with colliders and are
  placed a tick late (the improvements note's "children lag" item).
- **Triggers** (core): `TriggerDesc` on a field of a component that keeps a previous copy,
  firing `StateEdge` at the end of the tick. Unused by arcfire so far.
- **Patterns**: ten in the library, `Emitter` and `Runner` on any kind, the `patterns`
  system stepping every runner of every kind.
- **Sound**: none. The audio module (slice 4 of the engine) has 64 voices, buses,
  `pause_with_sim`, streams and `music_play` crossfades; no priority, no stealing, and a
  full voice pool or command ring drops what does not fit (`AudioStats::dropped`).
- **The extract**: 0.31 ms a frame on average at stress, 4.5 ms in its worst window (8.2
  ms at 59,000 bullets); the guess is its side tables growing by copy as a kind's high-water
  climbs.
- **The tilemap module**: Tiled JSON maps (`tilemap_load`, `tilemap_spawn` into the cosmetic
  `tilemap.tile` kind, object layers with properties, animated tiles); arcanum uses it with
  a fixed camera over a map that does not move.

## The ground

**Layer scroll (R1).** The renderer gains a per-layer offset: `sprite_layer_scroll(e, layer,
offset)` (a small table the sprite module publishes, a few entries), extracted like a
sprite's state so a frame interpolates it between ticks, and added to the position of every
sprite drawn on that layer in the vertex shader. A plane, or a map, is then still sprites
that never change and one offset a tick. The game's planes move to it (their tiles stop
being written), and the ground map uses it. What it is not: a camera per layer (no zoom,
no rotation), or anything the sim reads; a sprite's position stays its world position, so
collision and the game never see the offset. The improvements note's parallax item closes
with this, and the notebook has the deltas a tick before and after at the three plane
densities slice 1 measured, and for the map.

**The map (R2).** `tools/make_maps.py` writes `assets/maps/shoreline.tmj` and its tileset
from Kenney's terrain tiles, as arcanum's script does: open sea with islands of grass and
sand, beaches, a few roads, an airfield near the carrier's mark, the battleship's open
water at the end. 17 columns of 32-unit tiles (the field's width and a margin), as many
rows as the stage is long. It loads through the tilemap module into the stage's scope and
is spawned once, its tiles on the ground layer; the ground's offset each tick is the
stage's distance times the ground's rate, so the map moves exactly as the ground rows do.
A loop or a restart sets the offset back with the distance.

**Ground enemies from the map (R3).** The map's `enemies` object layer places the ground
rows: a point object with a `type` (`turret`, `tank`, `bunker`) and properties for its
pattern, health and score (and a tank's `route`, a bunker's formation). The stage spawns a
row when its map position comes within a margin of the field's top, at the world position
that map point has then, and from there it rides the scroll as ground rows do now. The
stage file keeps `ground` marks for rows not on a map (the bench, a test). Tanks drive a
short route relative to the map (a polyline in the object, followed by arc length with
`path.cc`), firing while the ship is ahead of them. Bunkers launch a formation of popcorn
from where they stand every so often until destroyed. Ground rows are on the ground layer
below the low cloud.

**Shadows (R4).** Kind `shadow` (cosmetic, 512: `Transform2`, `Local2`, `Parent`,
`Sprite`): every air row and every flying part gets one at spawn, the same sprite's uv in
black at a third of its alpha, offset down and right by its altitude (a constant per
kind, larger for bosses), on the ground plane's layer. It dies with its parent.

## Bosses

**Parts compose before collision (R5).** `scene` gains a Sim pass, `scene.compose_bodies`,
for the child kinds that have a `Collider2`: after every writer of `Transform2` and
`Local2` in Sim, before `collision.resolve`, it places those children by their parents as
`scene.compose` does in PostSim (which still does every other child, and these again for
anything a later Sim system moved). The graze ring and the shell stop lagging. A test: a
part on a boss that moves into a standing shot is hit on that tick.

**Kinds (R6).** `boss` (2: `Transform2`, `Sprite`, `Collider2`, `Boss`) and `part` (256:
`Transform2`, `Local2`, `Parent`, `Sprite`, `Collider2`, `Health`, `Emitter`, `Runner`,
`Part`). A part's `Parent` is its boss or another part (a turret on a bay door), its
`Local2` its place on the hull. `Boss`: which definition, its phase, its clock, its
timeout, its movement (a path, as a formation's, entered and then held, swaying). `Part`:
its index in the definition, its phase, whether it is a core, whether it is armored (on
the hull's layer, absorbing shots without damage until its phase).

**Definitions (R7).** `assets/bosses/carrier.boss.json` and `battleship.boss.json`: the
hull (sprite, size, collider boxes that absorb shots), the entry path and the hold, the
timeout, the parts (name, sprite, offset, collider, health, score, pattern, the phase it
fires in, armored until which phase, a parent part), and the phases (a name, which parts'
deaths end it, what the hull's movement does). Read at load like a stage; a definition
that names a pattern the library lacks, or a part that does not exist, is a load error.

**Phases (R8).** A trigger on `part.Health.hp` crossing below 1 fires a `StateEdge` for
each part destroyed; a handler marks it, emits a `PartDown` (score, an explosion, its
children's fate: a turret on a destroyed door goes with it), and when every part a phase
names is down, the boss advances: the next phase's parts unarmor and start their patterns,
the hull's movement changes. A core is the last phase's part; its death is the boss's.

**The carrier (R9).** The mid-boss. A long hull that enters from the top and holds near it,
swaying: four turrets (`ring` and `aimed_burst`), two bays that open and launch popcorn
formations (a bespoke pattern fires no bullets; the bay's emitter schedules members like a
formation mark), then, when the turrets are down, the core opens (`spiral` and a bespoke
`fan_storm`). Timeout 45 seconds.

**The battleship (R10).** The stage's boss. A broad ship that fills most of the field's
width, entering slowly: two main guns (a bespoke `broadside`: walls of slow bullets with
gaps), four anti-air mounts (`aimed_burst`, `needles`), a missile bay (`homing`); when
the main guns are down the deck opens (`cross`, `wave`); then the bridge, the core, alone
with a bespoke `core_burst` (rings and spirals together). Timeout 75 seconds.

**Kill, or the stomp (R11).** A boss's core destroyed: time slows to a quarter for a second
as at a death, the parts that remain burst in turn, the hull breaks in pieces (particles cut
from its sprite, as the ship's quarters are), every bullet becomes a chip, the score takes
the boss's bonus (its base times the timeout's fraction left). A boss whose timeout runs
out does not go quietly: it stomps. The warning sounds, the screen tints, every part still
standing switches to the boss's `stomp` pattern (bespoke for each: the carrier launches
every bay at once and its turrets go to a closing ring; the battleship's guns walk a wall of
bullets down the field with no lane in it) and the hull itself comes down the field and
back, a body the ship must not touch, for about five seconds. It is meant to cost an egg: a
bomb held then is the way out (the stomp's bullets cancel like any others, the hull's body
does not), and the shell of a ship just hatched protects as it always does. Then it leaves
upward, paying nothing for the parts still standing, and the stage's cursor moves on. The
bosses' timeouts and health are tuned so that a player paying attention kills them with
time to spare (the notebook records the bot's times at Normal); the stomp is for the one
who did not.

**The stage and the bosses (R12).** Stage marks gain `boss {name}`: the boss spawns, and
the stage's cursor waits at the mark until the boss has died or left; the scroll keeps
moving (the map and the planes) at the speed the stage last set, which the script slows
before each boss. The end mark comes after the battleship. Shoreline's script is written
out at full length: the popcorn and turret opening of slices 1 and 2, the islands with tanks
and bunkers, the carrier, a second half over the airfield and open water with denser
patterns, the battleship.

**The HUD (R13).** A boss's health is a bar across the top of the field (the UI's bar
meter): the sum of its living parts' health over their total, with the boss's name, shown
while a boss is up.

## Art

**The bosses' ships (R14).** The pack's ships are 32 pixels; a carrier and a battleship
are a few hundred. They are made here in the pack's style, kept in `assets/generated/`
with their own credits section (the method, the date, the sources or references, and the
statement that they are Korvid's under the repository's license), never mixed into a
vendored directory. Each is a hull image and a sheet of its parts at the pack's pixel
scale and palette. How they are made is open question 1.

**Explosions (R15).** Kenney's pack has a few explosion frames (its tiles); a part's death
and a boss's kill use them as a flipbook (an anim clip from a sheet), with the particle
bursts slice 1 has. Slice 5 does presentation properly.

## Sound

**Assets (R16).** Kenney's *Sci-Fi Sounds* and *Impact Sounds* (CC0) for shots, hits,
explosions, the bomb, the egg and pickups; for music, Juhani Junkala's *5 Chiptunes
(Action)* if its license reads CC0 from its own page (the master PRD has it as reported),
Kenney's *Music Loops* otherwise: a stage track and a boss track. Each downloaded with your
OK at the milestone, its license read from the archive, the files taken recorded in the
credits with the archive's hash.

**The audio module under load (R17).** Three changes, each with a test in the mixer's own
test (the mixer renders offline): a play carries a priority (0 to 3); with every voice busy,
a play steals the voice of the lowest priority that is oldest (or is dropped if every voice
is of a higher priority), and `AudioStats` counts steals; and a sound started more than once
in one tick starts once (the louder request wins), so a ring of a hundred bullets is one
sound. Voices started with `pause_with_sim` follow the engine's time scale in pitch (open
question 2).

**What plays (R18).** Shots at a low priority with a little pitch spread; enemy hits and
kills by size; part and boss deaths at the top priority; the graze a soft tick, limited
per tick; the bomb; the egg's break, fall and bloom; pickups; a warning before each boss.
Music through `music_play`: the stage track, crossfading to the boss track at a boss mark
and back after. The notebook: voices live, steals, starts merged and drops at Normal and at
stress.

## Performance

**The extract's worst windows (R19).** First measured with the profiler's frame keeping
(`--hitch-ms` on the windows that spike) to see what the extract does in them; if it is the
side tables growing (`sent`, `sent_sprites`, `sent_placements` resized as a kind's
high-water climbs), they are sized once from the kind's capacity (address space, as the
tables are), and the notebook has the worst windows before and after at stress and at rank
56. If it is something else, it is written up and fixed if small, or carried with a design.

**The rest (R20).** The notebook's slice 3 entries: the ground and planes before and after
the layer scroll; a boss fight's cost (parts, runners, the hull's collider in the grouped
broadphase); the audio counts; the extract. Threading is not this slice's business unless
an entry finds a serial cost that a boss makes large.

## Running itself

**The demo (R21).** Re-scripted for the full stage, keyed to ticks as now: the opening as in
slice 2, a bomb in the carrier's core phase, the egg once, the battleship fought until it
dies (the demo flies the bot's rule against it and aims at its parts), the end mark. It
hashes as the others do. If the battleship's fight under the bot's rule runs past its
timeout, the demo is scripted to let it leave; the stage's end is the same.

**The bot (R22).** It aims at the lowest part it can reach and keeps under it, dodging as
now; at a boss it bombs earlier (more than 25 bullets close). Clearing the stage at Normal is
slice 4's goal; here it is measured (how often it dies, where).

## The engine's part

- `sprite` and `renderer-vulkan`: layer scroll (R1).
- `scene`: `scene.compose_bodies` (R5).
- `audio`: priorities, stealing, one start a sound a tick, time scale on `pause_with_sim`
  voices (R17).
- `sprite`: the extract's side tables (R19), if that is the cause.
- `tilemap`: whatever a scrolling map and an object layer of enemies ask that it lacks
  (expected: nothing; the map is spawned once on a layer that scrolls).

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Layer scroll in the renderer; planes and the ground on it; measured | P0 |
| R2 | The Shoreline map, generated, loaded, scrolling with the stage | P0 |
| R3 | Ground enemies from the map: turrets, tanks on routes, bunkers | P0 |
| R4 | Shadows | P1 |
| R5 | Children with colliders composed before collision; the test | P0 |
| R6 | `boss` and `part` kinds | P0 |
| R7 | Boss definition files | P0 |
| R8 | Phases on part triggers; cores | P0 |
| R9 | The carrier | P0 |
| R10 | The battleship | P0 |
| R11 | A boss's kill; the stomp at its timeout, then its leaving | P0 |
| R12 | Boss marks; the stage at full length | P0 |
| R13 | The boss health bar | P1 |
| R14 | The bosses' art, made here, credited apart | P0 |
| R15 | Explosions from the pack | P1 |
| R16 | Sound and music assets, vendored and credited | P0 |
| R17 | Audio priorities, stealing, one start a tick; time scale | P0 |
| R18 | The game's sounds and music | P0 |
| R19 | The extract's worst windows | P0 |
| R20 | The notebook's slice 3 entries | P0 |
| R21 | The demo through both bosses, hashing | P0 |
| R22 | The bot at bosses, measured | P1 |

## Milestones

Each leaves the game building, running and hashing.

1. **Ground** (R1, R2, R3, R4): layer scroll in the engine and the planes on it, the map
   and its generator, ground enemies from the map, shadows; the before-and-after entry.
2. **Parts and the carrier** (R5, R6, R7, R8, R9, R13, R14 for the carrier): the Sim
   compose and its test, the kinds, the definition files, phases on triggers, the carrier
   and its art, the health bar, the boss mark.
3. **The battleship and the stage** (R10, R11, R12, R14, R15): the battleship and its art,
   kill and leave, explosions, the full script, the demo through both bosses.
4. **Sound** (R16, R17, R18): the downloads (asked for), the audio module's changes and
   tests, the game's sounds and music, the audio entries.
5. **The extract and the notebook** (R19, R20, R22): the extract's spikes, the bot at the
   bosses, the remaining entries, the README and the credits.

## Open (answered: see Decided)

1. **How the bosses' ships are made.** The master PRD says image generation with the
   pack's sprites as reference, cleaned to its palette and grid. There is no image
   generator in this environment. Choices: (a) a script here that kitbashes the pack's own
   pieces (hull plates from its terrain and ship tiles, its turrets and guns, mirrored,
   scaled, recolored to its palette) into large ships, deterministic and regenerable, kept
   in `assets/generated/` with the script as the provenance; (b) you generate them with an
   image tool and they are cleaned here by script to the palette and grid; (c) drawn by
   hand. (Suggested: (a) now, so the slice is not waiting on art, with the files shaped so
   (b) can replace them later without code changes.)
2. **Sound and slow time.** When the sim runs at a quarter, should the sounds it started
   (`pause_with_sim`) slow and drop in pitch with it? (Suggested: yes for effects, by the
   time scale, floored at half pitch; music never.)
3. **The music's license.** Juhani Junkala's chiptunes are reported CC0; if the page says
   otherwise (CC-BY is fine with credit), Kenney's *Music Loops* (CC0) instead. (Suggested:
   take whichever is CC0, credit regardless.)
4. **Ground enemies on the map or in the stage file.** Suggested above: the map, since the
   ground is where they sit and Tiled is where they are placed by eye; the stage file keeps
   `ground` marks for the bench and tests.

## Decided

- Boss art: a script here kitbashes the pack's own pieces into the bosses' ships, kept in
  `assets/generated/` with the script as their provenance, shaped so images made elsewhere
  can replace them without code changes.
- Sound and slow time: effects started with `pause_with_sim` follow the time scale in
  pitch, floored at half; music never does.
- Music: whichever of the two candidates reads CC0 from its own page, credited regardless.
- Ground enemies are placed on the map's object layer; the stage file keeps `ground` marks
  for the bench and tests.
- A boss's timeout stomps the player (R11): a finale meant to cost an egg, a held bomb the
  way out, then the boss leaves. The bosses are tuned so that it is rarely seen.
