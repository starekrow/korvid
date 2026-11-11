# Arcfire performance notes

A notebook. Observations and measurements as they are made, under slice and milestone
headers, in the order they happened; short notes where a number needs one. Measured in
`linux-dev` with `--prof-report` unless an entry says otherwise, on the development
machine, with the dials the entry names (`--stress`, a difficulty, or a one-off). Engine
findings that come out of an entry go to `docs/design/arcfire-improvements.md`; the entry
links to them by name.

What to write down when something is measured: the dials, the live counts the game
reports (sprites, bullets, particles, enemies, Set deltas, pairs tested), the tick time at
one thread and at all of them, the GPU frame time, the top zones, and one sentence on what
it means.

## Slice 1 — Skeleton

All entries 2026-11-09, `linux-dev`, `--prof-report`, a 1280 x 720 window at 60 Hz with
vsync, 16 hardware threads. "all" is the default thread count, "t1" `--threads 1`. Zone
times are the profiler's per-frame averages over the run's report windows (summed over
threads); a zone missing from an entry was not in the top ten. `render` is almost all
`acquire`, the wait for vsync: no entry here is near a frame's budget.

**M1. Pairs tested were not reported.** The collision module had no count of the pairs its
broadphase looks at, only the overlaps it found. Added `CollisionStats::colliders`,
`tested` (candidate pairs in neighbouring cells, each once, before layers and masks) and
`cell` to `collision_stats` so this notebook can say what the grid does with bullets that
by mask never meet one another (the improvements note's broadphase item).

**M5. Normal under the bot** (`--autoplay --run-seconds 62`; the stage is 3249 ticks, then
loops). 57 kills, 2 grazes, one death (the egg; tick 2397), no restart. Bullets peak 16,
mean 2; sprites peak 241, mean 196; Set deltas peak 207, mean 41 a tick; pairs tested peak
130, mean 12; 37 colliders at most. Tick 0.45 ms a frame (all), 0.31 ms (t1); the extract
0.04 ms. 442k sprite rows uploaded over 3724 frames (119 a frame, 13 KB at 112 bytes a
row). One sentence: at slice 1's density the game costs nothing; the frame is vsync.

**M5. Stress** (`--stress --run-seconds 62`: bullets x6, formations x3 at half the
spacing, enemy health x2, sparks x4, planes in 128-unit tiles, invulnerable, looping).
Bullets peak 270, mean 62, at most 18 spawned in a tick; sprites peak 671, mean 384; Set
deltas peak 512, mean 225; colliders peak 299; **pairs tested peak 4,644, mean 583**, for
a peak of a few dozen real overlaps. Tick 0.56 ms (all), 0.43 ms (t1); extract 0.08 ms;
broadphase 0.05 ms, 0.35 ms worst. 1.47M rows uploaded over 3721 frames (395 a frame, 43
KB). The stress dials of slice 1 are not yet a stress: one volley per popcorn is the
pattern library's job (slice 2). Threads buy nothing at this size (t1 is faster: the
pool's fan-out costs more than the work).

**M1. The planes' cost at three tile scales** (`--autoplay --run-seconds 20 --dial
plane_density=N`; three planes; everything else Normal).

| tiles | plane sprites | Set deltas a tick (mean) | rows uploaded a frame | extract ms a frame | tick ms a frame |
|---|---|---|---|---|---|
| 1024 units (N = 1) | 6 | 42 | 113 (12.6 KB) | under 0.04 (not in the top ten) | 0.44 |
| 128 units (N = 8) | 105 | 141 | 209 (23.4 KB) | 0.04 | 0.48 |
| 32 units (N = 32) | 1,224 | 1,259 | 1,318 (148 KB) | 0.22 | 0.63 |

Every plane tile is a Set delta every tick and an uploaded row every frame: the deltas
follow the tile count one for one (141 against 42 is the 99 tiles more; 1,259 against 42
is 1,218 more). At the art's own scale (six 1024-unit tiles)
parallax costs six rows a tick and does not matter; it would matter for a plane of many
small tiles (stage three's battle, a ground map's tiles in slice 3), where a per-layer
scroll offset would be one uniform instead of a row per tile. Conclusion in the
improvements note's parallax item.

**R21. Bullets at volume** (`--stress --run-seconds 45 --dial bullet_density=150`: every
volley 150 bullets in a ring around its emitter, many emitters at once). Bullets peak
6,429, mean 1,344, at most 450 spawned in a tick; sprites peak 6,760; Set deltas peak
6,603, mean 1,521; colliders peak 6,470. **Pairs tested peak 1,648,483 in one tick, mean
130,221**: the bullets of a fresh ring sit in one or two cells of a 36-unit grid, and
every pair among them is looked at and thrown out by mask. Broadphase 0.89 ms a frame on
average (all), **7.3 ms worst**; t1 the same (it is one serial pass). Tick 1.56 ms (all),
1.40 ms (t1). The extract: 0.16 ms a frame for ~1,500 deltas a tick, 0.82 ms worst. The
bullets system: 0.18 ms (all), 0.12 ms (t1). The renderer grew its sprite tables once,
4,096 to 8,192 rows, around tick 1,400 (2,736 sprites live in the line before; every row
remarked dirty, so the next frame uploaded the table whole); no frame ring growth. 8.8M
rows uploaded over 2706 frames (3,252 a frame, 356 KB). The broadphase is the first thing
that will break at slice 2's density; the extract and the upload are not yet visible.

## Slice 2 — Bullet hell

Entries 2026-11-09, `linux-dev`, `--prof-report`, the same machine and window as slice 1.
The stage is Shoreline on the pattern library; `--stress` is rank 24 from M4 on (see the
first entry). Zone times are per-frame averages over the run's report windows, "worst" the
worst window's; `render` is still almost all the wait for vsync.

**M4. How much the library makes.** At rank 1 the densest moment of the stage is a few
thousand bullets; the stage's quiet stretches keep the mean low. Peaks by rank over 45
seconds at the stress formations (x3, half spacing): rank 1, 1,761; 2, 3,452; 4, 5,384; 8,
8,499; 16, 17,714; 32, 36,411. At rank 32 the cross turret's stretch holds 20,000 to 36,000
for about seven seconds (ticks 2,100 to 2,500). Twenty thousand sustained is rank 24 or so,
so that is the stress position now (the PRD's R12 said 1).

**M4. The broadphase, before and after** (`--stress --dial rank=4`, all threads; the
same run, deterministically, both times). Before (one grid, pairs tested before masks):
5,421 colliders at peak, **643,172 pairs tested** at peak and 82,175 on average; the
broadphase 0.69 ms a frame on average and 4.5 ms in its worst window. After (grouped by
layer and mask, `collision/src/pairs.cc`): the same colliders, **256 pairs tested** at peak
and 34 on average; 0.12 ms a frame, 1.0 ms worst. At rank 2 before: 243,565 peak, 2.5 ms
worst; at rank 1, 74,091 and 1.4 ms. What is left is linear: filing 24,000 colliders into
grids and the diff. Slice 1's heavy run (6,470 colliders) tested 1.65M pairs; at 23,884
colliders now the peak is 949.

**M4. Stress at three thread counts** (`--stress`, rank 24, 45 s). Bullets peak 23,860,
mean 4,558, up to 800 spawned in a tick; active bullets (with a program) peak 819; runners
stepped peak 828; sprites peak 25,700; Set deltas peak 25,526, mean 4,785.

| | tick | patterns | bullets | broadphase | extract | record |
|---|---|---|---|---|---|---|
| 1 thread | 1.00 (16.4 worst) | 0.053 | 0.104 | 0.26 (1.8) | 0.28 (4.5) | 0.41 |
| 8 threads | 1.12 (14.2) | 0.057 | 0.065 | 0.28 (1.6) | 0.31 (4.5) | 0.37 |
| 16 threads | 1.22 (14.8) | 0.059 | 0.152 | 0.28 (1.8) | 0.31 (4.7) | 0.36 |

ms a frame, worst window in brackets. The whole tick is about a millisecond at 24,000
bullets and threads do not help it: the pool's fan-out costs about what it saves at this
size (the bullets system is fastest at 8, slowest at 16; everything else is serial or too
small to split). The runners cost nothing worth the name (0.06 ms for ~800). The extract's
average is a quarter of the tick, under the PRD's third, but its worst window is 4.5 ms,
which is the next thing to look at (the improvements note). Uploads: 6,400 rows a frame on
average (700 KB at 112 bytes).

**M4. Toward sixty thousand** (`--stress --dial rank=56`). Bullets peak 58,808, mean
9,112, up to 1,824 spawned in a tick, under the bullet kind's 65,536. Tick 1.78 ms a frame
(14.3 worst window); extract 0.53 (8.2 worst); broadphase 0.40 (2.3); bullets 0.37;
patterns 0.06 (2.2 worst); record 0.65. 13,200 rows uploaded a frame on average (1.5 MB).
The frame ring, sized from the sprite-rows hint (21 MB), never grew.

**M4. The renderer's growth** (`--stress --sprite-rows 4096`, against the game's hint of
98,304). With the small hint the sprite tables grew three times as the cross filled the
field: 4,096 to 8,192 rows at frame 1,978 (4,446 live), to 16,384 at 2,023, to 32,768 at
2,094, each remarking every row dirty. One frame near the second growth took 31.8 ms; the
run's other 20-plus ms frames were spread through it, so the growths are not clearly worse
than vsync's own misses at this size. The frame ring grew from 8 to 16 MB in the same
stretch, and that found a bug: a ring that grew mid-frame left the frame's earlier bytes
counted against the new buffer, so its `used` wrapped below zero at release (the run's
"ring peak" read 18,014,398,509,481,983 KB). Fixed in M4; the hint now sizes the ring too.

**M4. Each pattern alone** (`--bench NAME --dial rank=1`, 12 s, one emitter looping).

| pattern | bullets peak | mean | active peak |
|---|---|---|---|
| aimed | 21 | 17 | 0 |
| aimed_burst | 21 | 15 | 0 |
| ring | 167 | 125 | 0 |
| spiral | 120 | 91 | 0 |
| rain | 102 | 71 | 0 |
| needles | 25 | 17 | 25 |
| wave | 83 | 61 | 83 |
| split | 81 | 59 | 2 |
| cross | 113 | 83 | 0 |
| homing | 15 | 10 | 15 |

A runner is 288 bytes (`BmlRunner`); at stress's ~800 runners that is 230 KB of hashed
state, and the 24,000 plain bullets carry none of it.

**M4. Normal under the bot** (`--autoplay`, the minute). Bullets peak 161, mean 23; no
deaths, 12 grazes, 57 kills. (The run's fps dropped to 47 after the window lost focus at
tick 294: the compositor's throttling, not the game.)

**M5. The bomb at scale** (`--bench ring --dial rank=200 --dial invulnerable=0`: one ring
emitter at a rank that keeps 10,000 to 16,000 bullets up; the bot bombs when more than 40
close in). Four bombs in 25 seconds, each into 13,380 to 14,431 bullets: the cancel (every
bullet of both kinds found, destroyed by command, the first 256 made chips, the rest paid
in score) took 1.23 ms the first time and 0.17 to 0.31 ms after. The destroys land at the
next boundary; no frame of the run went past its vsync for it. In Shoreline's stress run
with the ship vulnerable (`--stress --dial invulnerable=0`) the bot bombed at ticks 560
(591 bullets) and 915 (1,479) and made 965 chips in the run, 1,614 bullets past the chips'
cap paid at once; items peaked at 257.

**M5. Normal under the bot, again** (`--autoplay`, the minute). No deaths, 12 grazes, 57
kills, as in M4: the bot never felt crowded enough to bomb, and items peaked at 2 (the
formations' bomb drops, which the bot does not go for). An earlier run of the same binary
died three times; it could not be made to again, under load or not, and two runs side by
side hash the same tick for tick. The likeliest cause is real input reaching the window:
the bot did not hold `focus`, so a Shift (or a pad) would have halved the ship's speed. The
pilots now hold every gameplay action every tick.

## Slice 3 — Shoreline

**M1. The ground map, first try** (`--autoplay`, 25 s, Normal). Shoreline's map is 17 x
650 tiles in three layers, 15,024 rows of `tilemap.tile` (sprites peak 15,362, mean
15,169), scrolled by layer, so the deltas stayed what the air makes (mean 60 a tick). The
extract did not: 1.05 ms a frame on average (2.0 worst) and the tick 1.50, against about
0.5 before the map. The extract compares every live row of every drawable kind against
what it sent, every tick; a map that never changes paid that compare 15,000 times a tick.
R1's exit asks for a cost per tick that does not depend on the map's size, so this is the
engine finding of the milestone (the improvements note, "Many Sets per tick").

**M1. Still kinds** (same run). `KindDesc::still` marks a kind whose rows never change
once spawned; each table counts its spawns, destroys and restores (`Table::churn`), and
the extract skips a still table whose count has not moved, and otherwise looks only at
its births and deaths. `tilemap.tile` is still. Extract 0.002 ms a frame (0.19 worst, the
tick the map spawns), tick 0.50 ms (13.8 worst, the boot's load); the hashes are those of
before the change, at every thread count, I/O backend and asset source. Frames are
vsync's: 51 fps in a window the compositor holds at 50-ish when it is not focused.

**M1. Layer scroll against moving the tiles** (`--bench ring --dial plane_density=N`,
15 s each: three planes at slice 1's three densities, under one ring emitter, whose
bullets are the ~110 deltas a tick that remain).

| planes | tiles | deltas a tick (mean), layer / moved | rows uploaded a frame, layer / moved | extract ms/frame, layer / moved |
|---|---|---|---|---|
| density 1 (1024 units) | 6 | 112 / 118 | 163 / 182 | 0.023 / 0.038 |
| density 8 (128 units) | 105 | 112 / 215 | 172 / 271 | 0.053 / 0.057 |
| density 32 (32 units) | 1,224 | 112 / 1,334 | 178 / 1,227 | 0.18 / 0.21 |

"Moved" is `--dial scroll_rows=1`, slice 1's way, and its numbers are slice 1's (1,259
deltas at 1,224 tiles). On the layer, the planes' deltas are gone at every density: the
count is the bullets' alone. The extract barely moves because the plane kind is not still
(the dial moves its rows, so it cannot be) and its compare is still done: at 1,224 rows
that compare is most of the 0.18 ms. At the art's scale (six tiles) it is nothing.

**M2. The carrier.** A boss is two kinds: the hull (`boss`) and seven parts (`part`), which
`collision.compose` places in Sim before the broadphase (a new engine pass: the graze ring
and the shell use it too). The demo now meets the carrier at tick 2710 and ends at tick
6437, the carrier having timed out (the demo's pilot does not yet kill it at full health),
with hash `95d0e0efabd81879` across thread counts, I/O backends and asset sources. With
the parts' health at a fifth (`--dial enemy_health=0.2`), the demo's rule brings the
carrier down in 34 seconds: turrets, the core's phase at tick 4559, the core at 5062. Under
`--stress` (twice the health, rank 24) the fight runs with the stage's other bullets:
23,367 at peak over the 75 seconds, the tick's cost not yet broken out (M5's entry).

**M3. The stage at full length.** Shoreline is 18,400 units of marks now, its map 26,000
(what scrolls by while the marks wait at the bosses is open water). The demo, under the
rule and with its one egg, meets the carrier at tick 5078, bombs as its core opens (tick
6143), kills it 43 seconds into the fight (tick 7967: a bonus of 503, the time nearly out;
120 bullets to chips) and watches it break up; it meets the battleship at tick 12214, takes
its main guns by tick 16430, is still on the deck hatches when the battleship's time runs
out (tick 17076: it stomps, then leaves), and ends at tick 18409 with 80,173 points. The
bosses' health was cut by about a third from M2's to get there (the carrier's parts the
rule must kill: 266; the battleship's: 354): under the rule the bot does about 7 damage a
second against the carrier's turrets and 6 against the battleship's guns, a third of what
a volley can do, because it spends most ticks dodging. M5 measures the bot at Normal,
vulnerable. Measured at four times the sim's rate (the CLI's `rate 4`), which changes no
tick: a five-minute demo takes a little over a minute.

**M4. Sound** (a minute each, real time: sounds follow the time scale, not the CLI's rate).

| run | voices peak (of 64) | steals | plays merged into a start | dropped | clipped samples |
|---|---|---|---|---|---|
| Normal, `--autoplay` | 26 | 0 | 25 | 0 | 0 |
| `--stress` | 36 | 0 | 4,317 | 0 | 10 |
| `--stress --dial rank=56 --dial particles=48` (50,858 bullets at peak) | 35 | 0 | 4,115 | 0 | 76 |

Fourteen short effects, each starting at most once a tick, cap what can be live at once
well under the pool, however many bullets there are: the merging is what holds it, and the
stealing (tested in the module) is not reached. The clipping at stress is a few dozen
samples a minute: the explosions summed; the buses are at one.

The sounds' pitch draws from the cosmetic stream, which showed that a chip's toss came from
it too: a chip is sim state, so the particle dial (and now every sound) changed where chips
went. A chip's toss is now a hash of its bullet and the tick. The demo's hash is now
`e078b2f13df8fa11` at tick 18409, matching across threads, I/O backends and asset sources.

## Slice 3, M5 — the extract and the rest

**M5. The extract's worst windows** (`--stress` and `--stress --dial rank=56`, 40 s each,
`--profile` traces read tick by tick: each extract's time against its deltas, with zones
around the side tables' growth and the delta list's). Before: the worst windows were all
growth. At rank 56, 14.0 ms for 48,313 deltas of which 12.4 were the delta list growing
(7.8 of 8.8 at stress); a kind's side tables, 2 ms at a time. Without growth the extract
is linear: 50 to 100 ns a delta. With `sprite_rows_hint` sizing them once (from the game's
`sprite_rows`, 98,304): 3.3 ms at rank 56 and 2.0 at stress under load, the growth moved
to the first extract (10 ms) and the bullets' first rows (3 to 4 ms), and 37 MB more
memory live, all of it the dev build's fill of what was reserved.

**M5. Spike hunting** (`--spike-hunt`: no memory fills, poison or guard pages). The same
runs, the delta list's growth costs 0.01 ms with or without the hint, and the worst
extracts are the linear ones: 1.89 ms at stress either way, 2.92 ms at rank 56 without
the hint and 2.89 with it. The growth spikes were the dev build's own fills and its
pages-per-allocation, not the copy. The hint stays for plain dev runs; measurements of
spikes from here on say whether they were taken hunting.

**M5. A boss fight's cost** (the demo through the carrier, `--spike-hunt`, a `--profile`
trace read tick by tick). The tick's median is 0.30 ms before the carrier (ticks 3500 to
5000), 0.30 in its turrets' phase and 0.28 in its core's; the p99 0.89, 0.81, 0.65. The
boss's own system costs 0.002 ms a tick, `collision.compose` 0.004, the broadphase 0.009
with the hull's body and seven parts in its groups, the patterns 0.002. A boss is a few
rows among a few hundred: it is the bullets it fires that cost, and at Normal there are
not many. The demo hashes the same with `--spike-hunt` as without it, every tick (no
read of memory the fills would have hidden).

**M5. The bot at Normal** (`--autoplay`, vulnerable, 430 s real time, 23,179 ticks). It
does not reach the carrier. Each attempt it dies three times, from about 1,500 to 2,400
ticks after the start (the islands' turrets and tanks over the opening's popcorn), and
restarts: seven restarts in the run, never a bomb (at Normal 40 bullets never close in at
once, which is when it bombs). Against the bosses it has been measured invulnerable (M3):
the carrier's core open 31 to 38 s into the fight and the carrier dead at 43 s; the
battleship's guns down at 35 to 70 s, its deck rarely before its time runs out. Clearing
Shoreline at Normal is slice 4's; the rule's two holes are plain: it never bombs to save
itself, and it dodges more than it shoots (a third of a volley's damage at a boss).
Audio in the run: 24 voices at peak, 177 starts merged, nothing stolen or dropped.

**After slice 3: the sky.** Shoreline's parallax: low clouds (1.35 times the ground's
speed), their shadows (the same tiles darkened, at the same rate), high wisps (2.1 times),
each a plane on a layer of its own, 12 still tiles in all and three layer offsets a tick.
The demo's hash is unchanged (`e078b2f13df8fa11`): planes are cosmetic.

**After slice 3: the GPU** (`--spike-hunt --prof-report`, the new `gpu` line; a Radeon 780M,
integrated, RADV). Normal under the bot: 0.43 ms a GPU frame (0.50 worst), sort 0.19, draw
0.23, overdraw 4.3 (the sea, the land and decoration layers, the clouds and their shadows).
Stress, its first 30 s: 0.67 ms, draw 0.45. Rank 56 at its peak (43,987 bullets, 62,378
sprites): 3.3 ms (4.1 worst), sort 0.69, draw 2.37, overdraw 29. After the peak the sort
stays at 0.5 ms with 1,500 bullets left: it works over the table's high-water rows. The
GPU is nowhere near a frame's budget; the frames that miss vsync are the CPU's or the
compositor's. Timings were also checked in a trace: every GPU frame starts 0.1 to 0.9 ms
after its recording began on the CPU, so the calibration lines the tracks up.

**After slice 3: the plane density dial** cut the picture smaller instead of into more
tiles (stress's 8 showed the clouds an eighth their size). Now a tile at density n shows
its 1/n cell of the texture on one grid, the picture the same at any density (12 pixels of
388,800 differ between 1 and 8, in the shots), and the scroll wraps by the texture's period.
Density 32 is 3,648 tiles over three planes, within the plane kind's 4,096.

## Slice 4 — Arsenal

**M1. The beam's cast** (`collision_cast_all`: every collider of every kind, filtered by
layer, a walk). The beam at level 5, `--spike-hunt`, a trace: under `--bench ring --dial
rank=200` (16,212 bullets on average, 19,817 at peak) the `beams` system costs 0.046 ms a
tick at the median, 0.14 at p99, 0.89 at worst; at rank 56 with the beam clearing the field
(6,039 bullets at peak) 0.012, 0.042, 0.11. About 3 ns a collider passed over: at 50,000
bullets, 0.15 ms. Not worth the broadphase's groups yet.

**M2. Three beams** (the same bench with `--dial options=2`: the ship's beam and one from
each option, each its own cast; 16,062 bullets on average, 19,789 at peak). `beams` costs
0.115 ms a tick at the median, 0.34 at p99, 0.43 at worst: two and a half times the one
beam, as three walks of the colliders should be. `options` itself, two rows eased toward
their places, 0.004.

**M3. The chain, by the bot** (`--autoplay --stage shoreline --dial invulnerable=1`, Normal:
the bot does not chase chains, so this is what flying through the stage earns). 284 kills;
the longest chain 34. With the chain 260,585, without it (`--dial chain_window=0`)
234,835: the chain is worth 25,750, about 15% of what the kills and chips paid. The stage
bonus (three eggs, six bombs never used) is 90,000 of either. The demo scores 198,300, just
under the first extend. The PRD's half a million for a good Normal run wants a player who
keeps chains going; that is the tuning pass's to check.

**M4. The bot with the arsenal** (`--autoplay --stage shoreline --difficulty D`, the ship
vulnerable). Slice 3's bot died three times an attempt in the opening islands at Normal and
never reached a boss. This one clears Shoreline at all three difficulties without losing an
egg:

| | Easy | Normal | Hard |
|---|---|---|---|
| eggs lost, restarts | 0, 0 | 0, 0 | 0, 0 |
| bombs used (ticks) | 2 (1619, 8312) | 2 (961, 8365) | 6 (896, 1570, 1803, 3090, 7743, 8473) |
| carrier: on, down | 5078, 5459 (2 s held) | 5078, 5536 (3 s) | 5078, 5607 (4 s) |
| battleship: on, down | 9706, 10040 (5 s) | 9783, 10178 (0 s) | 9854, 10437 (3 s) |
| score | 449,261 | 438,816 | 445,629 |
| longest chain | 34 | 33 | 43 |

What changed: bullets' passes are judged against the way the ship is going (it was running
into the ones it had stepped clear of, a pass that missed where it stood); a dodge keeps
some of the last one, so two passes alike no longer make it dither in place until one
lands; it bombs when a hit is three ticks off and the stick cannot help. The bombs at ticks
8312 to 8473 are the second half's dense stretch, at every difficulty.

Not all of that is the bot: the arsenal outguns Shoreline. The bot has Spread at level five
by tick 1400 (five power items and a tank's before the islands), an option by 3300 and
Storm by 3900, and Storm's bolts find enemies before they fire, as far as off the field.
The bosses are held 0 to 5 seconds before they go down, against slice 3's 8 to 10 at Spread
one: their phases show, briefly, and each fight can be finished, so by the PRD (R16) and the
decision to tune at the end, their health is not touched. That is the first thing the
tuning pass has: the power curve, Storm's reach and damage, the bosses' health. Under
`--stress` the bot's item runs cost a little density: bullets peak 25,753 and average 3,648
over 90 seconds, against 27,364 and 4,584 at M3.

Departures from the PRD, both from the Beam's own rule (a hull stops it): the bot wants
Lance or Storm at a boss, not Lance or the Beam, since the carrier's core and bays are
inside its hull and a Beam ship cannot finish it (the bot that took the Beam timed out at
the carrier every time); and the demo shows the Beam at the islands, not on a boss. Two
rules came out of making the demo's story hold: a weapon item drifts down at a third of
the others' speed, through several turns of its cycle (falling as fast as the rest it left
the field after about one turn, so "take it when it shows the type you want" was luck),
and an item carrier shot down off the field (Storm reaches there) leaves its item inside
it.

**M4. The demo** (`--demo --hash`) is 11,829 ticks (it was 18,901: the bosses go down in
seconds) and hashes e9ff4a19c469df51 at threads 1, 8 and 16, from a kpak and a zip, with
I/O sync and io_uring. Its weapons, picked up on the clock: Spread to five by tick 1400,
the Beam at 3102, an option at 3262, Storm at 3948, the carrier's core bombed and down at
5522, the second option at 7740, Lance at 8857, an egg at 9037, the battleship down at 10930
after 13 seconds held. Score 406,910.

## Slice 5 — Presentation

**M1. A second run is the first.** `engine_sim_reset` empties every kind that is not
cosmetic back to its startup slots, the sim tick to 0, the timers, undelivered events,
unapplied commands and the units' ticket counters dropped. `--demo --again --hash` plays
the demo, resets, and plays it again in the same process: 11,830 hashes each, identical
line for line, ending e9ff4a19c469df51 as the first run and the hash matrix do. The reset
itself, with 26 entities live: nothing worth timing beside the cover's fade.

**M2. The screens hold the sim.** Every screen around a run holds it (`HoldUi`), and a
run's end holds it at once (`HoldGame`): a screen's own hold lands a tick later, which let
one more sim tick through the first time, so the demo's last hashed tick stayed its end
mark's only once `run_over` held it itself. The demo now ends through its results, name
entry and table, and still hashes e9ff4a19c469df51 at tick 11829, seven ways. Found on the
way, in the engine: the sprite extract's removes walked the table's rows, so a table a
fresh sim had emptied (no rows) left its last sprites drawn; they walk every slot it sent
now. Opening a screen costs a frame or two of 40 to 50 fps in a dev build (1,300 UI deltas
as its tree is built), nothing after.

**M3. Record and replay.** A bot run at Normal records 117,958 bytes (11,318 ticks: the
stick changes most ticks, 12 bytes a change) and at Hard 121,548. Each replays with all of
its hashes matching (189 and 193) at 1 and 16 threads, the bot standing down and the file
flying the ship. The first try diverged for stretches of 35 ticks and recovered: a
recorded stick of length 1.00000012, clamped again by the sample, moved the ship a few
millionths differently until a wall evened it out; the replay now sets the sampled value
exactly. The attract mode's demo, from the title, hashes as `--demo` on its first pass and
after each fresh sim; the first pass did not at first, because one sim tick ran on the
title before its screen's hold landed: the title now holds the sim itself from the moment
it opens.

**M4. Glows at rank 24** (`--stress --spike-hunt --prof-report`, 40 seconds, 20,508 bullets
at peak, 3,366 on average; the dial off, then on). A halo is a second sprite a bullet:

| | off | on |
|---|---|---|
| deltas a frame, average (peak) | 3,470 (20,683) | 6,843 (41,190) |
| tick, ms average | 1.57 | 1.95 |
| extract, ms average (worst window) | 0.20 (0.55) | 0.55 (2.84) |
| GPU frame, ms average (worst) | 0.70 (2.00) | 1.65 (7.72) |
| overdraw at peak | 18 | 93 |

The `glows` pass itself is 2 ms of CPU a frame summed over 13 chunks on the pool, about
0.15 ms of the tick. What it costs is the GPU's: twenty thousand 60-unit halos, overdraw
five times over. On at the difficulties, where the bullets number in the hundreds; off
under `--stress` until the renderer draws a bullet and its halo as one.

**M4. Stray halos and shadows.** Two cosmetic kinds drew a streak across the field for a
frame, which the rank 24 run made a flicker (starekrow saw it): a glow row taking a new
bullet was drawn moving from where its last bullet had died, and an air row's shadow,
spawned at the origin and composed onto its owner the same tick, from the field's middle.
A glow now jumps when it takes a bullet (its previous transform set to the new one); a
shadow is born where it belongs.

**M4. A pause changed a pilot's run.** One Normal recording came out 437,649 against the
438,816 every other run of that build gives: a pause and a resume in a bot run, by hand.
The bot sets its actions for the next sample; the next sample was a paused tick's, which
used them up, and the first sim tick after the resume flew on the real devices, nothing
pressed. The engine now keeps an override set on a sim tick for the next sim tick's
sample (paused ticks sample the devices, so a pause menu works); one set on a paused tick
(a script on a menu) is for the next sample as before. A run paused twice, the menu worked
between, now ends at 438,816 too. Left: an action's `was_down` across a pause comes from
the paused ticks, so a press held through one would read as new.

**M4. Particles at render rate.** Sparks are cosmetic rows stepped at 60 Hz; the renderer
interpolates their position and draws their alpha (and every tint) as the last tick left
it. This panel runs at 60 Hz, where a frame is a tick and the question cannot show. On a
144 Hz display a ten-tick spark's fade would step every 2.4 frames: ten levels over a
sixth of a second. Decided: nothing now. If it shows, the cheapest fix is the renderer
interpolating tint as it does position; the engine's render-rate class is not wanted for
it.

**M4. The demo** hashes 0e15cc3d537639a1 at tick 11829 (the `glows` dial is new in the
world row), the same seven ways; the two recorded runs were made again and replay matching
at 1 and 16 threads.

## Tuning (after slice 5, starekrow's pass)

**What changed.** A death drops the weapon a power level. The Beam is shaped by level
(two beams from the ship at 2, each splitting halfway up at 3, three at 4 with the outer
two split, all three split at 5), passes over a boss's hull, and is light on any one
target; an option's beam is one straight beam that leans after the ship's sideways
movement. Heavies (the big yellow ship, 36 to 44 health) come before the carrier and
through the second half; a missile boat (an airship of 300 to 380 health) crosses three
times. Every boss part has three times its health (the battleship's four and a half), and
the timeouts are 60 seconds (carrier) and 90 (battleship).

**The weapons at the bosses** (the bot keeping its weapon, level five, both options,
invulnerable; held seconds before each went down). The first Beam was the fastest, its
focus drawing the segments onto one part at a third more damage and the options' straight
beams on the same target; at a third of a damage a tick a segment, no focus bonus, it is
the slowest, as the trade wants:

| | carrier | battleship |
|---|---|---|
| Spread | 14 | 30 |
| Beam (first) | 11 | 21 |
| Beam | 20 | 37 |
| Storm | 13 | 24 |
| Lance | 12 | 31 |

**The bot, after.** Easy, Normal, Hard: the carrier 15, 27, 28 seconds held, the battleship
21, 29, 30; eggs lost 0, 1, 0; bombs 5, 5, 11; scores 539,362, 533,042, 536,033. The demo
is 14,459 ticks (the fights are longer) and hashes 4c09b8ae7ce5b009 seven ways; the two
recorded runs were made again (14,521 and 14,622 ticks) and replay matching.

## Slice 6 — Orbit and the battle

**M1. Game shaders.** A material names a fragment shader of the game's own; the renderer
keeps eight pipeline slots (the three blends, then five (shader, blend) pairs made on first
use) and sorts by layer and slot, so 2,048 buckets where there were 768. The sort's cost,
the same build before and after under `--bench ring --dial rank=56` (a Radeon 780M): 0.175
to 0.198 ms a frame before, 0.192 to 0.211 after, about 0.015 ms; the draws unchanged. The
shaders are named by their file (`arcfire_nebula`), not by the module's prefix the PRD
proposed (`example_arcfire.nebula`): the shader rule's outputs are its inputs' stems, so
the prefix is in the file name. The interface is `sprite_fragment.slangh` rather than all
of `sprite_common.slangh`: the varyings, the bindings and a texel helper.

The nebula (four octaves of value noise warped by two more such sums, twelve noise
lookups a pixel, between a dark and a bright color) over the whole field costs 0.31 ms of GPU a frame (`--bench ring` against
`--bench ring --gas`: draw 0.36 to 0.67 ms), for one sprite. The dissolve on a boss's hull
is too small to see in the numbers. Hidden from the renderer (`arcfire_nebula.spv` moved
away), the gas logs once and draws as a plain dark wash; the game plays on. The debug
build's runs of both shaders are validation-clean. The demo still hashes 4c09b8ae7ce5b009
at tick 14459 seven ways: the dissolve writes a dying hull's Sprite, a sim column, so the
hashes in the dying differ and meet again once the hull is gone; the two recorded runs
were made again (the same 14,521 and 14,622 ticks, the same input) and replay matching at
1 and 16 threads.

**M2. The full-frame pass and the shimmer.** With a post shader set, the world (every layer
under `Z_POST`, 70) draws into a target of the swapchain's size and format, the shimmer
draws it to the window, and the HUD (90 on) draws over it. Its cost, `--bench ring` with
the `shimmer` dial off and on (no rings: the target and the copy): the GPU frame 0.53 to
0.63 ms, the draw 0.36 to 0.47, overdraw 4.2 to 5.2 (the pass is one more layer over the
window). Under `--stress` the same tenth of a millisecond: sixteen rings at most, a few
dozen ALU operations a pixel each, are lost in the copy's own cost. Turned off, nothing of
the pass is made: the frame is the one before. Without its shader (`arcfire_shimmer.spv`
moved away) the pass logs once and the frame draws straight; the debug build's demo with
the pass on is validation-clean.

The rings are the game's module state, not cosmetic rows as the PRD had them: sixteen
fixed slots that age on unpaused ticks and go to the renderer in window pixels from the
camera (`hud.cc`'s `shimmer`, after the camera on the main thread). A part's or a ground
row's blast, a boss's, a hull's break and the bomb start one; the oldest gives way. At its
widest a ring bends the world about 4 pixels across a band a fifth of its radius wide,
which reads as heat over the islands and almost not at all over open water. A window
resize remakes the target (the swapchain's size moved); nothing here can resize the window
from a script, so that is left for the platform checklist.

Found on the way, from M1: a boss's shadow (a cosmetic child of the hull) stayed whole
while the hull burned away, a grey block where the carrier had been. It fades with the
dissolve now. The `shimmer` dial is a field of the world row's Dials, so every hash
moved: the demo hashes 866dde056d3002a4 at tick 14459 seven ways, and the recorded runs
were made again.

**M3. Orbit.** The second stage: no map, the gas under the stars, debris, twelve station
segments with turrets on their pads, the missile boat twice, heavies from the start, the
gunship pair at 8,800 units and the station core at the end. Five more patterns
(`tight_ring`, `cross_spiral`, `curtain`, `aimed_walls`, `core_finale`), Juhani
Junkala's Level 2 and Ending for the stage and its bosses, the art from
`make_bosses.py`. Both stage files are read at startup; arcade plays Shoreline then Orbit
and goes round a difficulty up; stage select offers both.

Under the bot at Normal, Orbit's whole run: bullets 564 at peak (51 mean), sprites 2,091
(1,118 mean: without the map's 17,000 tiles a stage is small), deltas 1,872 at peak, items
498 at peak (the core's bullets turned to chips). Easy, Normal, Hard under the bot: no
death; one; fourteen and three restarts, most of them in the debris and turrets before the
gunships and at the gunships themselves. The bot that clears Shoreline at Hard untouched
does not clear Orbit's opening at Hard: the end of the game is harder, as the master PRD
asks, and how much is the tuning pass's. With the bot invulnerable and the arsenal
carried in from Shoreline, the gunships go down in 8 to 22 seconds and the core in 32
(Normal) or 80 (Hard).

How it went in, and where it differs from the PRD. A segment is not a ground row: its
picture is a plane row on the map's layer (which scrolls with the stage's distance, as the
ground rows do, so the turrets on its pads stay on them) and its two turrets are ground
rows; the planes take the pictures away when the distance jumps back (a restart, a loop).
The gunships are two bosses on one mark (a boss mark may name several; the stage waits for
all of them), sharing the bar, whose full stays the pair's when one goes down. The stage
change is arcade's alone and is a held tick behind a cover, as a run's start is: nothing
of its timing reaches the sim. A stage without ground has no shadows. The `Stage` row
says which stage it is, so the hash covers it.

Found on the way, from M1: the gas never showed. It drew under the star plane, whose
texture is opaque; the purple I took for it in M1's screenshot was the old nebula texture
plane. The shader ran (M1's 0.31 ms is its cost) and the fallback test showed the plain
material under the same stars. Over the gas the stars are now drawn added (their black
adds nothing), as the PRD has it: a star field over the nebula.

The demo (Shoreline) hashes d28836ef3dcf2d66 at tick 14459 seven ways (new textures and
the `Stage` row's index moved every hash); the two recorded runs were made again and
replay matching. An arcade run at Hard under the bot, recorded across the stage change and
three restarts in Orbit (63,585 ticks), replays matching at 1 and 16 threads, 1,060 hashes:
the change on its held tick behind the cover leaves nothing of its timing in the sim. Not
kept in `tests/replays/` (each platform records its own; two are the PRD's), but the way to
make one is `--arcade --autoplay --difficulty hard --record FILE`. A debug build's Orbit
under the invulnerable bot is validation-clean.

**M4. The battle.** Orbit's background: the capital ship in four sections and a fleet of
small ships, all cosmetic (`battle.cc`). The first pass held the ships in lanes either
side of the field, drifting down with a far plane; starekrow: "it looks like rain, not a
fleet". Now the attackers (orange) come at the capital ship in waves from the field's
edges and strafe it, pass after pass, then leave; its fighters (cyan) chase them, or with
none near wheel after a wandering mote round it; every ship steers (a seek no tighter than
its acceleration) and faces its way; both sides trade tracers that end in flashes and now
and then a burst. The sections break away at fractions of the stage, each burning away
under the dissolve shader with bursts and a shimmer ring; with the last one gone (three
quarters of the way through), the fleet flies off the field and the station core comes
over clear space. Its state is the stage's clock, so a restart puts the ship back whole.

The `fleet` dial at stress (`--stress --stage orbit`, a minute, `--spike-hunt`): with 3,000
asked for, about 2,450 are up at once (the attackers come and go in waves); sprites 690
to 3,140 and Set deltas 110 to 2,540 a frame against `--dial fleet=0`. The extract 0.26 to
0.38 ms a tick; the tick itself 1.28 to 1.30 ms (the battle's own loop is lost in it); the
GPU frame 0.97 to 1.18 ms (sort 0.30 to 0.32, draw 0.63 to 0.71, overdraw 4.0 to 4.6).
Thousands of sprites doing nothing but moving cost a tenth of a millisecond of extract and
two tenths of GPU: the sprite path's per-row costs, measured on rows the sim never sees.
At Normal's 300 they are in the noise.

Orbit's demo (`--demo --stage orbit`) has no script but the rule: its own weapons by the
stage's clock, a bomb when the station's core opens. It dies once early (the egg plays)
and runs 19,702 ticks, hashing d919148ebf99b49c at its last, the same at 1 and 16 threads
and twice in one process (`--again`). A dev build's demo read a fleet ship after
destroying it (a bus error at once from the freed row's fill; the improvements note has
it). The `fleet` dial is a field of the world row's Dials, so the hashes moved again:
Shoreline's demo hashes 71c75f2fcd49b3e8 at tick 14459 seven ways; the recorded runs were made
again and replay matching; the debug build's Orbit demo is validation-clean.

## Slice 6, closing

Where the game stands. Two stages, four and five and a half minutes under their demos; arcade through both and round
again a difficulty up; four weapons, options, the egg, bombs and chips, the chain; five
bosses (the carrier, the battleship, the gunship pair, the station core) in phases;
twenty-one patterns; three shaders of the game's own (the nebula, the dissolve, the
shimmer) through the engine's material slots and its full-frame pass; a title, stage
select, options, results, name entry and score tables; a demo for each stage, a bot,
attract mode; record and replay. The art is CC0 or drawn here; nothing generated by an
image model.

What it says about the engine, slice by slice, in this notebook's numbers:

- **The sim scales by data, not threads.** Twenty thousand bullets (stress, rank 24) tick
  in about 1.1 ms at any thread count (slice 2, M4); at rank 56, nearly sixty thousand at peak. The
  broadphase's grouping took pairs tested from 643,172 to 256 at peak (slice 2). Threads
  help the patterns' runners and the extract's chunks, and little else at this size.
- **Rows that do not change cost nothing.** Shoreline's 15,000 map tiles are a still kind
  scrolled by a layer offset: their extract went from 1.05 to 0.002 ms (slice 3). The
  battle's 3,000 moving cosmetic rows cost 0.12 ms of extract and 0.2 ms of GPU (M4).
- **The GPU is never the limit here.** 0.43 ms a frame at Normal, 3.3 ms at rank 56's peak
  with overdraw 29 (after slice 3); the 2,048-bucket sort for game shaders costs 0.015 ms
  more (M1), the nebula 0.31 ms over the field, the full-frame pass 0.1 ms (M2). Frames that
  miss vsync are the CPU's or the compositor's.
- **Determinism held through all of it.** Every slice's demo hashes the same across thread
  counts, I/O backends and asset sources, and replays match their recordings, an arcade
  run across the stage change included (M3). Presentation stays out of the sim by
  construction: cosmetic kinds, the module's own random stream, holds behind covers.
- **What the game asked of the engine** is in `docs/design/arcfire-improvements.md`: the
  layer scroll, still kinds, spike hunting, GPU timing, record and replay, a fresh sim, game
  shaders and the full-frame pass among what was built; a few small limits noted for when a
  second game asks.

Left for M5, on the other machine: the platform checklist on Windows (the PRD's table:
the three shaders and their fallback, the full-frame pass against Windows' swapchain
formats and a resize, the GPU timing queries, HiDPI, input layouts, text entry, the
per-user files under %APPDATA%, the sync I/O backend, WASAPI, MSVC /W4 /WX on the unity
build, the ship build packaged) and Windows' own hash matrix and recorded runs. The window
resize that remakes the full-frame pass's target is untested on Linux too: nothing here
resizes a window from a script.

## Windows, first look

Entries 2026-11-11, `windows-dev` (MSVC `/O2`, against gcc's `-O2`; no LTO on either), the
same machine (Radeon 780M, 16 threads), `--prof-report`, a 1280 x 720 window, which at the
panel's 150% is a 1920 x 1080 swapchain: 2.25 times Linux's pixels, if Linux's was 1280 x
720 (no entry here says). Zone times are averages over every report window; a zone outside
a window's top ten counts as nothing there. The Linux side is this notebook's earlier
entries, on older commits, so the ratios are rough until Linux runs the same set at the
same commit.

**Power plan.** Run once on Windows' Balanced plan and again on Performance (on AC): every
number moved 10% or less (the tick at 1 thread 2.12 to 1.92 ms, the rest within noise).
The power plan is not the gap.

**The CPU** (Performance plan; ms a frame).

| | Windows | Linux |
|---|---|---|
| `--stress --spike-hunt`, tick at 1 / 8 / 16 threads | 1.92 / 2.19 / 2.46 | 1.00 / 1.12 / 1.22 (slice 2, M4); 1.57 all threads (slice 5, M4) |
| the same, `sprite.extract` | 0.82 / 0.81 / 0.71 | 0.28 / 0.31 / 0.31 (slice 2); 0.20 (slice 5) |
| `--stress --stage orbit --spike-hunt`, tick / extract | 3.49 / 1.17 | 1.28 to 1.30 / 0.26 to 0.38 (slice 6, M4) |

Stress here: bullets peak 25,041, mean 4,355; sprites peak 43,791. The tick is one and a
half to two times Linux's and the gap grows with load; threads behave as they do on Linux
(slightly slower with more, at this size). The extract is three to four times Linux's,
the largest gap and the first lead. Orbit's Linux entry reports 690 to 3,140 sprites where
this run's mean is 10,522, so that row compares different stretches of the stage.

**The GPU.** Under load it is the same chip doing the same work a pixel: at rank 56
(`--bench ring --dial rank=56`, overdraw 25.8 here, 29 on Linux) the draw is 4.66 ms
against 2.37, about the pixel ratio; the nebula adds 0.65 ms here (`--bench ring` against
`--bench ring --gas`) against 0.31, the pixel ratio again. The sort, which does not depend
on the window, is 0.22 against 0.19 to 0.21. At light load it is not: `--bench ring` draws
in 1.98 ms against 0.36 (5.5 times, for 2.25 times the pixels), on either power plan,
which does not govern the GPU's clocks. Unexplained; never near a frame's budget.

**Open: `--gas` halves the tick.** `--bench ring` ticks at about 1.45 ms and `--bench ring
--gas` at about 0.85, steady window by window and alike on both power plans, though the
flag only adds the nebula plane to the stage (`stage.cc`, `gas_test`): the same sim, the
same 635 sprites. Not yet looked at on Linux.

**The extract, looked into** (`--stress --spike-hunt --threads 1`, 45 s; zones and counters
added for the runs and taken out after). Spike hunting does take on Windows (its log line;
the fills, poison and guard pages all check it), so the comparisons above that name it
are like for like. An extract visits 8,456 slots on average, 5,198 of them live, and
sends 4,509 deltas: nearly every live row moved. The Sets pass is 0.58 of its 0.74 ms, the
Removes 0.1. That is about 165 ns a delta, against slice 3 M5's 50 to 100 and slice 5's
58 (0.20 ms for 3,470).

The code cl makes for the loop is tight: `is_alive`, `handle_at` and the bounds checks
inline, three calls to `schema_equal` a row, and `out.push` calling `Array::reserve` out
of line every push (cl does not inline it; a few ns). A sampled profile (Visual Studio's
collector, unelevated, read with `xperf -tle -a profile -detail`) puts `extract_sprites`
and `schema_equal` second and third of the process, together about 17% of its samples;
first is `Map<Handle,u32>::find_dense`, the renderer's entity-to-slot lookup in
`renderer_apply` (the `apply` zone, not the extract). Two changes to `schema_equal`, each
measured and backed out: comparing a word at a time inline instead of calling the CRT's
`memcmp` per field (0.82 to 0.74 ms, inside the run-to-run noise), and comparing
neighbouring fields as one run of bytes (0.74 to 0.78, none). The compare's cost is not
its instructions: it is where each row's data is first loaded, so the misses land on it.
The extract is memory-bound here, and why that is three times Linux's per delta on the
same machine is open: the next step is `perf record` of the same run on Linux at this
commit, to see whether the samples spread the same way (everything slower alike: memory
or clocks) or not (a function cl compiles worse). Linux's transparent huge pages against
Windows' 4 KB pages is one candidate; the arrays a pass streams through (the current and
previous transforms, the sprites, the three side tables, the delta list) are several MB at
stress.

To do: the same set on Linux at the same commit, with the swapchain's size logged, and a
`perf record` of the stress run beside this one; `renderer_apply`'s handle map; the
light-load GPU draw; the `--gas` tick.
