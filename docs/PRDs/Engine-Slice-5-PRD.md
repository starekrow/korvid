# Engine Slice 5 — Dev PRD

Nov 8, 2026 · @starekrow

## Goal and exit criteria

Slice 4 made the engine good enough for a small game that fits in memory and never changes scenery: `rocks` loads everything at startup and keeps it until exit. Slice 5 is the drive to production: what a game needs once it has more content than one screen, more than one place to be, and a build that ships without a source tree next to it. Assets stop living forever, a pack file replaces the loose `assets/` directory, levels load and unload behind a fade, and music streams and crossfades. A second example, `arcanum`, a small JRPG built on CC0 art and music, is the test bed, and it is written to find the next slice's requirements as much as to prove this one.

The spine does not change. What changes: asset records gain reference counts and scopes, the I/O queue learns ranged reads and mounts, the clock's pause becomes a set of reasons, the camera learns to follow at render rate, the renderer's sort learns a key within a layer, text becomes UTF-8 throughout, tools become engines, and six pieces arrive: a pack format (a zip file) and its tool, a `level` module, a `tilemap` module reading Tiled maps, tile collision, streamed music with fades, and an input action map.

It is done when:

- An asset loaded into a scope goes away when the last scope holding it is released: its renderer texture and material are destroyed past the frames in flight, its sound's samples are freed once the mixer has let go of them, its path leaves the name table, and its handle goes stale. Assets loaded as today stay forever. The leak report at exit names any scope still held.
- `kpack` builds a pack from a game's staged assets as part of the ship build, byte-identical from run to run. A pack is a zip file: `unzip -l` lists it and any zip tool extracts it. The ship build reads every asset from the pack through the same I/O queue and backends; debug and dev read loose files unless `--pack` names one. Any zip mounts, at a cost the dev build reports, and nothing in a game changes between a pack, a zip and a directory.
- `kpack` is an engine: built from the engine's modules with its verbs as CLI commands, its work as systems, its I/O through the queue, and the leak report as part of its exit status. It runs on a machine with no display.
- Text is UTF-8 on every path. A `Text` label shows `Café — “naïve” …` from a UTF-8 source file, a code point the font lacks shows as U+FFFD, a malformed string shows replacement characters and never overruns, and the CLI prints and patches UTF-8 strings.
- A level loads behind a fade with the sim held: its assets arrive into a new scope, the game builds it, the old level's entities and scope go, and the sim resumes on the same sim tick however long the disk took. Assets the two levels share are never reloaded. `level reload` reads a map edited in Tiled again without restarting.
- A Tiled map (`.tmj`, with embedded or `.tsj` tilesets) loads as an asset and renders as tile layers and tile objects in draw order, flips included, with no seams at integer zoom while the camera moves. A game can ask which tile is at a cell, read layer, map and object properties, and find an object by name.
- Sprites in a layer can sort by their feet: the player walks behind a tree trunk and in front of it again with no layer changes, the order following the interpolated position frame by frame. A layer that does not ask for it pays nothing.
- A box moves through a tilemap and stops at solid tiles and solid objects, slides along walls, never snags on the seam between two wall tiles, never tunnels at any speed, and touching a wall is not overlapping it. Every fencepost is a test.
- A music track streams from a compressed Ogg in memory without decoding it whole, honours loop points, and crossfades into another track over a given time with an equal-power curve. Requesting the track already playing does nothing. Music keeps playing through a pause and a level change.
- `arcanum` starts in a village, a player under keyboard or gamepad control walks the map with the camera following, collides with walls, water and props, sorts against trees and NPCs, and walks through an exit into the other map. Escape or Start opens a pause menu whose level select switches between the two maps with a fade and a music crossfade, whose Credits screen names who made the game and its content, and Quit quits. `arcanum --demo --hash` walks, collides, takes an exit, switches levels through the menu and back, and matches across runs, thread counts and I/O backends, and from a pack, a repacked zip and loose files.
- `rocks --demo --hash` still matches; validation stays clean; the leak report is empty; every new module has its README and header docs and a test suite where the logic allows one; `docs/design/arcanum-notes.md` lists what the game wanted and the engine did not have, with every tick of latency the game observed between cause and effect.

## Asset lifetimes

**Scopes (R1).** A scope is a named set of asset references: `asset_scope_create(e, name)` returns an `asset.scope` handle. Every load function gains a scope argument (`asset_load_texture(e, path, scope)` and so on); the existing signatures load into `asset_scope_global(e)`, which is never released, so every game written so far keeps its behaviour. Loading a path a scope already holds is a no-op; loading a path another scope holds adds a reference and returns the same handle, as the name table does today. `asset_scope_release(e, scope)` drops the scope's reference on each member and destroys the scope.

**References and the asset DAG (R2).** A record's count is the scopes that hold it plus the records that depend on it: a material holds its texture, a font holds its atlas texture and material, a tilemap holds its tilesets' textures and materials. When a count reaches zero the record is retired at the next pump: a texture through `renderer_texture_destroy` (already deferred past the frames in flight), a material through `renderer_material_destroy` (sprites still naming it fall back to the default look, and the dev build logs the first one per material), a font with its two, a load still in the queue cancelled if not submitted and discarded on completion if it was. The slot's generation bumps, the path leaves the name table, and a later load of the path reads the file again.

