# Principles review: fixes

Nov 18, 2026. A sketch of what to do about every gap a review found between
[the principles](../principles.md) and the code. Written first; nothing here is built.
It touches every principle, and trades a few statements down where holding them in full
would make games harder to write.

## How the review was done

Three reviewers read the code against principles 1 and 7, 2, 3 and 6, and 4, 5, 8, 9 and
10, and their findings were confirmed by reading the code. Alongside that, the claims were
run:

- rocks `--demo` for about 1,550 ticks, hashed every tick, in debug, dev and ship
  builds, at 1 and 16 threads, with early reads ordered and from snapshots, and on the
  CPU renderer: every configuration agrees on every tick.
- arcfire's recorded runs replay matched: Normal to tick 14,122 (236 hashes), Hard to
  15,264 (255), on 16 threads.
- kpack given a file that isn't a zip names it and exits 2.

What is promised and goes through the engine's front door holds: the context, the
tables, `engine_init`. The gaps are where a principle is stated as if it covered more
than that, and at the edges (tools, the glue around vendored code, a few platform files).

## How to choose

The author's rules for this review, from which the decisions below follow:

1. **Favor whoever writes the game.** A fix that adds ceremony to ordinary game code
   (another declaration, another table, an assert that fires on a reasonable call) loses
   to narrowing the statement. Docs that are painful to write don't get written; the
   same goes for declarations.
2. **If the engine can know it, the engine does it.** Prefer a check or a mechanism that
   needs nothing from the author over a rule the author has to remember.
3. **Clearer or shorter code wins.** A fix that deletes code, or makes it read better, is
   preferred over one that adds to it.
4. **A warning that names the fix beats a refusal** when the thing it catches is
   sometimes right.
5. **A change that moves hashes re-records the replays in the same commit.**

## Summary

S, M, L: effort. "Author" is what a game writer has to do differently.

| # | gap | decision | author | code | effort |
|---|---|---|---|---|---|
| 1 | module state isn't hashed | engine-owned sim randomness, `ctx.rng()`; optional field tables on resources; hash the engine's own pending state | less: no seeding code | + | M |
| 2 | Frame/Work may write module state the sim reads | narrow; keep events as the way in | nothing | none | S |
| 3 | Lua's `math.random` seeds from the clock | fixed seed in the lua module | nothing | − | S |
| 4 | UI events into the sim aren't replayed | record cross-cycle events automatically | nothing | + | M |
| 5 | prop handles and collision pairs hash and sort by kind id | use the kind's name hash | nothing | ± | S |
| 6 | the dev access check covers the context only | narrow; give the context what people go around it for; main-thread checks in main-thread APIs; diagnostics into the Frame cycle | nothing; arcfire `.stats` moves | − | M |
| 7 | handlers can't be pinned to the main thread | handlers run on the main thread by default | nothing | ± | S, measured |
| 8 | resources aren't asserted plain data | narrow, assert only resources with field tables | nothing | none | S |
| 9 | field tables can miss a member | dev warning from the layout's gaps | nothing, unless a skip is wanted | + | S |
| 10 | dynamic initializers exist | `-Wglobal-constructors` on clang, an object check on gcc | nothing | − | S |
| 11 | tools give Lua raw `realloc` | one `kv_lua_newstate` in the lua module | nothing | − | S |
| 12 | kvscript's fonts: bytes as code points, unchecked malloc, a second font path | UTF-8, the engine allocator; fold onto truetype later | nothing | − | S |
| 13 | macOS and iOS raw allocations | the general allocator | nothing | ± | S, unbuilt here |
| 14 | typeset pulls render through asset | correct the claim; split only when a tool needs it | nothing | none | S |
| 15 | a truncated Ogg passes silently; bundled reads fold three errors | log with the path and the reason | nothing | + | S |
| 16 | save has a second JSON writer | write through core's | nothing | − | S |
| 17 | renderer-cpu has a second hash | say why, under its own name | nothing | ± | S |
| 18 | four little-endian readers | one in core | nothing | − | S |
| 19 | ship runs a few checks | narrow, after measuring | nothing | none | S |
| 20 | coverage of "how it is held" is by hand | a determinism script; ship in the render gate | nothing | + | S |
| 21 | the docs overclaim | correct them to match the decisions | nothing | none | S |
| 22 | small things | each fixed or deleted | nothing | − | S |

## 1. Deterministic behavior

