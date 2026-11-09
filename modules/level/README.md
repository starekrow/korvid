# level

Places to be. A game gives the module a catalog of levels (a name, a map to load, a word
of its own each) and asks for one by name; the module fades the screen, holds the sim,
loads the level's assets into a scope of their own, lets the game build it under a root
entity, tears the old one down, and fades back in. Assets the two levels share never
leave memory; a reload reads every file again.

## How it works

`level.run` (PreSim, main thread, every tick, after the asset pumps) is a state machine:

| state | what happens | event |
|---|---|---|
| Leaving | the sim is held (`HoldLoading`); the fade quad comes up over `fade_out` seconds | `LevelLeaving{level}` as it starts |
| Loading | a new asset scope, the map loaded into it by the catalog's loader; then wait until nothing in the scope is pending | `LevelLoading{level, scope}` |
| Building | first tick: the old root is destroyed (the scene module takes its children with it, the tilemap its tiles) and the old scope released; second tick: a new root, so no tick shows both levels | `LevelBuild{level, root, map, scope, entry}` on the second |
| Entering | the hold clears on the next tick; the quad fades over `fade_in` | `LevelEntered{level}` at the end |
| Active | the level is live; a waiting request starts the next transition | |

The order in Building is the point: the old scope is released after the new one holds its
references, so an asset both levels load (a tileset, the player's sprite sheet, a sound)
keeps its count above zero and is never read twice. A **reload** (a request for the
current level) does the opposite on purpose: it releases the scope and destroys the root at
the end of Leaving, waits a tick for the asset pump to retire what nobody holds, and only
then loads, so every file the level uses is read from disk again. `LevelBuild::entry` is
then `"reload"`.

**Determinism.** The build happens on held ticks: `tick_count` does not move from the
moment the level starts loading to the moment its build handlers have run, however long the
disk takes. A demo that switches levels at a sim tick reaches the same state at the same
sim tick on every run. The fade runs on every tick, held or not, at the tick's length; its
quad lives in a cosmetic kind and never touches the hash.

## Using it

```cpp
constinit const LevelDesc k_levels[] = {
    {"village", "mygame/village.tmj"},
    {"field",   "mygame/field.tmj"},
};

bool mygame_startup(Engine& e, ModuleInstance& self) {
    LevelConfig cfg;
    cfg.levels = k_levels;
    cfg.load   = tilemap_load;       // null loads the map as a blob
    level_catalog(e, cfg);
    level_request(e, "village");     // the first level comes up out of black
    ...
}

void on_build(TickContext& ctx, Span<const LevelBuild> bs) {
    for (const LevelBuild& b : bs) {
        // Spawn the level's entities; children of b.root (Parent, OrphanPolicy::Destroy)
        // go when the level does. Place the player by b.entry.
    }
}
```

Load anything else the level needs into its scope in a `LevelLoading` handler
(`asset_load_texture(e, path, l.scope)`): the module waits for those too. Entities that
outlive levels (the player, the HUD) are not children of the root.

The root is named `level.root`, so a save that holds a `Parent` pointing at it writes
`"level.root@level.root"` and a load finds whatever root the rebuild made; no remap.

From the CLI: `level list`, `level load <name>`, `level reload`. Edit a map, save it,
`level reload`: the map and everything in its scope come from disk again.

## Quick reference

Header `korvid/level/level.h`:

| name | kind | what |
|---|---|---|
| `LevelDesc`, `LevelConfig`, `LevelMapLoader` | records, function type | the catalog: `{name, map, user}`, the levels and the map loader |
| `LevelTransition` | record | `fade_out`, `fade_in` (seconds), `tint`, `entry` (copied), `freeze` (true: hold the sim from the request) |
| `LevelState` | enum | `None`, `Leaving`, `Loading`, `Building`, `Entering`, `Active` |
| `LevelLeaving`, `LevelLoading`, `LevelBuild`, `LevelEntered` | events | above; `level` is the catalog index |
| `level_catalog(e, config)` | function | copies the catalog |
| `level_request(e, name, t)`, `level_reload(e, t)` | functions | start or queue a transition (the latest queued wins) |
| `level_current`, `level_name`, `level_find`, `level_state`, `level_user` | functions | where things are |
| `level_root`, `level_scope`, `level_map` | functions | the current level's root entity, asset scope, map asset |
| `KV_LEVELS`, `KV_LEVEL_NAME_MAX`, `KV_LEVEL_ENTRY_MAX` | constants | 64 levels, 32-byte names and entries |

What the module registers with the engine:

| name | kind | notes |
|---|---|---|
| `level.root` | kind | `Transform2`; one live at a time, plus one being replaced |
| `level.fade` | kind, cosmetic | `Transform2`, `Sprite`: the quad, screen space, layer 127, only while fading |
| `LevelLeaving`, `LevelLoading`, `LevelBuild`, `LevelEntered` | event types | `LevelBuild::root` has a schema, so a stale root drops the event |
| `level.run` | system, PreSim, main thread, `Always`, after the asset pumps | writes `level.root`, `level.fade` |
| `level list`, `level load`, `level reload` | CLI commands | |

## Behaviour worth knowing

- **Build handlers run on a held tick.** Spawning sim entities there is fine and is what
  keeps the start deterministic; a handler that reads input or the clock as if the sim
  were running is not.
- **The sim stops when the transition starts.** From the request on the sim is held, so
  the player freezes as the screen goes dark and the level's first sim tick follows the
  last one of the level before. `LevelTransition::freeze = false` lets the sim run until
  the screen is covered.
- **The fade's material is made on first use**, since the frame loop creates the renderer
  after the modules have started.
- **A user pause during a fade** stretches the fade in sim ticks, since the fade counts
  every tick. The demo scripts never pause mid-fade.
- **Without a renderer** there is no quad; every step and every hold is the same.

## Remembering

`level_persist(e, {"game.rock"})` names the kinds whose named entities a level remembers.
Leaving a level writes each of them to the level's record by name (`save_row`: columns,
attachments, bag) and notes the names its build made that are gone; building it again, the
module waits for the game's `LevelBuild` handlers to spawn it from the map, then destroys the
gone and gives the rest their rows back, on a held tick before the first sim tick. A
boulder pushed and left, or broken, is where it was left. `level_save(w, e)` and
`level_load(e, doc, v)` carry every level's record in a save (the live level's as it stands);
`level_forget` drops one. Unnamed entities are not remembered, and a kind that is not
listed is rebuilt from the map every time (arcanum's villagers, so a chase is level-local).

## Not in this module

Streaming levels in while one is played, several live levels, save games, level
descriptions beyond a name and a map (a level is what its map and its build handler say),
and transition effects other than a colored fade.
