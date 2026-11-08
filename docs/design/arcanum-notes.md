# Arcanum notes

Nov 8, 2026. What `example_arcanum` asked of the engine during slice 5, as found while
building it. Each item is either fixed in the slice (and says where) or is a candidate
for a later one.

## Fixed during the slice

- **kormake staged only the top level of `assets/`.** A game with `maps/`, `tiles/` and
  `music/` found nothing. Staging is now recursive.
- **kormake could not build a tool from source.** The pack rule needed `kpack`. Tools
  are now host instances: `host` is a target and a module platform, a host module
  (`tool_kpack`) is built for the build machine in the host config whatever the target, and
  a cross build carries the host's instances under `host/` in the same graph. The
  platform module split (`platform`, `platform_hid`, `platform_audio`) keeps the packer
  off SDL.
- **A Tiled tile object could only be wholly solid.** Trees and houses need collision
  only at the trunk or the walls. Tile objects now collide by their tile's collision
  shapes, scaled to the object.
- **There was no way to force the I/O backend**, which the determinism check needs.
  The frame loop now has `--io sync|uring`.
- **Asset scopes knew only the asset module's own kinds.** A tile map could not live in
  a level's scope. `asset_kind_register` lets other modules' records join scopes.
- **The level module could not declare writes to the asset tables.** A system after the
  pumps that writes a table the pumps read is a cycle to the scheduler. Main-thread asset
  calls are now treated like any game code's: not declared.

- **The level fade never drew.** The level module made its quad's material at startup,
  before the frame loop had created the renderer, and silently had none. It is made on
  first use now. Found by playing, not by a test: nothing checks that a transition is
  visible.
- **The player kept walking into the fade.** The sim held only once the screen was
  covered. Transitions now freeze the sim from the request on (`LevelTransition::freeze`).

- **The hash included handle kind numbers.** Adding a module that registered a handle
  kind changed every hash of every game, though no state did. Handles in fields now hash
  their kind by name (`kv_handle_kind_name_hash`), tables are summed rather than chained
  in registration order, and an empty table counts for nothing (fixed after the slice,
  with save and load).

## Latency the game saw

- **Villagers block in the same tick, by the game's own check.** The collision module
  reports overlaps after the move, so blocking through its events would let the player
  overlap a villager for one tick and then push back. Arcanum instead tests the
  villagers' feet boxes inside `walk`, against where they stood at the end of the last
  tick (`scene.compose` writes their world placement in PostSim). That is a one-tick lag on
  the villager's side only, invisible at walking speed. Candidate: a same-tick query in
  the collision module (`collision_query(ctx, box, mask)` over the last broadphase), so
  a game does not loop over a kind to block.
- **A level's music starts one asset pump after the build.** The track's path is a map
  property, known only once the map is parsed, so the stream loads after the scope is
  ready. The crossfade starts when the stream is ready, a tick or two into the fade-in.
  Candidate: a level loader hook that can add assets after the map has parsed but before
  the build.
- **Children are composed in PostSim**, so a Sim system reading a child's `Transform2`
  sees last tick's placement. It is noted in the slice 4 PRD; arcanum is the first game
  to read children from Sim.

## Wanted, not built

- **A UI module.** The menu is about 120 lines of label building, cursor handling and a
  dimmer sprite. Every game will write it again. A list widget with focus, repeat
  navigation and sounds, laid out in screen space, would cover the pause menu, the level
  select and the credits.
- **Text that wraps and scrolls.** The credits are one label per line, positioned by
  hand. Line wrapping and a scroll region belong with the UI.
- **Character animation from a sheet description.** The walk cycle is hand-written uv
  arithmetic (four facings by four frames). An animation clip per facing exists in the
  anim module, but making eight clips from a grid is boilerplate a sheet description
  (`.sheet.json`: grid, named rows and columns, frame times) would remove.
- **Tile animations.** The tilemap parses Tiled's per-tile animations but does not play
  them (the slice's P1).
- **Facesets and dialogue.** The pack has portraits for every character. A dialogue box
  with a face, a name and paged text is the next thing a JRPG needs.
- **Map transitions with a direction.** Exits are rectangles plus an entry name. A
  transition that keeps the player's offset along the edge, so walking out at the top of
  a road comes in at the top, needs both maps to agree on it.
- **Per-level asset lists.** The character sheets are loaded globally because the game
  makes renderer materials from them by hand. A material that follows a texture asset
  (`asset_material_for_texture`) would let them live in level scopes like the tilesets.

## Save and load (after the slice)

Nov 8, 2026. Boulders to push and hearts to lose gave a save something to hold; the
`save` module and the core pieces under it came from what that needed.

- **A kind could not be put back as it was.** Spawning rows again gives new handles, so
  every handle into the kind breaks and the next spawn differs from the saved run's.
  `Table::restore_slots` (over `Pool::restore_slots`) sets the slots, generations and free
  list exactly; `load_kind` fills the rows. A run continued from a save hashes as the
  saving run did.
- **The JSON reader lost integers past 2^53.** Numbers were doubles only. Integer
  literals now keep their exact value (`JsonValue::integer`, `json_u64`, `json_i64`).
- **Nothing found a handle kind by name.** `kv_handle_kind_find` does, for loading.
- **A handle into a level does not outlive the level.** The villagers' and boulders'
  `Parent` names the level's root, which is a new entity after the load's rebuild. The
  game passes a `SaveRemap` for `level.root`. Candidate: the level module could offer
  the remap itself (`level_save_remap(e)`), since every game that saves children of a
  level needs it.
- **Materials come back by the order they were made in.** `Sprite::material` is saved as
  `render.material#1:1` and read back with today's id for that name, which is right only
  while the game makes its materials in the same order at startup. Candidate: named
  materials (or `asset_material_for_texture`, above) so a save can name what it means.
- **The save's read had to hold the sim.** Without it, the tick the file arrived on
  decided when the menu closed, and the demo's hashes followed the disk. The game holds
  with `HoldLoading` and lets the level transition carry the hold. Candidate: a load
  helper in the level module that takes the bytes and does the hold, the request and the
  restore in one.
- **Enums are saved as numbers.** `Sprite::space` is `0`. A field trait for an enum's
  names would make saves readable and survive reordering an enum.
- **No CBOR yet.** The writer's calls are the data model, so CBOR is a second encoder and
  a decoder into the same tree (see `modules/save/README.md`).