**Sounds and the mixer (R3).** Slice 4 let the mixer read a `SoundAsset`'s samples without a lock because records lived until shutdown. Retiring a sound sends a `Retire(sound)` command through the ring; the mixer stops every voice on it at the next buffer and acknowledges through the return ring; `audio.extract` turns the acknowledgement into the free. No sample is freed while the device thread can reach it, and the mixer still never allocates, frees or logs. Without a device the acknowledgement is immediate.

**Readiness (R4).** `asset_scope_progress(e, scope)` returns pending, ready and failed counts and the bytes read; `AssetScopeReady { scope, failed }` arrives once every member is `Ready` or `Failed`, including members that turned up later (a material's texture, a map's tilesets). A scope that is empty when asked is ready at once. The CLI's filesystem gains `/scopes` (members, counts, bytes) and the existing mounts show each record's count.

## Packs

**A pack is a zip (R5).** Korvid is open source and its games should be easy to take apart, mod and learn from, so the pack format is one every developer already has tools for. A `.kpak` is a zip file (the PKWARE APPNOTE, the subset below) that `kpack` writes in a shape the engine reads fast:

- Every entry is stored (method 0), its data aligned to 4 KiB by padding the local header's extra field, as Android's `zipalign` does. Data can be read in place, and later loaded in place, with one ranged read.
- The first entry is `.kpak/index`: the entries' 64-bit path hashes, sorted, with each entry's exact data offset, size, method and CRC-32, and the central directory's offset and size it was built against. It is an ordinary stored file to any zip tool.
- Output is deterministic: entries in path order, every timestamp the zip epoch (1980-01-01 00:00), no extended-timestamp or Unix-owner extra fields, UTF-8 names (flag bit 11). Zip64 records appear only past 4 GiB or 65,535 entries. The archive comment is `kpak 1`.
- The packer fails the build on a path-hash collision.

**Any zip mounts (R6).** `io_mount_pack(q, path)` takes a `.kpak` or any `.zip`. It reads the end-of-central-directory record (and the Zip64 locator if present) and then:

- **The fast path.** If `.kpak/index` is present and names the central directory as it is, the index is the table: one ranged read per asset at its data offset, no header reads.
- **The degraded path.** Otherwise the mount parses the central directory into the same hashed table (a zip from `zip -r`, a zip edited after packing, a pack a modder rebuilt). A local header's extra field may differ from the central directory's, so an entry's data offset is not known until its local header is read: the first read of an entry is two reads (the 30-byte header with its name and extra, then the data), and the offset is cached for the next. Unaligned data is fine for buffered reads.
- **Compression.** Stored and deflated (method 8) entries load. Deflate goes through the `inflate` module (R36), on the main thread when the read completes (the job pool is the upgrade if hand-made zips of large assets show up in a profile), and the completion looks like any read. zstd (method 93) is P2. Encrypted entries, split archives and other methods fail the entry with `AssetError::Unsupported`.
- **Reporting.** In debug and dev the mount logs one line per pack: entries, whether it has a valid index, how many are deflated or unaligned. `kpack verify` says the same and more; `kpack repack in.zip out.kpak` makes any zip into a fast one. Debug and dev builds check each entry's CRC-32 on read; ship does not.

**Mounts and ranged reads (R7).** `IoRequest` gains `offset` and `length` (zero: to the end). The queue gains a mount list: `io_mount_dir(q, dir)` and `io_mount_pack(q, path)`, searched last-mounted first, so a patch or a mod mounted after the base pack overrides it. A read whose path resolves to a pack becomes a ranged read on the pack's file, opened once (`IORING_OP_READ` at the offset on the uring backend, `pread` on the sync one). Paths are normalized at the mount boundary (`a//b`, `./a`, backslashes from Windows zip tools), which also fixes the slice 4 note that two spellings of one file are two assets; directory entries are skipped. The frame loop mounts `<exe dir>/assets/` in debug and dev and `<exe dir>/<game>.kpak` in ship; `--pack <path>` (repeatable) and `--assets <dir>` override in any config.

**The tool (R8).** `kpack` is a tool module (`output = "exe"`, built for the host in the ship config, as kormake builds every tool) and an engine like any game: it is the first tool built the way R34 says tools are built. Its commands are CLI commands its module registers (`kpack pack`, `kpack list`, `kpack extract`, `kpack verify`, `kpack repack`); its reads and writes go through the I/O queue and its mounts, its CRCs and deflates through the job pool, its memory through the engine's allocators with the leak report at exit. The zip reading and writing code is the `pack` module's (R35), shared with the runtime's mounts, so the tool and the game cannot disagree about the format. It exports a `pack` rule that runs once per executable naming the tool (`tools = { "kpack" }`), with `per = "exe"`: kormake already knows the exact asset files each executable's closure stages under `bin/assets/`, so they are the rule's explicit inputs (passed in a Ninja response file) and it writes `bin/<exe>.kpak`; no depfile is needed. The ship config builds the pack by default; the other configs build it by name. On a native build kormake builds kpack in the target's own config and the rule uses that binary; a cross build needs host instances of source-built tools, which kormake does not have yet (an open question below). A pack entry is a staged file today; when a format earns a cook step (a tilemap, a texture to BC7), the cooker is another tool built the same way, its output is what gets staged, and nothing about packing changes.

