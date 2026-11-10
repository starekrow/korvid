# Arcfire improvements

Nov 9, 2026. What `example_arcfire` (modules/example_arcfire/docs/Arcfire-Master-PRD.md)
asks of the engine. A living note: an item arrives when the game finds it, moves to
"Fixed" when a slice builds it (saying where), and the numbers table at the end is the
engine's side of the story. Measurements live in
`modules/example_arcfire/docs/performance-notes.md`, the game's notebook; an entry there
that finds an engine limit adds a line here.

## Identified at PRD time

- **A game's fragment shader.** Stage three's procedural nebula is made on the CPU and
  uploaded; a shader version wants a material to name a fragment shader of the game's own,
  compiled by the build like the engine's, with the material's parameter block as its
  input. The renderer has one pipeline per blend; this is a pipeline per (shader, blend)
  and a bucket dimension. A stretch, slice 6, and the first thing a game has asked of the
  shader side.

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
- **Input record and replay.** The demos are scripts in code. A recorded input log
  (actions per tick) that replays through `input_action_override` would turn a tester's run
  into a hashing test; the frame loop is where it belongs. Slice 5.
- **Many Sets per tick.** The extract compares every row of every sprite kind against
  what it last sent; at 60,000 rows with most moving, that compare is itself a cost worth
  knowing. *Slice 1:* 0.16 ms a frame for ~1,500 Set deltas a tick, 0.82 ms worst at
  6,600. *Slice 2:* 0.31 ms a frame on average at stress (24,000 bullets, 4,800 deltas a
  tick on average), a quarter of the tick; 0.53 ms at 59,000. Its worst windows are the
  thing: 4.5 ms at 24,000 and 8.2 ms at 59,000, where the average is a tenth of that. A
  guess to check first in slice 3: the extract's side tables (`sent`, `sent_sprites`,
  `sent_placements`) are grown with `resize_fill` as a kind's high-water row count climbs,
  so a burst of spawns past the high-water is a copy of all three; sizing them once from
  the kind's capacity (address space, like the tables) would take that out. Then the
  compare itself, which a dirty bit from the writers would replace. *Slice 3, M1:* a map of
  15,000 still tiles cost the compare 1.05 ms a frame by itself; still kinds (Fixed) take
  the rows that never change out of it. The moving rows' compare is what remains.
- **Threads do not help at this size.** At 24,000 bullets the tick is a millisecond at 1,
  8 and 16 threads alike (1.00, 1.12, 1.22 ms); the pool's fan-out costs about what it
  saves. Worth a look when something per bullet costs more than a move: the grain sizes,
  and whether `parallel_for` should stay serial under a row count it measures.

## Found in slice 1

- **Action overrides land on the next sample.** A bot that decides in Sim from sim state
  drives the ship a tick late; the demo runs in Input before the sample to avoid it. Fine
  as it is, worth a line in the platform_hid docs.

## Found in slice 3

- **A pass that reads `*.Transform2` orders everything around it.** `collision.compose`
  reads every kind's transform (a child's parent may be anything) and writes the children
  with colliders; so it comes after every Sim writer of any transform, and a system that
  reads a composed child cannot come before any of them. Arcfire's patterns system read
  the parts and wrote the active bullets' transforms, which made a cycle; it is split in
  two (`actives` before, `patterns` after). A narrower read (the parents' kinds, which a
  kind could declare) would loosen it; nothing else has asked yet.
- **Handlers cannot read a value before a later handler writes it.** Systems mark such a
  read `^`; handlers refuse it. A handler that needed the run as it stood before the
  death's handler (to let the demo die once) went to a field on the ship instead. Worth
  having when a second one asks.
- **A faster clock for measuring.** The CLI's `rate` (`--cli-stdin` with `rate 4`) runs a
  five-minute demo in a little over one, hashes and all; a frame loop option would make it
  a flag.

## Fixed

- **Children lag their parent by a tick in Sim** (found in slice 1, fixed in slice 3, M2).
  `collision.compose` (Sim, after the resolve, before the broadphase) composes the children
  that have a `Collider2` with `scene_compose_with(e, component)`, so the graze ring, the
  shell and a boss's parts are tested where they are drawn; a test has a turret on a part
  on a moving boss meet a standing shot on the tick it gets there. The scheduler's set form
  takes a joined filter for it (`*[Parent+Collider2].Transform2`). The resolve reads child
  bodies early (`^`), where the last compose put them: it moves their parents, then the
  compose follows. The PRD had the pass in `scene` before the resolve; it is in `collision`
  after it, since scene does not know colliders and the resolve moves parents.
