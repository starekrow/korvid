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
