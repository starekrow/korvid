# example_arcanum

A JRPG skeleton, written to find out what a game larger than one screen asks of the
engine. Two Tiled maps, a village and a field, built from the CC0 Ninja Adventure pack
by Pixel-boy and AAA (`assets/CREDITS.md`). A player walks them under keyboard or
gamepad control, collides with walls, water, trees, houses and villagers, passes in front
of and behind them by where its feet are, and walks through the road at a map's edge to
the other map. Escape or Start opens a pause menu with a level select and the credits.
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
back to the village through the menu's level select, walks into a house's wall, opens the
credits and scrolls them, reloads the level, and quits. `--demo --hash` prints the state
hash every sim tick. The hashes match across runs, thread counts (`--threads N`), I/O
backends (`--io sync`, `--io uring`) and asset sources (loose files, `--pack` with the
kpak, or `--pack` with any zip of `bin/assets`). That covers the level loads, which hold
the sim, so the disk's speed never reaches the state.

## How it is built

- **Levels** come from the level module's catalog (`village`, `field`), each a Tiled map
  loaded with `tilemap_load`. On `LevelBuild` the game spawns the map's tiles (owned by
  the level's root), places the player at the entry it came in by (the `spawns` layer) or
  at `player_start`, spawns the villagers the `objects` layer names as children of the
  root, and loads the map's `music` property into the level's scope. On `LevelEntered` it
  calls `music_play`.
- **The player** is one entity in `.player` (outside the level, so it persists). `walk`
  (Sim) reads the `move` action, sweeps its feet box (10 by 5 px) through the map with
  `tilemap_move`, then checks the villagers' feet boxes in the same tick, trying the move
  whole and then each axis. Exits are the `exits` layer's rectangles. One is armed only
  once the player has stood outside every exit since arriving, so arriving on the far
  side's exit does not send the player straight back.
- **Drawing order**: the `objects` layer has `ysort`, so its trees and houses sort by their
  feet. The player and the villagers draw in that layer's sprite layer with
  `SpriteSort::Feet` (the feet 8 px below the center).
- **The camera** follows the player at render rate, clamped to the map, at the largest
  integer zoom that shows 240 rows, with pixel snap.
- **The menu** (`menu`, PreSim, every tick) pauses with `engine_pause` and draws text
  labels and a dimmer in cosmetic kinds. It navigates with `action_repeat`. Its sounds
  play on the Ui bus through the pause.
- **The credits** are `assets/credits.json`, read as a blob. A test checks that every
  author `CREDITS.md` names appears in them.
- **The maps** were first written by `tools/make_maps.py` (stdlib Python). They are plain
  Tiled JSON: open `assets/maps/*.tmj` in Tiled, edit, save, then type `level reload` into
  the CLI (`--cli-stdin`) to see the change without restarting.

## Where things are

| file | what |
|---|---|
| `src/game.cc` | the module: kinds, the menu, walking, villagers, the camera, the level handlers, the demo |
| `src/main.cc` | the frame loop's config |
| `assets/maps/` | `village.tmj`, `field.tmj` and the four tilesets (`.tsj`) |
| `assets/credits.json`, `assets/CREDITS.md`, `assets/LICENSE-NinjaAdventure.txt` | who made what |
| `tools/make_maps.py` | the maps' first versions |
| `tests/credits_test.cc` | the credits name everyone |

What the game wanted and the engine did not have is in `docs/design/arcanum-notes.md`.
