# Arcfire improvements

Nov 9, 2026. What `arcfire` (examples/arcfire/docs/Arcfire-Master-PRD.md)
asks of the engine. A living note: an item arrives when the game finds it, moves to
"Fixed" when a slice builds it (saying where), and the numbers table at the end is the
engine's side of the story. Measurements live in
`examples/arcfire/docs/performance-notes.md`, the game's notebook; an entry there
that finds an engine limit adds a line here.

## Identified at PRD time

- **Render-rate cosmetic entities.** Requirements v0 names an entity class updated per
  render on real dt, presentation only. Particles are the first thing that may need it:
  ten-tick sparks interpolated from a 60 Hz sim are stepped on a 144 Hz display. Slice 5
  measures the look and the cost before asking.
- **Beams: a segment cast.** `collision_cast` sweeps a box; a beam wants the ordered list
  of bodies along a segment from the ship to the top of the field, by mask, each tick.
  Slice 4.
- **Many Sets per tick.** The extract compares every row of every moving sprite ktable
  against what it last sent and writes a whole state per change: about 65 ns a delta in a
  dev build, 3 ms a tick at rank 56's 50,000. Its spikes are fixed (below); the compare
  itself is what is left, and a dirty bit from the writers is the design that would
  replace it. (Earlier numbers, for the record: slice 1, 0.16 ms a frame for ~1,500 Sets a
  tick; slice 2, 0.31 ms on average at 24,000 bullets, worst windows 4.5 ms there and 8.2
  ms at 59,000.)
- **Threads do not help at this size.** At 24,000 bullets the tick is a millisecond at 1,
  8 and 16 threads alike (1.00, 1.12, 1.22 ms); the pool's fan-out costs about what it
  saves. Worth a look when something per bullet costs more than a move: the grain sizes,
  and whether `parallel_for` should stay serial under a row count it measures.

## Found in slice 1

- **Action overrides land on the next sample.** A bot that decides in Sim from sim state
  drives the ship a tick late; the demo runs in Input before the sample to avoid it. Fine
  as it is. (A pause between used them up, which slice 5 fixed: below.)

## Found in slice 3

- **A pass that reads `*.Transform2` orders everything around it.** `collision.compose`
  reads every ktable's transform (a child's parent may be anything) and writes the children
  with colliders; so it comes after every Sim writer of any transform, and a system that
  reads a composed child cannot come before any of them. Arcfire's patterns system read
  the parts and wrote the active bullets' transforms, which made a cycle; it is split in
  two (`actives` before, `patterns` after). A narrower read (the parents' ktables, which a
  ktable could declare) would loosen it; nothing else has asked yet.
- **Handlers cannot read a value before a later handler writes it.** Systems mark such a
  read `^`; handlers refuse it. A handler that needed the run as it stood before the
  death's handler (to let the demo die once) went to a field on the ship instead. Worth
  having when a second one asks.
- **A faster clock for measuring.** The CLI's `rate` (`--cli-stdin` with `rate 4`) runs a
  five-minute demo in a little over one, hashes and all; a frame loop option would make it
  a flag.

## Found in slice 6

- **A BulletML runner has two top actions.** `core_finale` wanted three things at once
  (a spiral, rings, aimed fans); two of them had to share an action. A pattern of the
  reference's with `top1`..`top3` would not load. Raising it is a field in the runner
  (288 bytes now, every enemy carries one); one pattern asked.
- **Reading a row after destroying it.** The battle read a fleet ship's side for its count
  after `destroy`, and a dev build's freed-row fill made that a bus error, at once: the
  fill doing its job. Release builds would have read stale data quietly. A `destroy` that
  returns nothing to read is the way it is; the note is for the next system that counts
  as it culls.
- **Cosmetic ktables outlive a fresh sim.** `engine_sim_reset` empties the sim's ktables; the
  planes, the map's tiles, Orbit's battle and the segments' pictures are the game's to
  clear (arcfire does it when a stage is entered). Right for scenery a title shows across
  runs; a `KTableDesc` flag saying "cleared with the sim" would save a game the list.

## On macOS (slice 7)

Measured on an M3 Max (12 performance cores, 4 efficiency), a dev build with
`--spike-hunt --prof-report`, the window at 2560 x 1440 pixels (Retina: four times the
pixels of 1280 x 720), Metal at 120 Hz. Every run held 120 fps; most of a frame is the wait
for a drawable.