**1. Module state isn't hashed.** `engine_hash_state` hashes ktables only; module
state, published resources, pending timers, held events and the sim tick are outside it.
rocks keeps its `Rng`, score and next wave in its state, so the hash, the thread
comparison and replays can't see a divergence there until it reaches a column.

Asking for a field table on every module's state is the enforcing fix, and the wrong one:
state holds arrays, handles to assets, caches, all of which would need describing, for
every game. Instead, take away the reason the sim's state lives there and hash what the
engine already owns:

- **`ctx.rng()`, an engine-owned stream per module.** Seeded from the module's name (and
  an engine seed, 0 by default, `--seed N` to change it), hashed with the sim, reset by
  `engine_sim_reset`, saved by `save`. It belongs to the module's state resource, so the
  scheduler already orders every unit that draws from it, and a unit with
  `StateUse::None` that draws fails a dev check. A game deletes its seeding code:

  ```cpp
  // before                                  // after
  struct Game { ...; Rng rng; };             struct Game { ...; };
  g.rng = Rng::seeded(0x70c45, 7);           // nothing
  g.rng.range(-1.5f, 1.5f)                   ctx.rng().range(-1.5f, 1.5f)
  ```

  Cosmetic effects take a second stream, `ctx.rng(".fx")`, unhashed, so they never shift
  the sim's (arcfire's `fx` today, by hand).
- **Optional field tables on resources:** `ResourceDesc{".score", .fields = k_score_fields}`.
  A resource with one is hashed, saved and printable in the CLI, and is asserted plain
  data (8). One line, where the author wants it; nothing where they don't.
- **The engine's own pending state is hashed:** the timer wheel's records, held next-tick
  events, the sim tick. Nothing for the author to do.

The statement narrows to what is true: the hash covers ktables, engine-owned randomness,
resources with field tables and the engine's pending state; other module state is the
module's. Hashes move: re-record the replays.

**2. Frame and Work may write module state the sim reads.** `check_cycle_writes` refuses
writes to sim ktables only. Refusing writes to module state would split every module that
has a presentation half (ui, krome, text) into two modules, which is all cost. Narrow the
statement: Frame and Work never write sim ktables; their module's state is the module's to
keep straight, and events are the way into the sim. cycles.md says so with an example.

**3. Lua's `math.random` is seeded from the clock.** `luaopen_math` seeds from
`luaL_makeseed`, which mixes time and an address, in every Lua state, Krome's included.
Define `luaL_makeseed` as a constant in the lua module's build: one line, every state
deterministic, and a script that wants a varying seed calls `math.randomseed` itself.

**4. UI events into the sim aren't replayed.** cycles.md promises that a game records
events a UI sends its sim; the recorder records actions only. The engine knows from the
declarations which event types are emitted only outside the Sim cycle and read inside it
(`KromeEvent`, which kromedemo's and kromescreens' sims consume). The recorder records those types as they reach the
sim, and a replay drops the live ones and delivers the recorded ones at the same tick.
Automatic, so a menu-driven game replays without the author doing anything. Medium work,
in `frameloop/src/replay.cc` and the event bus; its own short design record first.

**5. Prop handles and collision pairs depend on registration order.** Props hash handle
bits with the kind id in them; columns already use the kind's name hash. Collision sorts
pairs by raw handle bits while its resolve sorts ktables by name. Both use the kind's name
hash. Invisible to authors; moves hashes.

## 3. Your declarations are a contract

**6. The dev check covers the context only.** About a hundred places reach data through
`ctx.engine`, unchecked. Checking every module API against the caller's sets is the
enforcing fix, and it would make ordinary calls (`ui_set_text`, `collision_stats`) assert
until the author declared resources they've never heard of. Narrow, and remove the
reasons people go around:

- **Narrow the statement** to what the code does: accesses through the context are
  checked; what a unit reaches through `ctx.engine` is its own business, and is for
  main-thread units, things that never change, and module functions that say they're
  safe.
- **Main-thread APIs check the thread.** ui, krome and the other main-thread-only
  functions get a dev check that the caller is on the main thread. That is the real
  hazard (a race), it costs a compare, and nobody declares anything.
