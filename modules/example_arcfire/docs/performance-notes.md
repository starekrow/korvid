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