**Shipping (R9).** `kormake package linux-ship <exe>` writes `dist/<exe>/`: the executable, its shaders, its pack, `NOTICE` and the third-party licenses the closure carries (SDL, stb, DejaVu, miniz's `tinfl`, arcanum's CC0 content and its credits; each module's `module.lua` names its licenses). The result runs from any working directory with no source tree present.

## Tools on the engine

**Tools are engines (R34).** Korvid's tools are built from the engine, not beside it, so the engine's I/O, jobs, memory, logging, CLI and module model are what a tool author already knows, and every tool exercises them. A tool executable is a module list like a game's: `core`, `platform`, `cli`, the `tool` module, and the tool's own module, which registers its verbs as CLI commands and its work as systems. The `tool` module's `tool_main(argc, argv)` starts the engine headless, runs the command line as one CLI request, then ticks the engine as fast as it will go (no frame pacing, no renderer) until a system calls `tool_finish(e, exit_code)`, shuts the engine down, and exits with that code, or with a failure if the leak report is not empty in debug and dev. A command that has work to do starts it and returns at once, as every command does; the work runs in the tool's systems over as many ticks as the I/O takes. `--threads`, `--log`, `--prof-report` and the rest of the engine's flags work on every tool. A tool module's commands also work in any engine that links it, so a dev build of a game can `kpack verify` the pack it is running from.

**A headless platform (R35).** The platform module brings up SDL's video and gamepad subsystems when a window opens, not at startup, so a tool (or a test, or a server) runs on a machine with no display; the I/O queue, the pref path, the clock and the CLI transports need none of it. The audio device already opens on request; the renderer asks for video itself (`platform_video_up`), since SDL finds the Vulkan loader through the video driver. `cpukiller`'s headless module list can now include the platform.kpak/index` format, local header parsing and the writer, with no I/O of its own, used by the platform's mounts and by `kpack`. Its fuzz target lives with it.

**Inflate (R36).** miniz's `tinfl` (MIT, 3.1.2) is vendored, pinned, and wrapped as the `inflate` module, which, like stb_truetype in the asset module, compiles the vendored file through one `.cc` wrapper and exposes a Korvid API: `inflate_raw(src, dst)` for a zip entry's deflate stream, `inflate_zlib` and `inflate_gzip` for the wrapped forms Tiled can write (P2), each into a caller-sized buffer with no allocation of its own, a CRC-32 (`crc32(seed, bytes)`, which `pack` and `kpack` use too), and a streaming form for the day chunked reads arrive. It has a test suite over streams from Python's zlib and gzip, random garbage included, and the asset module's PNG decoder moves onto it, so the engine no longer links the system's zlib.

## Text is UTF-8

**One encoding everywhere (R37).** Text in Korvid is UTF-8 by default on every path: `Text` strings, font lookups, JSON (already decoded to UTF-8), asset and file paths, zip entry names, Tiled names and properties, log lines, CLI requests and answers, `char` fields in schemas. Slice 4 rendered strings as Latin-1 bytes; that goes, now, before arcanum's content and credits are written against it. What is in the slice:

- `korvid/core/utf8.h`: decode one code point (invalid, overlong and surrogate sequences decode as U+FFFD and advance one byte, so a bad string never loops or overruns), encode, validate, count, and `utf8_copy(dst, cap, src)` that truncates at a code point boundary. Every fixed-size string copy in the engine (`Text::str`, schema `char[N]` patches from the CLI, paths) goes through it.
- Fonts take a character set: `FontDesc { px, ranges }`, defaulting to printable ASCII, Latin-1 Supplement, Latin Extended-A, General Punctuation (dashes, curly quotes, the ellipsis) and U+FFFD. Glyphs are looked up through a sorted code point table with a direct ASCII fast path; kerning is a sorted pair list from the font's kern data, not the dense 224 by 224 table (which does not survive a larger set). A code point the font does not have draws as U+FFFD if the font has it and as nothing otherwise, and the dev build logs the first miss per font.
- `text.layout` and `text_measure` decode UTF-8; `KV_TEXT_MAX` is 64 bytes, not characters, and the header says so.
- The CLI is UTF-8 in and out. The requirements' "UART-safe output: ASCII" becomes a property of a transport: a transport marked ASCII-only (UART, devkit TTY) escapes each non-ASCII code point as `\u{XXXX}` on the way out and unescapes it on the way in; stdin and TCP pass UTF-8 through.
- Zip entry names with the UTF-8 flag are UTF-8; names without it that are not valid UTF-8 are kept as bytes for lookup, logged once per pack, and never converted from code page 437.

Out of scope, as slice 4 had it: shaping, bidirectional text, combining marks drawn as such, line breaking by Unicode rules, normalization (two spellings of `é` are two paths), and text input.

## Levels

**Holding the sim (R10).** `TickClock::paused` becomes `holds`, a bitmask of reasons (`User`, `Loading`, `Focus`, room for more), and the sim is paused while any is set. `engine_pause(e, bool)` sets and clears `User` as before; `engine_hold(e, reason, bool)` is the general form, and the CLI's `pause` shows which holds are set. A player resuming from the menu during a load does not start the sim.

**The level module (R11).** The game hands the module its catalog at startup: `level_catalog(e, LevelDesc[] { name, map, user })`, where `map` is a path the level is built from (a `.tmj` for arcanum; the module itself only cares that it is an asset). `level_request(e, name, LevelTransition { fade_out, fade_in, color, entry })` starts a transition (`entry` is a name the game reads back, the exit the player came through), which runs as a state machine in `level.run` (PreSim, `Always`, main thread):

1. **Leaving.** `LevelLeaving { level }` goes out and the `Loading` hold is set, so the world freezes at the decision (`LevelTransition::freeze`, true by default; false holds only once the screen is covered); the module fades a screen-space full-window sprite in over `fade_out` seconds on the always clock.
2. **Loading.** A new scope is created and the map loaded into it; anything the game loads in its `LevelLoading { level, scope }` handler goes in too. The module waits for `AssetScopeReady`.
3. **Building.** The old level's root entity is destroyed, its tilemap instance with it, and **then** its scope is released, after the new scope holds its references, so shared assets never reach zero. `LevelBuild { level, root, map, entry }` goes out; the game spawns into it in its handler.
4. **Entering.** The hold clears, the fade goes out over `fade_in`, `LevelEntered { level }` goes out.

A request during a transition is queued behind it; a request for the current level is a reload. `level_current(e)` and `level_state(e)` are the queries; `level list`, `level load <name>` and `level reload` are CLI commands.

**Reload (R12).** A reload releases the level's scope **before** loading, the reverse of a switch, so the map, its tilesets and their images are read again; the edit, save, `level reload` loop is the slice's live-import story for maps. The game gets the same `LevelLeaving` and `LevelBuild` as for a switch, with `entry` set to `"reload"`, so arcanum puts the player back where it stood. A reload whose map fails to parse keeps the error on screen (a screen-space label from the level module in dev builds) and leaves the level empty until the next reload; it never crashes.

**What a level owns (R13).** A level has a root entity in the kind `level.root` (`Transform2` only). Game entities that belong to the level are children of it (`Parent` with `OrphanPolicy::Destroy`) and go with it; entities that outlive levels (the party, the HUD) are not. The tilemap's tile rows are owned by the tilemap instance and are not children: tens of thousands of rows in `scene.compose`'s per-tick depth walk is the wrong cost for things that never move.

**Determinism (R14).** Because loading holds the sim, a level's first sim tick has the same `tick_count` however long the I/O took, and `--demo --hash` covers a level switch. The demo script's level requests are keyed to sim ticks. The test runs under `--io sync` and the uring backend, from loose files, the pack and a deflated zip, and compares.

## Tilemaps

**The format (R15).** Tiled's JSON formats: maps (`.tmj`, or `.json`) in orthogonal orientation, finite, right-down render order; tilesets embedded or external (`.tsj`), each a single image with tile size, margin and spacing. Tile layers in `csv`/array encoding or uncompressed base64; global tile IDs with the horizontal, vertical and diagonal flip bits; group layers flattened in order; layer `visible`, `opacity`, `offsetx/y` and `tintcolor`. Object layers as lists of objects (id, name, class, x, y, width, height, rotation, point, rectangle, or tile object with a `gid`). A tileset's per-tile collision shapes (the rectangles of Tiled's collision editor, in the tile's `objectgroup`) and per-tile animations. Custom properties of type string, int, float, bool and color on the map, layers, tilesets, tiles and objects. TMX is XML, and Korvid has no XML reader: Tiled exports JSON from the same document, and the pack rule may later run `tiled --export-map` as a system tool, as the shader rule runs `slangc`. Compressed layer data (zlib, gzip, zstd; `inflate` makes zlib and gzip cheap), infinite (chunked) maps, image-collection tilesets, isometric and hex orientations, Wang sets and templates are later.

**Loading (R16).** `tilemap_load(e, path, scope)` returns a `tilemap.map` handle at once. The read goes through a new generic asset facility, `asset_load_blob(e, path, scope)` (bytes in memory, the same states, events, scopes and mounts as every asset), and the `tilemap.pump` parses it, loads each external tileset as a blob and each tileset image as a material (alpha blend, the image's own texture) in the map's scope, and turns `Ready` when all of them are. The blob settles slice 4's open question about who reads a format: the asset module reads bytes, the module that understands them parses them. Animation clips move onto it (P1). Parse errors carry the JSON position, as materials do.

**Rendering (R17).** `tilemap_spawn(ctx, map, TilemapPlace { origin, base_layer })` creates an instance: one sprite row per non-empty cell per visible tile layer in the cosmetic kind `tilemap.tile` (`Transform2`, `Sprite`), and one per tile object. The Tiled layer's index added to `base_layer` is the sprite layer, so Tiled's layer order is the draw order. A cell's uv comes from its tileset with the flips applied (horizontal and vertical by uv, diagonal as a quarter turn plus a flip), its size is the tileset's tile size in world units of one pixel, its tint the layer's opacity and tint color. A tile object is placed by its bottom-left corner, as Tiled anchors tile objects in orthogonal maps. A layer with the bool property `ysort` sorts its rows by their feet (R20): a tile's feet are its cell's bottom edge, a tile object's its `y`, and a tile property `foot` (pixels up from the bottom) adjusts either. Tiles never move, so after the first `Set` they send no deltas. `tilemap_despawn` destroys an instance's rows; a level's instance goes with the level. Tiled's per-tile animations (water, torches) become anim clips on those rows (P1). A GPU tile-layer pass is the upgrade when a map is too large for a row per tile and is the "background" primitive the requirements name; it is not in this slice (R33).

**No seams (R18).** Tiles are pixel art drawn with the renderer's nearest sampler, so a seam or a shimmer comes from placement, not filtering. The camera's pixel snap (R21) puts the view on the texel grid at integer zoom, tile positions are whole pixels by construction, and a tileset with no spacing does not bleed. Non-integer zoom is allowed and documented as the game's choice.

**Queries (R19).** Over a `Ready` map: `tilemap_size`, `tilemap_cell_of(map, point)`, `tilemap_cell_center`, `tilemap_gid_at(map, layer, x, y)`, `tilemap_layer_find(map, name)`, `tilemap_tile_property(map, gid, key)`, `tilemap_object_find(map, name)`, `tilemap_objects(map, layer)`, `tilemap_property(map, key)` and the same for layers.

## Draw order

**A key within the layer (R20).** Layer is the only draw order today: a bucket is layer and blend, and rows of one bucket draw in row order (`docs/design/renderer.md`). A top-down game needs sprites in one layer ordered by where they stand, every frame, as they move. `Sprite` gains `SpriteSort sort = SpriteSort::Row` and `f32 order = 0`:

- `Row`: as today. A layer whose rows are all `Row` costs exactly what it costs now.
- `Key`: drawn by ascending `order`, ties in row order. For a game that computes its own order.
- `Feet`: drawn by descending world y of the sprite's feet, ties in row order: the feet are the interpolated position plus `order` (for a sprite centered on its position, `order = -size.y / 2`). The key is computed on the GPU from the same interpolated position the vertex shader draws, so the order changes on the frame the sprites cross, not a tick later, and moving sprites send no extra deltas for it.

The CPU already counts rows per bucket; it also counts keyed rows per bucket, and only buckets with keyed rows get the extra pass: after the scatter, a segmented sort of each such bucket's range by `(key, row)`, stable, so `--check-sort` extends to verify keys. Blend is part of the bucket, so a layer that mixes blend modes sorts within each blend mode and draws them in blend order; a sorted layer should be one blend (`alpha`), and the dev build logs a sorted layer with more than one. `Screen` sprites sort the same way in their own space. The delta stream and the GPU row carry `sort` and `order`.

## Camera follow

**Following at render rate (R21).** The sprite module's README notes that a camera a system writes lags a moving entity by the frame's alpha: the followed sprite jitters, which a scrolling tile map makes impossible to miss. `Camera2` gains `follow` (an entity handle with a `Transform2`), `offset`, `bounds` (a world rect the view is clamped inside; empty means none) and `pixel_snap`. When `follow` resolves, the frame loop centers the camera on the entity's interpolated position, at the same alpha the sprites use, then clamps the view to `bounds` (centering on an axis where the view is larger than the bounds), then, with `pixel_snap`, rounds the center to the nearest pixel of the world at the current zoom. A stale `follow` leaves `center` where it was. The frame loop reads the followed entity's interpolated transform from the renderer's side of the stream, not from the sim.

## Collision

The collision module finds overlaps between entities and reports them as events, with no response, one phase after the move. A game on a tile map needs the other half: a box that moves and stops. This is also the slice's instrument for latency: every place the game sees a cause on one tick and its effect on a later one goes in the arcanum notes with the count.

**Solids (R22).** At load, a tilemap builds its solid set: a grid of cells that are solid whole (any tile in the cell has the bool property `solid`, or the cell is non-empty in a layer whose `collision` property is true), the per-tile collision rectangles from the tileset, placed in each cell that uses the tile (flips applied), and the rectangles of objects with `solid` set. `tilemap_solid(map, x, y)` and `tilemap_solids_in(map, rect, out)` query it. The map's edges are solid unless the map says otherwise.

**Moving a box (R23).** `tilemap_move(map, Box2 box, vec2 delta) -> TileMove { vec2 moved; bool hit_x, hit_y; }` moves an axis-aligned box through the solids and returns how far it went: x first, then y, each axis swept against the solids in the box's path, so a box slides along a wall it hits at an angle. The rules, each one a test:

- Boxes are half-open. A box whose right edge equals a wall's left edge is touching, not overlapping, may stay there forever, and may move away or along the wall freely.
- A box stopped by a wall ends exactly on the wall's edge, in exact arithmetic on the box's own values (no epsilon pushes), so positions do not drift into or away from walls over thousands of ticks.
- Moving along a flat wall made of many tiles never stops at the seam between two of them, in either axis order, in either direction.
- A move longer than a tile, or longer than the map, stops at the first solid in its path; nothing tunnels.
- A box that starts overlapping a solid (spawned in a wall, a solid object appeared on it) is not pushed: it may move only in directions that do not deepen the overlap, and the result says so (`started_inside`), so the game can decide.
- A diagonal move into an inside corner stops on both axes; onto an outside corner it slides by the x-then-y rule, documented, the same every time.

**Entities (R24).** Entities collide with each other through the collision module as they do now. Arcanum uses it for exits (sensor rectangles from the `exits` object layer: `OverlapBegin` with the player requests the level) and for NPCs (the player is blocked by solid NPCs). The module's pairs are found after the move, so an NPC block is a tick late: the player overlaps for one tick and is pushed back. That latency is measured, written down, and either accepted or fixed by a same-tick query (`collision_query(ctx, box, mask)` against the entities of the previous broadphase plus a sweep), which is the candidate engine change the notes decide. An exit the player spawns inside (arriving through a door onto the far side's door) must not send it straight back: the game arms an exit only after an `OverlapEnd` with it, and the demo covers the arrival.

## Music

**Streamed sources (R25).** `asset_load_stream(e, path, scope)` returns an `asset.stream`: the compressed file in memory (a whole-file read; from a pack, a ranged one), validated as Ogg Vorbis, with the rate, channels, length in frames and loop points. Reading the file from disk in chunks as it plays is later; a few minutes of Vorbis is a few megabytes. Loop points come from the `LOOPSTART` and `LOOPLENGTH` (or `LOOPEND`) comments in frames, the convention RPG tools write; without them the whole track loops.

**Decoding ahead (R26).** A voice started on a stream owns a stb_vorbis decoder opened over the bytes with a fixed `alloc_buffer` from the audio module's allocator and a PCM ring of one second. `audio.stream_fill` (PostSim, `Always`, main thread, after `audio.extract`, and again in idle time) decodes ahead to keep each ring full, seeking to the loop start at the loop end so the join is exact to the frame. The mixer reads a stream voice from its ring as it reads a sound, at its pitch; an underrun plays silence for the missing frames, counts it, and the counter shows in `audio_stats`. The mixer never decodes. Retiring a stream follows R3.

**Fades (R27).** `audio_fade(ctx, voice, target, seconds, FadeCurve, stop_at_end)` ramps a voice's volume per frame in the mixer, linear or equal-power; a fade replaces any fade in progress from the current volume. It works on every voice, sounds included.

**Tracks and crossfades (R28).** On top, in the audio module: `music_play(ctx, stream, MusicPlay { fade_out, fade_in, overlap, volume })` plays a track on the `Music` bus with `pause_with_sim = false`. A track already playing (by stream handle) is left alone. Otherwise the current track fades out over `fade_out` with equal power while the new one fades in over `fade_in`, starting `overlap` seconds before the old one's fade ends (0: back to back; the full fade: a true crossfade). A third request mid-crossfade fades the outgoing tracks out over what is left of the shorter fade and starts the new one; at most three music voices exist at once. `music_stop(ctx, fade)`, `music_duck(ctx, volume, seconds)` (P1, for a menu or a dialogue box) and `music_current(ctx)` complete it. The tests run the mixer: a fade ends on its target at the right frame, a crossfade's summed power stays flat within a tolerance, a loop point joins without a step, an underrun is silence and a count.

## Input actions

**The action map (R29).** Games read keys and pad buttons today, which is two code paths per control and no repeat for menus. A small `input` layer in the platform module maps physical inputs to named actions, as data: `InputActionDesc { name, kind: Button | Axis2, bindings }` in a `constinit` table the game passes at startup, or a `.input.json`. Bindings name keys, pad buttons, pad sticks (with a dead zone) and the d-pad; an `Axis2` combines four buttons and a stick and clamps to the unit circle. Per tick: `action_down`, `action_pressed`, `action_released`, `action_axis`, and `action_repeat(name, delay, rate)` for menu navigation. Actions are sampled with the keys, so the hash and the demo scripts see them like any input; the demo scripts may drive actions directly. Rebinding at runtime and saving bindings are later.

## The example

`example_arcanum` is a JRPG skeleton, written the way `rocks` is (kinds, systems with declared sets, handlers) and kept honest: anything it has to work around in the engine goes in `docs/design/arcanum-notes.md` with a sentence on why.

- **Content.** CC0 art and music, vendored under `example_arcanum/assets/` with the source, version, download URL and license text in `assets/CREDITS.md`, and credited in the game's `NOTICE` though CC0 asks for nothing. The first choice is the Ninja Adventure pack by pixel-boy (16x16 top-down tilesets, characters with four-direction walk cycles, music and effects, all CC0); Kenney's Tiny Town (CC0, 16x16) is the fallback for tiles. The license is checked against the files in the download itself, not only the store page, when it is vendored. Only what the game uses is vendored.
- **Maps.** Two maps, `village.tmj` and `field.tmj`, sharing the pack's main tileset plus one tileset each, so the switch exercises shared and unshared assets. Layers: `ground`, `detail`, `objects` (`ysort`: tree trunks, fences, signs and houses as tile objects, the player and NPCs drawn here), `above` (canopies, roof edges), and object layers `spawns` (`player_start` and one point per entry) and `exits` (rectangles naming a target level and entry). Tile properties mark water, walls and trunks `solid`, with collision rectangles where a tile is solid only in part. The map property `music` names its track. The maps are made in Tiled and committed as Tiled saves them; a short note in the module README says how to edit one and `level reload` it.
- **Music.** Two looping tracks from the pack as Ogg Vorbis, with `LOOPSTART`/`LOOPLENGTH` added by `vorbiscomment` where a track has a clean loop point and none where it loops whole. The Oggs are committed, so the build needs no audio tools.
- **Playing.** On startup the game loads the `village` level. The player entity (outside any level, so it persists) is placed at the entry it arrived by, or `player_start`, on each `LevelBuild`. It walks under the `move` action in eight directions at a fixed speed with free (not grid) movement, so collision has corners and seams to get wrong, using `tilemap_move` with a feet box; a walk clip per facing from the sheet plays while it moves (slice 4's players) and the facing's idle frame holds when it stops. Its sprite sorts by its feet in the `objects` layer. Two NPCs stand in the village (one paces a fixed route) and block the player. The camera follows the player, clamped to the map, at an integer zoom picked from the window height, with pixel snap.
- **Pausing.** `menu` (Escape or Start) pauses and opens a menu in screen space: Resume, Level (a submenu listing the catalog, the current one marked), Credits, Quit. `move` with repeat navigates, `confirm` selects, `cancel` backs out or resumes. Choosing a level closes the menu, resumes, and requests the level with a half-second fade; on `LevelEntered` the game plays the map's music, which crossfades. Music keeps playing under the menu.
- **Credits.** Credits in the pause menu opens a screen-space credits screen over the dimmed, paused game: Programming and Design, starekrow; Art, and Music and Sound, each CC0 author by name with the pack's title (pixel-boy for Ninja Adventure, Kenney for anything taken from Tiny Town), with a line that the content is CC0; Coding Assistance, Claude Code; then the engine's third-party software (SDL, stb, DejaVu, miniz's `tinfl`). The text is data, `assets/credits.json` (sections of a heading and lines), laid out as one label per line, so a credit changes without a build; it scrolls under `move` when it is taller than the window, and `cancel` or `confirm` returns to the pause menu. A test checks that every source in `CREDITS.md` has a line in `credits.json`, so vendoring a new pack without crediting it fails the suite.
- **Exits.** Walking into an exit requests its level and entry with the same fade. Arriving inside the far side's exit does not trigger it until the player has left it.
- **Demo.** `--demo` walks a fixed path that slides along a wall, runs into an inside corner, walks behind and in front of a tree, bumps the pacing NPC, takes the exit to the field, returns through the menu's level select, opens and closes the credits, issues a reload, and quits. `--hash` prints the per-tick state hash; the check is the one `rocks` uses, with `--io sync`, the pack and a deflated zip added.

## Requirements

| ID | Requirement | Priority |
| --- | --- | --- |
| R1 | Asset scopes; every load takes one; the global scope keeps today's behaviour | P0 |
| R2 | Reference counts over scopes and dependents; retirement of textures, materials, fonts and pending loads; stale handles; the name table forgets | P0 |
| R3 | Sound and stream retirement through the mixer's rings, freed on acknowledgement | P0 |
| R4 | Scope progress, `AssetScopeReady`, `/scopes` in the CLI, counts in the mounts; unreleased scopes in the leak report | P0 |
| R5 | `.kpak` as a zip: stored, 4 KiB-aligned entries, `.kpak/index`, deterministic, Zip64 when needed | P0 |
| R6 | Any zip mounts: index fast path, central-directory degraded path with cached offsets, deflate through `inflate` on the job pool, CRC checks in dev, one-line mount report | P0 |
| R7 | Ranged reads and mounts (directories and packs, override order, normalized paths) on both I/O backends; config defaults, `--pack`, `--assets` | P0 |
| R8 | `kpack` tool module and `pack` rule over the staged assets with a depfile; `list`, `extract`, `verify`, `repack` | P0 |
| R9 | `kormake package`: a self-contained distribution directory with licenses and credits | P1 |
| R10 | Pause as hold reasons; `engine_hold`; the CLI shows holds | P0 |
| R11 | `level` module: catalog, requests with entries, the four-step transition with fades, queued requests, CLI commands | P0 |
| R12 | `level reload`: release first, read again, the player kept in place, parse errors shown and survived | P0 |
| R13 | Level roots in `level.root`; children go with the level; tile rows owned by the tilemap instance | P0 |
| R14 | Level loads hold the sim; `--demo --hash` covers a switch across thread counts, I/O backends and mount types | P0 |
| R15 | Tiled JSON maps and tilesets: orthogonal, finite, csv or uncompressed base64, flips, groups, objects and tile objects, collision shapes, properties | P0 |
| R16 | `tilemap_load` through `asset_load_blob`; tilesets and their materials in the map's scope | P0 |
| R17 | `tilemap_spawn` and `tilemap_despawn`: a cosmetic row per tile and tile object, layer order, flips, opacity, tint, `ysort` layers | P0 |
| R18 | No seams at integer zoom with pixel snap | P0 |
| R19 | Tilemap queries and properties | P0 |
| R20 | Sort keys within a layer: `Row`, `Key`, `Feet` from the interpolated position; keyed buckets only; `--check-sort` covers keys | P0 |
| R21 | `Camera2` follow at render rate from the interpolated transform; bounds; pixel snap | P0 |
| R22 | Tilemap solids: whole cells, per-tile rectangles, solid objects, map edges; queries | P0 |
| R23 | `tilemap_move`: swept, axis-separated, half-open, exact stops, no seams, no tunnelling, `started_inside`; every fencepost tested | P0 |
| R24 | Entity collision in arcanum through the collision module; the one-tick NPC latency measured and decided; exits armed after leaving | P0 |
| R25 | `asset.stream`: Ogg in memory, loop points from the comments | P0 |
| R26 | Decode-ahead rings filled on the main thread and in idle time; underruns counted | P0 |
| R27 | `audio_fade` in the mixer, linear and equal-power, on any voice | P0 |
| R28 | `music_play` with crossfade, overlap, same-track no-op; `music_stop`, `music_current`; mixer tests | P0 |
| R29 | Input action map: buttons and 2D axes over keys, pads and sticks; edges and repeat; demo scripts drive actions | P0 |
| R30 | `example_arcanum` as described on vendored CC0 content, with the credits screen from `credits.json` checked against `CREDITS.md`, `--demo --hash`, `arcanum-notes.md` with the latencies | P0 |
| R31 | Tiled tile animations as clips; anim clips loaded through `asset_load_blob`; `music_duck`; `collision_query` if the notes call for it | P1 |
| R32 | Chunked streaming from disk; zstd entries in packs; compressed Tiled layer data; TMX through `tiled --export-map`; a mods directory mounted after the pack | P2 |
| R33 | A GPU tile-layer pass in place of a row per tile; parallax image layers | P2 |
| R34 | Tools are engines: the `tool` module, `tool_main` and `tool_finish`, commands as CLI commands, work as systems, the leak report as an exit status; `kpack` built this way | P0 |
| R35 | Headless platform (video and gamepads on window open); the renderer brings video up itself; the `pack` module for the zip format, shared by mounts and `kpack`, with a fuzz target | P0 |
| R36 | `inflate` module wrapping vendored `tinfl`: raw, zlib and gzip streams into caller buffers, CRC-32, tests and a fuzz target | P0 |
| R37 | UTF-8 everywhere: `utf8.h`, font character sets with sorted glyph and kern tables, UTF-8 layout and measure, UTF-8 CLI with ASCII-only transports escaping, boundary-safe copies | P0 |

## Out of scope for slice 5

- Cooked formats: BC or ASTC textures, binary maps, relocatable load-and-go blobs. The pack carries source files; cooking slots in ahead of it later, and alignment is already there for it.
- Pack signing, encryption or obfuscation. Packs are meant to be opened.
- Residency streaming, memory budgets per scope that evict, and background loading while a level is being played. A level here loads behind a fade.
- Hot reload of arbitrary assets from file watches; `level reload` is the one reload path.
- Physics, slopes, one-way platforms, shapes other than boxes in `tilemap_move`, entity-versus-entity response in the engine. NPC routines beyond a fixed route, dialogue, battles, inventory, saves. Arcanum's later slices will ask for them, with notes.
- Sorting across blend modes within a layer, and sorting by anything but one scalar.
- A UI module. Arcanum's menu is text labels, a cursor sprite and actions; what it needed goes in the notes, and a UI slice starts from them.
- Audio effects, spatialization, beat-synced music transitions, more than one device.
- Windows, macOS, consoles; signing and installers for the packaged build.

## Open questions, risks and milestones

**Open questions**

- Whether a level's map should be the level's whole description (the arcanum way: exits, entries, music and spawns as Tiled properties and objects) or a `.level.json` naming a map and more. The catalog takes a path so either works; arcanum will say which it missed.
- Whether released assets should linger (a recently-used cache with a byte budget) so walking back through a door does not reread the file. Acquire-before-release already covers the shared case; a cache is cheap to add once the scope counts show the misses.
- Whether NPC blocking should be fixed in the engine (a same-tick `collision_query`) or accepted at one tick late. The notes decide with the measurement in hand.
- Whether `audio.stream_fill` belongs on a job instead of the main thread. Decoding a second of stereo Vorbis is a fraction of a millisecond; a job is the move if two tracks and ambience start to show up in the profile.
- Host instances of tools built from source, so a cross build can run kpack (and later cookers) built for the build machine: one Ninja graph with per-config object directories and flags, as the kormake v1 PRD describes. Native builds use the target config's own tool binary until then.
- Whether the action map lives in the platform module or a module of its own. It is in the platform module because sampling is.

**Risks**

- **Retirement races.** Freeing a texture, a material or a sound while something still reads it is the new class of bug this slice opens. Each one has a single deferred path (renderer frames in flight, the mixer acknowledgement, the pump), a stale handle is null everywhere, and the test suite releases a scope while a sprite draws it and a voice plays it.
- **Zip's corners.** Real zips carry data descriptors, mismatched local and central extra fields, Zip64 records on small archives, names in code page 437, and duplicate names. The reader takes sizes from the central directory, re-reads the local header for the offset, treats non-UTF-8 names as bytes, and keeps the last of a duplicate with a log line. The parser has a fuzz target over the central directory and local headers, and the test corpus includes archives from `zip`, `7z`, Python's `zipfile` and Windows Explorer.
- **The keyed sort.** A segmented sort per keyed bucket is a new compute pass in the one part of the renderer that is easy to get subtly wrong. It is stable by construction (key then row), `--check-sort` reads it back, and a bucket with every row `Row` never enters it.
- **Rows per tile.** A 128 by 128 map with three layers is about 49,000 rows, near the renderer's sprite table limit and linear in the extract's per-tick scan of every drawable kind. Arcanum's maps are small; the slice measures the extract and the cull at the large size and records the numbers, and R33 is the answer if they are bad.
- **Determinism across loads.** Anything a game does between `LevelLeaving` and `LevelEntered` that writes sim state on paused ticks breaks the hash. The handlers run on paused ticks by design; the arcanum demo checks it and the level README says it plainly.
- **Underruns.** The decode-ahead ring is a second long; a main-thread stall longer than that is silence in the music. The counter makes it visible, and a long stall already drops frames.
- **Third-party content.** A CC0 pack can change between versions or carry a file under different terms. The version and URL are pinned in `CREDITS.md`, the license is read from the download, and only files the game uses are vendored.
- **Tiled versions.** Tiled's JSON has changed field names across versions (`type` became `class`). The parser accepts both and the test maps cover 1.8 and later.

**Milestones**

1. UTF-8: `utf8.h`, fonts with character sets, text layout, the CLI and schema strings. Scopes, counts, retirement, sound retirement through the mixer, `AssetScopeReady`, the CLI views. `rocks` unchanged and its hash unchanged.
2. The headless platform, the `tool` module, `inflate` and `pack`. Ranged reads and mounts; the zip reader with its corpus and fuzz targets; `.kpak`, `kpack` on the tool module and its rule; the ship config reading the pack; `kormake package`.
3. Holds, the `level` module with a test game that loads two empty levels; the hash across a switch; reload.
4. `asset_load_blob`, the `tilemap` module and its parser tests over Tiled-saved maps; tile rows; camera follow with bounds and pixel snap. The CC0 content vendored and the two maps made.
5. Sort keys in the renderer with `--check-sort`; `ysort` layers; solids and `tilemap_move` with the fencepost suite.
6. The action map; arcanum walking, colliding and sorting, NPCs, exits, the pause menu, the level select and the credits screen.
7. Streams, the decode-ahead, fades, music and crossfades with the mixer tests; arcanum's tracks.
8. The arcanum demo and hash across backends and mounts, READMEs and header docs, `arcanum-notes.md`, the final validation pass.
