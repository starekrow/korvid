# example_arcanum

A JRPG skeleton, written to find out what a game larger than one screen asks of the
engine. Two Tiled maps, a village and a field, built from the CC0 Ninja Adventure pack
by Pixel-boy and AAA (`assets/CREDITS.md`). A player walks them under keyboard or
gamepad control, collides with walls, water, trees, houses and villagers, pushes
boulders, loses a heart for each walk into a villager, passes in front of and behind
things by where its feet are, and walks through the road at a map's edge to the other
map. Escape or Start opens a pause menu: save, load, a level select and the credits.
Each map has its own music, and the change between them is a crossfade.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory:

```bash
tools/kormake/bin/linux/kormake build linux-dev
build/linux-dev/bin/example_arcanum
```

The ship build packs the game's assets into `bin/example_arcanum.kpak`, and
`kormake package linux-ship example_arcanum` makes `dist/example_arcanum/`, which runs
on its own.

Controls: arrows or WASD (or the left stick or the d-pad) walk; Escape or Start opens and
closes the menu; Enter, Space or South chooses; Backspace or East goes back.

`--demo` plays a script and quits. It walks east out of the village into the field, goes
back to the village through the menu's level select, pushes the boulder up the lane,
walks into the pacing villager (a heart), saves, walks into the villager again (three
more), loads (the hearts and everything else come back), opens the credits and scrolls
them, reloads the level, and quits. The log's `save:` and `load:` lines carry a hash of
the saved kinds, which match. `--demo --hash` prints the state hash every sim tick. The
hashes match across runs, thread counts (`--threads N`), I/O backends (`--io sync`,
`--io uring`) and asset sources (loose files, `--pack` with the kpak, or `--pack` with
any zip of `bin/assets`). That covers the level loads and the save's read, which hold
the sim, so the disk's speed never reaches the state.

The save is `~/.local/share/korvid/arcanum/save.json` (the demo's is `save-demo.json`,
so a scripted run never overwrites a game).

## How it is built

- **Levels** come from the level module's catalog (`village`, `field`), each a Tiled map
  loaded with `tilemap_load`. On `LevelBuild` the game spawns the map's tiles (owned by
  the level's root), places the player at the entry it came in by (the `spawns` layer) or
  at `player_start`, spawns the villagers the `objects` layer names as children of the
  root, and loads the map's `music` property into the level's scope and calls
  `music_play`: a half-second true crossfade, running with the screen's fade-in.
- **The player** is one entity in `.player` (outside the level, so it persists). `walk`
  (Sim) reads the `move` action and moves its feet box (10 by 5 px) an axis at a time, so
  a blocked diagonal slides: swept through the map with `tilemap_move`, then against the
  boulders, then against the villagers' feet boxes in the same tick. Exits are the `exits`
  layer's rectangles. One is armed only once the player has stood outside every exit
  since arriving, so arriving on the far side's exit does not send the player straight
  back.
- **Boulders** are `.rock`, children of the level's root, placed by the map's `rock`
  points. A boulder in the player's way slows the player to half pace and is pushed clear
  of where the player would be, if the map (swept with `tilemap_move`), the other
  boulders and the villagers leave it room; otherwise the player stops against it. A
  boulder the player already stands inside (a map edit) is walked out of, not dragged.
  They go back where the map has them when the level is entered again.
- **Hearts**: the player's `Vitals` (five hearts, sim state, so hashed and saved). A move
  a villager blocks costs one, then a second of red blinking that nothing hurts through.
  The last one faints the player back to `player_start`, whole. The bar top left (`hud`,
  PostSim) is screen-space sprites and a label in cosmetic kinds.
- **Save and load** (the `save` module). Saving writes one JSON object: the level's
  name, the player's handle, and the `.player`, `.npc` and `.rock` kinds slot for slot
  (`save_kinds`); the tiles are not saved, the map builds them. Loading closes the menu
  and holds the sim (`HoldLoading`) while the file is read, then asks the level module
  for the saved level (a reload when it is this one), whose transition keeps the hold. On
  `LevelBuild` the game restores the kinds (`load_kinds`) instead of spawning the map's
  villagers and boulders, with a remap that points their `Parent` at the new root. A
  missing or unreadable save puts the menu back with a note.
- **Drawing order**: the `objects` layer has `ysort`, so its trees and houses sort by their
  feet. The player and the villagers draw in that layer's sprite layer with
  `SpriteSort::Feet` (the feet 8 px below the center).
- **The camera** follows the player at render rate, clamped to the map, at the largest
  integer zoom that shows 240 rows, with pixel snap.
- **The menu** (`menu`, PreSim, every tick) pauses with `engine_pause` and draws text
  labels and a dimmer in cosmetic kinds. It navigates with `action_repeat`. Its sounds
  play on the Ui bus through the pause. A note under it says what saving or loading did;
  `files` (PreSim) reaps the save's write and read.
- **The credits** are `assets/credits.json`, read as a blob. A test checks that every
  author `CREDITS.md` names appears in them.
- **The maps** were first written by `tools/make_maps.py` (stdlib Python). They are plain
  Tiled JSON: open `assets/maps/*.tmj` in Tiled, edit, save, then type `level reload` into
  the CLI (`--cli-stdin`) to see the change without restarting.

## Where things are

| file | what |
|---|---|
| `src/game.cc` | the module: kinds, the menu, saving and loading, walking and pushing, villagers, the hearts, the camera, the level handlers, the demo |
| `src/main.cc` | the frame loop's config |
| `assets/maps/` | `village.tmj`, `field.tmj` and the four tilesets (`.tsj`) |
| `assets/credits.json`, `assets/CREDITS.md`, `assets/LICENSE-NinjaAdventure.txt` | who made what |
| `tools/make_maps.py` | the maps' first versions |
| `tests/credits_test.cc` | the credits name everyone |

What the game wanted and the engine did not have is in `docs/design/arcanum-notes.md`.
