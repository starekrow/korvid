# Arcfire improvements

Nov 9, 2026. What `example_arcfire` (modules/example_arcfire/docs/Arcfire-Master-PRD.md)
asks of the engine. A living note: an item arrives when the game finds it, moves to
"Fixed" when a slice builds it (saying where), and the numbers table at the end is the
engine's side of the story. Measurements live in
`modules/example_arcfire/docs/performance-notes.md`, the game's notebook; an entry there
that finds an engine limit adds a line here.

## Identified at PRD time

- **An `xml` module.** BulletML is XML, and Tiled's native TMX is XML. A module beside
  `json`, present only in a game that lists it, wrapping a vendored small parser the way
  `inflate` wraps miniz: candidates yxml (MIT, ~6 KiB, byte-fed, non-validating, skips the
  DTD, one root element, the predefined entities and character references only) and hoxml
  (C99, header-only, the same shape); the module's README states the edges the parser
  documents and the tests pin them. A parser is written here only if neither holds up.
  The tilemap module could read TMX through it later. Slice 2.
- **A game's fragment shader.** Stage three's procedural nebula is made on the CPU and
  uploaded; a shader version wants a material to name a fragment shader of the game's own,
  compiled by the build like the engine's, with the material's parameter block as its
  input. The renderer has one pipeline per blend; this is a pipeline per (shader, blend)
  and a bucket dimension. A stretch, slice 6, and the first thing a game has asked of the
  shader side.

- **Broadphase cell size.** `collision.broadphase` files every collider into one uniform
  grid whose cell is the largest collider's diameter (`modules/collision/src`, the
  `cell = 2 * max_r` line). A boss hull of radius 200 puts every bullet on the screen
  into a few cells and the pair walk approaches n² for bodies that, by mask, can never
  meet. *Measured in slice 1, before any boss:* the cell is already only part of it.
  Bullets fired as a ring start in one or two cells, and every pair among them is tested
  and thrown out by mask: at 6,470 colliders (`--dial bullet_density=150`) the broadphase
  tested 1.65M pairs in one tick (130k on average) and took 7.3 ms at worst, serially.
  Bullets never meet bullets; a per-mask-group grid would never look at those pairs. Candidates: build the grid per layer group and only walk pairs across groups
  whose masks intersect; keep the few large bodies in a list tested against the grid
  instead of sizing the grid to them; or a two-level grid. The bullet-versus-one-ship
  case (20,000 against 1) wants to be a lookup from the ship's cell, not a walk over the
  bullets. Slice 2.