- **The context grows what people go around it for.** `ctx.live(ktable)` (a read of the
  ktable's liveness) covers most of the reads arcfire does by hand. arcanum's const-cast
  around `read_ktable` goes once the context can do what it wanted.
- **Diagnostics move to the Frame cycle**, which may read anything and runs when nothing
  else does: arcfire's `.stats` and `.stats_mark` lose their undeclared reads by moving,
  with no declarations added.
- arcfire's `.hud` writes ui's model from a main-thread system; with the thread check
  that is now stated and checked rather than lucky.

**7. Handlers can't be pinned to the main thread,** while the ui docs tell handlers to
call main-thread-only functions. Handlers run one at a time with their module's state
already; make them run on the main thread by default, with `.any_thread = true` for a heavy
one. The trap goes away with nothing to declare. Measure arcfire `--stress` first
(principle 8): if handlers lose parallelism that showed, keep the default and add
`.main_thread` instead.

## 2 and 6. Declarative stomps immediate; plain data at every boundary

**8. Resources aren't asserted plain data.** A published resource is a pointer the owner
keeps; nothing copies it, so the assert would buy nothing (collision publishes an
`Array<Handle>`, rightly). Narrow: components, events, appends and resources with field
tables are plain data, asserted.

**9. Field tables can miss a member.** Krome's event table leaves out `x` and `y`. At
`engine_init`, dev builds walk each table's fields in offset order: a gap between two
fields, or at the end, wider than the struct's alignment means a member is missing, and
the log names the struct and the bytes. `KV_FIELD_SKIP(T, m)` says a member is left out
on purpose. Catches the mistake for nothing; costs a line only when the author means it.

**10. "Nothing runs before main" isn't guarded beyond the tables.** A few dynamic
initializers exist (core, platform, arcfire's generated CPU shaders), all harmless.
`-Wglobal-constructors` in the clang toolchains (Apple, Android), and on gcc a check in
`tools/check-fast-objects.sh` style that no object has a `_GLOBAL__sub_I_` symbol. Fix the
three: `constinit` on the globals, and the shader generator. A game that adds a static
with a constructor gets a compile error naming it, which is the principle.

Not changed: handle kinds and asset kinds register in startup, inside `engine_init`.
That's still nothing before `main`; the principle's text says "declared or registered in
`engine_init`".

## 4, 9 and 10. Owners, dependencies, one copy

**11. Tools give Lua raw `realloc`.** goldberg and kvscript call `luaL_newstate()`. One
`kv_lua_newstate(Allocator*)` in the lua module, which Krome, goldberg and kvscript all
call: the allocator and the seed (3) in one place, three copies of the setup deleted.

**12. kvscript's fonts.** It walks text as bytes, so non-ASCII draws wrongly, and mallocs
glyph coverage unchecked. Walk UTF-8 with `utf8_next`, allocate from the scratch arena.
Its font code is a second path to stb_truetype beside `asset`'s, and the two size fonts
differently on purpose (kvscript matches Pillow). Leave the path, say why in a comment;
fold it onto the truetype module when someone next works there.

**13. macOS and iOS raw allocations.** `fs_watch_macos.cc` assigns `realloc`'s result
unchecked; the iOS host window mallocs its framebuffer outside the accounting and fails
silently. Both to `mem_alloc`/`mem_realloc` (thread-safe, so the GCD thread is fine),
checked and logged. Built and run on the next pass on a Mac.

**14. typeset pulls the renderer.** It says it depends on "nothing that draws"; it depends
on `asset`, which depends on `render`, since a font asset is an atlas texture. Splitting
font metrics from atlases is real work that no game asks for today. Correct the comment,
and note the split as the fix when a tool wants typeset without a renderer.

**16. Save has a second JSON writer**, already drifted (`\b` and `\f`). Save's writer
calls core's string and number writers; its copies go. **17.** renderer-cpu's
`hash_bytes`/`hash_mix` are a different algorithm under core's names, for frame diffs that
are never stored. Rename to `frame_hash` with a line saying why it isn't core's, or use
core's if a bench shows no difference. **18.** Four little-endian readers become one
`load_le32` in core; three copies go.

## 5. Fail loudly, with names

**15.** A truncated or corrupt Ogg decodes to a shorter sound and reports success; log a
warning with the path and how far it got. `io_read_bundled` folds CRC mismatch, inflate
failure and a short read into one `EIO`: give each its own message, and the shader loader
passes the reason on.

## 7 and 8. Checks cost in dev; numbers

**19. Ship runs a few checks:** allocator canaries, the engine tag on ids, type keys, and
the always-on cycle refusals. Each is a compare per call and stops a corrupted or
diverging run, which is what a player would want. Time ship with and without them on
arcfire `--stress`; if nothing shows, narrow the statement: ship keeps the per-call checks
that stop a broken run, and drops everything per row or per access.

**20. "How it is held" is held by hand.** Two scripts so nobody has to remember:
`tools/determinism-check.sh` runs rocks `--demo` at 1 and 16 threads, ordered and
snapshot, debug and ship, and compares hashes (what this review did by hand); the render
gate replays the recordings in ship as well as dev.

Principle 8 in the READMEs: a few "costs nothing" claims are argued from the algorithm,
not measured. Narrow the principle to what is reasonable: a claim is a number, or says it
is argued from the algorithm ("by construction"). Saves writing benches for claims that
are true by inspection.

## 21. The docs

To match the decisions above, once they're made:

- scheduling.md and modules.md: the dev check is for accesses through the context.
- modules.md: handles in sim columns are fine (they're generational and hashed by kind
  name); check before use.
- cycles.md: events from a UI are replayed (4); Frame and Work own their module's state
  (2).
- `asset/texture.h` stops recommending `ctx.events->read`, which dev builds refuse;
  arcfire's `stats.cc` comment stops saying it runs after the extract.
- principles.md, the author's to change; proposed:
  - 1: the hash covers ktables, engine-owned randomness, resources with field tables and the
    engine's pending state.
  - 2: "declared, or registered in `engine_init`"; constinit guards the tables, the
    toolchain guards the rest (10).
  - 3: accesses through the context are checked; around it, the unit is on its own.
  - 6: resources with field tables are asserted plain data.
  - 7: ship keeps the per-call checks that stop a broken run (19).
  - 8: a number, or "by construction".
  - 9: Lua's vendored code is `modules/lua/vendor/`.
  - Map order: insertion order until a removal swaps the last entry in.

## 22. Small things

- `.before` naming a unit in another cycle is accepted and ignored: say so once in the
  schedule log, so a stale order is visible.
- level's help strings repeat the command name, which `help` already prints.
- `JsonBuilder::finish()` says "out of memory, or a value attached twice" for every
  refusal: say which.
- `prof_frame` and `prof_tick` record without checking `prof_enabled()`: check, like the
  rest.
- testbed's sim reads the window size: a fixed field size, and the camera fits it.
- Declared dependencies a module doesn't use (`level`→`cli`, and any of
  `frameloop`→`scene`, `script`→`math`, `text`/`tilemap`→`platform` that check out):
  delete them.

## Order

Grouped so each batch is one coherent change, and the ones that move hashes are together:

1. **Deletions and one-copy fixes**, no hash change: 11, 12, 15, 16, 17, 18, 22, 3 (the
   Lua seed changes no sim hash), 10, 13, 14.
2. **The determinism batch**, hashes move, replays re-recorded: 1, 5, then 4 with its own
   design record.
3. **The contract batch**: 6, 7 (measured), 8, 9.
4. **Measurement and scripts**: 19, 20.
5. **Docs**, each with the batch it describes; principles.md when the author has decided.

## Open questions

- [x] `ctx.rng()`: one stream per module (As built). Named sim streams wait for a game that
  wants one; cosmetic streams stay in module state.
- [x] Handlers on the main thread by default: no cost shows (As built).
- [ ] Record cross-cycle events in the existing `.kvr` format, or bump it to `KVR2`?
- [ ] Narrow principle 8 to "a number, or by construction": agreed?

## As built

Nov 19, 2026, on `docs-foundation`, rebased onto Krome slice 6. Item by item:

- **1. Done, a little narrower.** `ctx.rng()` is in `phase.h`: a stream per module seeded
  from its name and `EngineConfig::seed` (`--seed N`, kept in recordings), seeded again by
  `engine_sim_reset`, refused in dev to `StateUse::Read`/`None` units and parallel
  chunks. A stream counts in the hash once drawn from, so a module that never draws (or
  an empty one) adds nothing, as an empty ktable adds nothing (the hash test pins that).
  `ResourceDesc::fields` hashes a published resource; `engine_publish` checks it is plain
  data the table fits. The timer wheel (each record's tick, type and event through its
  schema; never the emitter pointer, nor the ticket, which carries the issuing unit's
  number and so moved the hash whenever any system was added), held next-tick events and
  the sim tick are hashed. rocks and testbed draw from `ctx.rng()`
  and lost their seeding. rocks' score was not given a field table: its high score comes
  from a file at a time the disk decides, which is input, not state.
- **2. Narrowed**, in cycles.md.
- **3. Done:** `luai_makeseed` is a constant in the lua module's build; nothing under
  `vendor/` changed.
- **4. Not done.** It wants the design record the sketch asked for; cycles.md says the
  recorder keeps actions only.
- **5. Half.** Props hash a handle by its kind's name. Collision's pair order is left: "a
  has the lower handle bits" is a contract games read, and two differently built games
  are not promised one hash.
- **6. Done.** scheduling.md and modules.md say what is checked (accesses through the
  context). `ctx.live(ktable)`. `engine_check_main_thread`, called where every `ui_`
  function passes. arcanum's `react` goes through the context instead of a const-cast
  table and three writes made to please the checker (its demo hashes the same tick for
  tick, 1,073 ticks compared). arcfire's `.stats` stays a
  main-thread diagnostic in PostSim: moved to the Frame cycle its per-tick counts would
  mean per frame. Found on the way: since the extract moved to the Frame cycle its
  "deltas" read 0 every tick; a Frame unit after the extract adds them up and
  `stats_mark` is gone. The frame loop's fps line had the same trouble at 120 Hz (a frame
  with no tick reporting 0) and reports a mean over its frames now.