- **Parallax: layer scroll** (slice 3, M1). `SpriteLayers` (`sprite/layers.h`, the
  resource `sprite.layers`): an offset per layer the game sets, sent as a `Layer` delta
  when it changes and read by the vertex shader from a 256-entry buffer (binding 7),
  interpolated by the frame's alpha, a snap for a wrap. The camera does not know about
  layers; the renderer knows only an offset per layer. Arcfire's planes and its ground map
  scroll by it: 1,224 plane tiles went from 1,334 deltas a tick to 112 (the rest is the
  bullets) and from 1,227 rows uploaded a frame to 178. `TilemapPlace::scale` draws a map
  larger than its pixels. Slice 1's measurement, for the record: the deltas followed the
  tiles one for one (6 tiles: 42 deltas a tick; 105: 141; 1,224: 1,259), and the extract
  went from under 0.04 ms to 0.22 ms a frame at 1,224 tiles.
- **Still kinds** (slice 3, M1). `KindDesc::still`: rows that never change once spawned.
  A table counts its spawns, destroys and restores (`Table::churn`, `Table::restores`);
  `begin_tick` does not retake a still kind's previous copies unless that count moved, and
  the sprite extract skips a still table whose count has not moved since it last looked,
  and otherwise sends only its births and deaths (a restore: one full compare). A write to
  a living row of a still kind is not drawn: the contract, in the core and sprite READMEs.
  `tilemap.tile` is still. Shoreline's 15,000-tile map: extract 1.05 ms a frame before,
  0.002 after; hashes unchanged.
- **Broadphase cell size and masked pairs** (slice 2, M4). `collision.broadphase` groups
  colliders by (layer, mask), gives each group a grid sized to its own largest member, and
  pairs groups only where their masks meet, the smaller walked over the larger's grid
  (`collision/src/pairs.cc`). At rank 4 of the stress dials: 643,172 pairs tested at peak
  before, 256 after; 4.5 ms worst before, 1.0 ms after. A test holds it against a pass over
  every pair on sixty random scenes; rocks' and arcanum's demos hash as they did.
- **Kind capacity from outside** (slice 2, M4). Capacity turned out to be address space
  only (a table reserves it, rows commit as used, the extract's side tables follow the
  high-water mark), so the bullet kinds are simply large (65,536 plain, 8,192 active).
  `EngineConfig::capacities` and the frame loop's `--capacity KIND=N` set any kind's past
  its KindDesc's; a full kind logs once and counts `Table::refused`. Growable tables were
  not built: nothing measured asks for them.
- **Renderer table growth** (slice 2, M4). `RendererDesc::sprite_rows` and
  `FrameLoopConfig::sprite_rows` (or `--sprite-rows N`) size the sprite tables and the
  frame ring at startup; a growth logs its frame. With a small hint the tables grew three
  times in one stretch at stress with no clear cost over vsync's own misses (the notebook);
  the ring's growth found a bug, fixed: a ring that grew mid-frame counted that frame's
  earlier bytes against the new buffer and its `used` wrapped at release.
- **Events dropped for a stale handle, silently** (slice 2, M4). Dev builds warn the first
  time an event type is dropped for a stale handle, naming the type; `EvStaleOk` marks the
  types where it is the point (`OverlapEnd`).
- **A quit ran out its frame** (slice 2, M4). `engine_frame` runs no tick after a quit is
  asked for; arcfire's demo no longer holds the sim as it quits.

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
- **An `xml` module** (slice 2, M1). `modules/xml`: yxml (MIT, Yoran Heling, commit
  66507906, pinned with hashes in `src/vendor/README.md`) behind a flat tree in the JSON
  reader's shape. Chosen over hoxml for its age, its fixed name stack and a fuzzing history;
  hoxml read as younger, with a closing-tag compare that accepts a prefix and two unbounded
  copies. yxml's own character-reference range check is off (surrogates through as invalid
  UTF-8, U+E000-U+E7FE refused): the wrapper refuses the first, the README states the
  second. Edges pinned by tests; a fuzz test of mutated documents, clean under ASan and
  UBSan at 200,000 cases. kormake gained `private_includes` for the vendored header.
