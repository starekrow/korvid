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