- **The GPU.** Shoreline's demo: 1.0 to 1.1 ms a frame (sort 0.25 to 0.3, draw 0.75 to
  0.8). Rank 56's peak (`--stress --dial rank=56`: 55,600 bullets, 74,300 sprites): 4.4 ms
  (draw 4.2, sort 0.19), 8.3 worst. Against the Radeon 780M's 0.43 and 3.3 it is the same
  shape at four times the pixels: fill. The first frame's GPU time is 70 ms, once (first use
  of the textures and pipelines). Overdraw is not measured: Metal has no pipeline
  statistics, so the fps line leaves the number out rather than print 0.
- **The tick at rank 56's peak** is 4 to 5 ms, the extract about half of it (2.4 to 2.8 ms,
  around 50 ns a delta against Linux's 65). Threads still do not help: 4.0 ms a tick at 1
  thread, 5.0 at 12, 5.1 at the default 16; the pool's fan-out, with four slower cores in
  it, costs more than it saves. The grain-size question above stands on this machine too.
- **Loading (R5).** macOS reads synchronously, on the main thread, in the asset pump. A dev
  build's boot reads arcfire's 101 files (13 MB, loose) in one pump of 24 ms on a warm
  cache; decoding in the same pumps is about four times that (sounds 58 ms, textures 22,
  fonts 9). `sockit2m`: 3.0 GB/s of engine reads, 5.6 GB/s straight from the
  queue; its cold pass is warm here (macOS cannot drop one file's pages unprivileged).
  A `dispatch_io` backend would move the 24 ms off the main thread and not the decoding,
  so it waits for a game whose reads show. (Done after all, without `dispatch_io`'s
  copies: the dispatch ring, GCD's workers doing `pread`. Orbit's boot spends 137 ms in
  the platform pump against 199 synchronous, its worst pump 35 ms against 66; what is
  left there is opening and sizing each file, still on the main thread.)

## Found on Linux after slice 7

- **A handle's kind id is a registration number, and arcfire seeded from it.** `seed_of`
  (the pattern runners' `$rand`, every fire, the chips' toss) mixed the owner's raw handle
  bits, kind id included. The touchpad module (slice 7), registered before the game, moved
  every one of arcfire's kind ids by one, and with them every seed: Shoreline's demo ran a
  different game from tick 372 on, deterministically, on every platform. The state hash
  already replaces a kind id with its name's hash for this reason; `seed_of` now does too,
  so a seed depends on the entity's kind, slot and generation, and a pre-touchpad build and
  today's hash alike tick for tick. A raw `Handle::bits` is fine as a map key within a run;
  anything that reaches the sim (a seed, an order) wants the name hash. An engine helper
  (`handle_stable_bits`) would say so where the next game looks.
- **The asset tests waited a number of ticks for a read.** Since their fixtures moved to
  the temporary directory (iOS: the app bundle is read-only), Linux reads them from tmpfs,
  which io_uring hands to a worker thread instead of finishing at submit as it does from
  the page cache; the audio, level and tilemap suites failed under load, level by a
  segfault, the audio one alone too. They wait in real time now, and remove their scratch
  trees (`io_remove_tree`, new) instead of leaving one in /tmp per run.

## Fixed

- **A pass over the whole frame** (slice 6, M2). `renderer_post_layer(r, layer, shader)`:
  the layers below `layer` draw into a target of the renderer's own and a game's post
  shader (`post_fragment.slangh`) draws it to the window before the layers above; up to 16
  float4s from `renderer_post_params`. Off, or a shader that will not load, the frame is
  drawn straight as before, at no cost. On: about 0.1 ms of GPU a frame at 1280 x 720.
  Arcfire's shimmer is the first (docs/design/renderer.md, "The full-frame pass").

- **A game's fragment shader** (slice 6, M1; identified at PRD time). `MaterialDesc::shader`
  (a material file's `"shader"`) names a SPIR-V file the game's module builds from its
  `shaders/*.slang` through kormake's shader rule, whose `%{shader_includes}` puts every
  module's `shaders/` on the include path; the game includes `sprite_fragment.slangh`
  (the varyings, the bindings, `sprite_texel`) and defines `fs_main`. The renderer keeps
  eight pipeline slots, the three blends and five (shader, blend) pairs built on first use,
  and buckets by layer and slot: 2,048 buckets, about 0.015 ms more sort a frame at rank
  56. A shader that will not load draws as its blend's engine pipeline, logged once. The
  push block's `time` animates what the sim never sees. Arcfire's nebula gas and the
  bosses' dissolve are the first two (docs/design/renderer.md, "Game shaders").

- **A pause used up a pilot's actions** (slice 5). An override set on a sim tick was taken
  by the next sample, a paused tick's when a pause began, so the first sim tick after the
  resume flew on the devices: a bot run paused once by hand was another run. An override
  set on a sim tick now waits for the next sim tick's sample; paused ticks sample the
  devices (a pause menu works); one set on a paused tick is for the next sample, as a
  script on a menu wants. Left: `was_down` across a pause comes from the paused ticks.

- **Input record and replay** (slice 5; identified at PRD time). `--record FILE` and
  `--replay FILE` in the frame loop: the game's named actions per sim tick since the last
  fresh sim, as changes, and the state hash every 60 ticks; the header carries the game's
  arguments (and `frameloop_record_note`'s, for a run a menu chose), so a replay starts the
  same run; the game's pilots stand down (`frameloop_replaying`). Found on the way: the
  action sample clamps an override's stick to the unit circle, and a recorded stick has
  been clamped once already; a length of 1.00000012 rounds again, so the replay drifted a
  unit's millionth at a time, visible in the hash for a stretch and gone again at a wall.
  `input_action_override_exact` sets it as it is. Two bot runs (Shoreline at Normal and
  Hard) replay with every hash matching at 1 and 16 threads; a recording with one stick
  value flipped is caught at the next hash. Not done: a unit test of the frame loop's part
  (it opens a window), and typed text is not recorded (arcfire takes none on a sim tick).

- **A fresh sim** (slice 5, asked for by the title). A second run in one process began at a
  later sim tick with every ktable's slots and generations where the first left them, and
  patterns seed from `(handle, tick)`: a different run from the same input.
  `engine_sim_reset` (core) puts every ktable that is not cosmetic back to its startup slots
  (`KTable::restore_slots` with none), the sim tick to 0, and drops the timer wheel,
  undelivered events, unapplied commands and the units' ticket counters, then emits
  `SimReset` for module state that follows the sim (the collision module forgets its last
  pairs, whose handles the new run is issued again). A paused tick's units after the call
  see the new sim tick. Tested in core (`reset_test.cc`: handles, hashes and a scheduled
  event against a fresh engine); in the game, `--demo --again --hash`. Cosmetic rows
  parented to a sim entity stay their owner's to clear. Alongside, two dev conveniences:
  the CLI's `press ACTION [TICKS] [X Y]` and `type TEXT` (platform_hid), and `screenshot FILE` (frameloop).
- **The sprite extract's removes past a ktable's rows** (slice 5). A ktable restored to fewer
  slots (a load, a fresh sim's none) left the sprites it had sent above its new row count
  drawn: the remove pass walked the ktable's rows, not what it had sent. It walks every
  slot it sent now.

- **What the GPU does** (after slice 3; asked for by starekrow). Timestamps around the
  frame's passes and a pipeline-statistics query around its sort and drawing, read at the
  frame's next fence (no stall), go to the profiler's new `gpu` track (`prof_track`: spans
  timed on another clock, calibrated to the CPU's) and to `renderer_gpu_stats`, which the
  frame loop logs with its fps line under `--prof-report` or `--profile`; debug-utils names
  and labels on everything, in every build, for RenderDoc and the Radeon GPU Profiler
  (`MESA_VK_TRACE=rgp` on RADV). On this machine (a Radeon 780M, integrated) arcfire's GPU
  frame is 0.43 ms at Normal (sort 0.19, draw 0.23, overdraw 4.3: sea, land, clouds); 0.67
  at stress early on; at rank 56's peak (44,000 bullets, 62,000 sprites) 3.3 ms, 4.1 worst,
  of which the draw is 2.4 at overdraw 29: fill, not vertices, and a sixth of a frame. Two
  findings: the sort's cost follows the sprite table's high-water rows, not the live ones
  (0.2 ms before rank 56's peak, 0.5 to 0.7 after it with 1,500 bullets left), since it
  counts and scatters every row up to the high-water mark; a compaction, or dispatching over
  a live count, would take that out. And overdraw is the number to watch: the bullets'
  sprites are 24 units with most of the square transparent, so a tighter quad (or the
  bullets as points) is the draw's lever.
- **The extract's worst windows** (slice 3, M5). Traced tick by tick: every outlier was a
  table growing, the delta list (12.4 ms at rank 56, 7.8 at stress) or a ktable's side
  tables (2 ms); without growth the extract is linear in its deltas. `sprite_rows_hint`
  (passed by the frame loop from `FrameLoopConfig::sprite_rows`) sizes them once: in a dev
  build the worst extract under load went from 14.0 ms to 3.3 at rank 56 and from 8.8 to
  2.0 at stress, the cost moved to the first extract (10 ms, once) and a ktable's first rows
  (3 to 4 ms). Then **spike hunting** (below) showed what the growth was: with the dev
  build's fills and guard pages off, growing the delta list costs 0.01 ms with or without
  the hint, and the worst extract is 1.9 ms at stress and 2.9 at rank 56 either way,
  linear (55 to 85 ns a delta). The spikes were the dev build's own; the hint stays,
  because plain dev runs are what people play all day.
- **Spike hunting** (slice 3, M5; the user's idea). `dev_spike_hunt(true)`, the frame
  loop's `--spike-hunt`: a dev build without the work that is not checking (memory filled
  as it is handed out and given back, poison on destroyed rows, pool items and released
  arena space, large allocations on pages of their own behind a guard page, every event's
  emitter noted for the trace), keeping its asserts, access checks, counters and profiler.
  A test in the memory test. Timings near a ship build's, from the build everyone runs.
- **Audio under load** (slice 3, M4). `SoundPlay::priority` (0 to 3); a full pool takes
  the oldest voice of the lowest priority not above the play's (`AudioStats::steals`) or
  refuses it; a sound asked for again before the tick's extract starts once, the louder
  play winning (`AudioStats::merged`); voices that pause with the sim play at the time
  scale as a pitch, floored at half (`MixOp::SimRate`). Tests in the module's and the
  mixer's own. In arcfire, the merging does the work: at stress, 4,317 plays in a minute
  were folded into a start already under way, the voices peaked at 36 of 64, and nothing was stolen or
  dropped; at rank 56 with 50,000 bullets, 35. Stealing has not yet been needed by the
  game, only by the test.
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
- **Still ktables** (slice 3, M1). `KTableDesc::still`: rows that never change once spawned.
  A ktable counts its spawns, destroys and restores (`KTable::churn`, `KTable::restores`);
  `begin_tick` does not retake a still ktable's previous copies unless that count moved, and
  the sprite extract skips a still ktable whose count has not moved since it last looked,
  and otherwise sends only its births and deaths (a restore: one full compare). A write to
  a living row of a still ktable is not drawn: the contract, in the core and sprite READMEs.
  `tilemap.tile` is still. Shoreline's 15,000-tile map: extract 1.05 ms a frame before,
  0.002 after; hashes unchanged.
- **Broadphase cell size and masked pairs** (slice 2, M4). `collision.broadphase` groups
  colliders by (layer, mask), gives each group a grid sized to its own largest member, and
  pairs groups only where their masks meet, the smaller walked over the larger's grid
  (`collision/src/pairs.cc`). At rank 4 of the stress dials: 643,172 pairs tested at peak
  before, 256 after; 4.5 ms worst before, 1.0 ms after. A test holds it against a pass over
  every pair on sixty random scenes; rocks' and arcanum's demos hash as they did.
- **KTable capacity from outside** (slice 2, M4). Capacity turned out to be address space
  only (a ktable reserves it, rows commit as used, the extract's side tables follow the
  high-water mark), so the bullet ktables are simply large (65,536 plain, 8,192 active).
  `EngineConfig::capacities` and the frame loop's `--capacity KTABLE=N` set any ktable's
  past its KTableDesc's; a full ktable logs once and counts `KTable::refused`. Growable
  ktables were not built: nothing measured asks for them.
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