- **7. Done, measured.** Handlers run on the main thread unless `.any_thread = true`.
  arcfire `--stress`, dev, the profiler's tick zone per frame, six pairs alternated:
  main thread 0.537 ms mean, any thread 0.595 ms; main faster in four of six. The runs
  spread 0.15 to 0.75 ms on this machine (a laptop's clocks), wider than any difference:
  no cost shows.
- **8. Narrowed** in the docs; principles.md's wording is the author's (21).
- **9. Done.** The warning found one table, Krome's event; fixed. `KV_FIELD_SKIP` was not
  added: nothing needs to leave a member out on purpose yet.
- **10. Done, without the clang flag.** Five globals are `constinit` with their members
  initialized; `tools/check-static-init.sh` fails a build with a dynamic initializer.
  `-Wglobal-constructors` was not added: no clang here to try it on, and slang's CPU
  shader prelude has one (harmless) that `-Werror` would turn into a broken macOS build.
- **11, 12. Done:** `kv_lua_newstate` in the lua module, used by Krome, goldberg and
  kvscript; kvscript draws UTF-8, allocates checked.
- **13. Done, not built here** (no Apple toolchain).
- **14. The claim corrected**, and the split named as the fix for when it is wanted.
- **15. Done**, more than sketched: a cut Ogg usually has no length at all, so that is
  refused; one that stops early warns and keeps its length in silence. Bundled reads
  log which of four things went wrong.
- **16. Done:** `json_write_string`, `json_write_f64`, `json_write_f32` in core; save's
  copies deleted. A save f32 that did not round-trip (`0x15ae43fd`) does now.
- **17. Renamed, measured:** `frame_hash` is 4x core's on the CPU renderer's records, and
  is never stored.
- **18. Done:** `korvid/core/bytes.h`; seven copies replaced.
- **19. Not measured.** Timing ship with its per-call checks off needs a build that turns
  them off; until then principle 7's wording stays the author's question (21).
- **20. Done:** `tools/determinism-check.sh` does what this review did by hand. Its first
  run found the ticket in the timer hash (1, above): adding a diagnostic system had
  moved arcfire's hashes. It runs rocks in debug, dev and ship, at 1 and 16 threads, in
  both early-read modes and on the CPU renderer, and replays both arcfire recordings.
  Runs by wall time came out a few hundred ticks long on a busy machine (a hidden window's
  frames are throttled, and a frame runs at most four ticks), so frameloop has
  `--run-ticks N`, a run that ends at sim tick N on any machine, and the check runs
  each configuration to tick 1,500. The render gate is unchanged.
- **21. The concept pages are done; principles.md is proposed above, for the author.**
- **22. Done** but for `JsonBuilder::finish()`'s message, which still names two causes.

arcfire's recordings were made again twice: once when the hash grew (the game unchanged:
Normal still ends at tick 14,122, Hard at 15,264), and once after the rebase onto Krome
slice 6, where `asset_live` joined arcfire and moved the hash at tick 120 (`main` replays
its own recordings matched). Both replay matched at 16 threads.