- **Parallax.** Each plane tile is an entity moved every tick: a Set delta and an upload
  per tile per tick for what is one offset per layer. A per-layer parallax rate (a small
  table the vertex shader reads, indexed by the row's layer) would scroll a plane for the
  cost of one uniform. Slice 1 ships the game-side version and measures its share of the
  extract and the upload; slice 3 decides. The question underneath: should the camera, or
  the renderer, know about layers at all. *Measured in slice 1:* the deltas follow the
  tiles one for one (6 tiles: 42 deltas a tick in all; 105: 141; 1,224: 1,259) and so do
  the uploaded rows (13, 23, 148 KB a frame); the extract went from under 0.04 ms to 0.22
  ms a frame at 1,224 tiles. At the art's scale (six 1024-unit tiles for three planes) it
  is nothing; a plane of hundreds of small tiles (a ground map, stage three's battle) is
  where a per-layer offset pays.
- **Render-rate cosmetic entities.** Requirements v0 names an entity class updated per
  render on real dt, presentation only. Particles are the first thing that may need it:
  ten-tick sparks interpolated from a 60 Hz sim are stepped on a 144 Hz display. Slice 5
  measures the look and the cost before asking.
- **Beams: a segment cast.** `collision_cast` sweeps a box; a beam wants the ordered list
  of bodies along a segment from the ship to the top of the field, by mask, each tick.
  Slice 4.
- **Audio under load.** 64 voices, no priority, no stealing: a hundred shot sounds a
  second fill the pool and the command ring, and `dropped` counts them. Wanted: a
  priority per play, stealing the oldest or quietest of the lowest priority, and one start
  per sound asset per tick (the second request in a tick retriggers nothing). Slice 3.
- **Kind capacity from outside.** Capacity is a constant in the kind's descriptor;
  `--stress` wants `bullet` four times larger than play does. A per-kind override at
  registration (from the command line or the game's config), or tables that grow. Growth
  touches the page reservation and every slot-indexed side table (the extract's). Slice 2.
- **Renderer table growth.** The sprite table doubles with every row remarked dirty; at
  tens of thousands of rows that is a hitch where the targets allow none. Pre-size from a
  hint (the sum of the kinds' capacities is known at startup), or grow in steps that stay
  under the budget. Slice 2. *Seen in slice 1:* one growth, 4,096 to 8,192 rows, at about
  2,700 live sprites in the bullet-heavy run; not visible in the frame times at that size.
- **Input record and replay.** The demos are scripts in code. A recorded input log
  (actions per tick) that replays through `input_action_override` would turn a tester's run
  into a hashing test; the frame loop is where it belongs. Slice 5.
- **Many Sets per tick.** The extract compares every row of every sprite kind against
  what it last sent; at 60,000 rows with most moving, that compare is itself a cost worth
  knowing. Slice 2 reports it. *Slice 1:* 0.16 ms a frame for ~1,500 Set deltas a tick,
  0.82 ms worst at 6,600, so far linear and small.

## Found in slice 1

- **An event about an entity its own emitter destroys is dropped without a word.** The bus
  drops an event whose schema names a handle that is stale at delivery. The game's first
  `Kill{enemy, at, score}` was emitted by the handler that destroyed the enemy, so every
  kill vanished: no score, no error, no trace line. The rule is right for `OverlapEnd`;
  for an event that is *about* a death it is a trap. Candidates: a dev-build warning (or a
  trace line) when an event is dropped for staleness, once per type; or a field flag that
  says "this handle may be dead, deliver anyway". The game now leaves the handle out.
- **A quit runs out the frame.** `engine_request_quit` from inside a tick lets the frame
  run the rest of its ticks, so a demo that quits on an event hashes one tick more in a
  run whose frame happened to hold two ticks. The game holds the sim (`HoldGame`) as it
  quits so every run ends on the same tick; `engine_frame` could stop running ticks once a
  quit is asked for instead.
- **Children lag their parent by a tick in Sim.** `scene.compose` places children in
  PostSim, so the collision broadphase (Sim) sees the graze ring and the shell where the
  ship was a tick ago (six units, at full speed). Harmless for a ring; it would not be for
  a child hitbox or a turret part's collider on a moving boss (slice 3). A compose before
  the broadphase for kinds with colliders, or the resolve composing what it moves.
- **Action overrides land on the next sample.** A bot that decides in Sim from sim state
  drives the ship a tick late; the demo runs in Input before the sample to avoid it. Fine
  as it is, worth a line in the platform_hid docs.

## Fixed

- **Time scale** (slice 1). `engine_time_scale(e, s)` and `engine_time_scale_get(e)`:
  `TickClock::scale`, a second factor on the time the clock accumulates while unpaused,
  times the existing `rate` (which stays the developer's: the CLI's `rate`, and now `time
  scale` and `/clock/scale`). A tick is still `tick_dt` long; scale 0 runs no sim ticks
  without pausing; paused ticks ignore it. The frame loop's fps line shows it when it is
  not 1. Test in `core/tests/tick_test.cc` (ten seconds at 0.25 is 150 ticks). The egg
  ramps it each tick; the demo, which dies once, hashes the same across thread counts,
  I/O backends and asset sources while frames of slow time vary. Audio is untouched.
- **PNGs of fewer than 8 bits a sample** (slice 1). Kenney's sheets are 4-bit palette
  images, which the decoder refused; it now reads 1, 2 and 4-bit gray and palette images
  (`asset/src/png.cc`, tested with hand-encoded images).
- **Options with values for a game** (slice 1). `FrameLoopConfig::options` and
  `frameloop_option(e, name, i)`, for `--dial name=value`, repeatable.
- **The broadphase's pair count** (slice 1). `CollisionStats::colliders`, `tested`,
  `cell`, for the HUD's counters and this note's numbers.


