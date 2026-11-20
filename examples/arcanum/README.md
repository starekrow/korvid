# arcanum

A small JRPG, written to find out what a game larger than one screen asks of the engine.
Two Tiled maps, a village and a field, built from the CC0 Ninja Adventure pack by
Pixel-boy and AAA (`assets/CREDITS.md`). It starts on a title over the village; the hero
is named in a text field and an iris opens on the game. The hero walks the maps under
keyboard or gamepad control, collides with walls, water, trees, houses and villagers,
pushes boulders, talks to villagers (a dialogue box with their faces, typed out), and
carries a hammer: at a boulder it opens an aiming minigame that can break it, and at a
villager it starts a chase at twice the hero's speed that ends at the game-over screen.
The levels remember their boulders, pushed or broken. Escape or Start opens a pause
menu: save, load, a level select and the credits. Each map has its own music, and the
change between them is a crossfade.

## Building and running

From the repository root, once, to build the build tool:

```bash
tools/kormake/bootstrap.sh
```

Then build the configuration you want and run the executable from its bin directory:

```bash
tools/kormake/bin/linux/kormake arcanum dev    # or, in this directory: kormake dev
build/linux-dev/bin/arcanum
```

The ship build packs the game's assets into `bin/arcanum.kpak`, and
`kormake package arcanum ship` makes `dist/arcanum/`, which runs
on its own.

Controls: arrows or WASD (or the left stick or the d-pad) walk; Enter, Space or South talks
and chooses; H, X or West swings the hammer; Escape or Start opens and closes the menu;
Backspace or East goes back. On the name screen the keyboard types.

`--demo` plays a script and quits. It chooses New game and types a name, pushes the
boulder up the lane, walks out to the field and back (the boulder is where it was left),
talks to the old man, hammers a boulder and breaks it on the gold mark, hears the old man
thank it, saves, hammers the pacing farmer, is chased down and killed, continues from the
save (the boulder still broken) and quits. The log says each step (`talk:`, `strike:`,
`broken:`, `quest:`, `hostile:`, `hurt:`, `dead:`, `save:`, `load:`, and the level module's
`remembers:`). `--demo --hash` prints the state hash every sim tick. The hashes match
across runs, thread counts (`--threads N`), I/O backends (`--io sync`, `--io uring`) and
asset sources (loose files, `--pack` with the kpak, or `--pack` with any zip of
`bin/assets`). That covers the level loads and the save's read, which hold the sim, so
the disk's speed never reaches the state. `--screenshot FILE --screenshot-tick N` catches
a moment of it.

The save is `~/.local/share/korvid/arcanum/save.json` (the demo's is `save-demo.json`,
so a scripted run never overwrites a game). It carries a `version`, which is 2 since the
module lost its `example_` prefix and its ktables were renamed with it. A save from a newer
version is refused, and so is an older one unless `upgrade_save` (src/game.cc) can bring
it up. For now that is a stub that brings up nothing, so version 1 saves say "The save is
from an older version".

## How it is built

- **Levels** come from the level module's catalog (`village`, `field`), each a Tiled map
  loaded with `tilemap_load`, its solids the collision module's static world
  (`tilemap_collide`). On `LevelBuild` the game spawns the map's tiles, places the player at
  the entry it came by (the `spawns` layer) or `player_start`, and spawns the villagers and
  boulders the `objects` layer names as children of the level's root, each named for its map
  object (`tilemap_object_name`) and with the object's properties in its bag (a villager's
  `display` name, `dialogue`, the old man's `quest_rocks`). The level module remembers
  `.rock` (`level_persist`): left and come back to, the boulders are where they were left.
  The characters' sheets load into each level's scope, so the level is built once their
  clips exist.
- **Bodies.** The player, the villagers and the boulders have `Collider2` bodies and move
  by intent (`Move2`): `walk` and `villagers` write intents, `collision.resolve` moves
  everything against the map and each other in the same tick, the player pushing boulders
  at their pace, and `react` reads the `MoveResult2`s: a villager that stopped the player or
  ran into it costs a heart. `villagers` reads where they stand with an early read (`^`),
  since the resolve writes it. A villager stopped on its route turns back.
- **Talking** (confirm) and **the hammer** (H) query the bodies a step ahead
  (`collision_query`). A villager talks: the `dialogue` screen with its face, its name and
  its lines from its bag, paged by `|`, typed out; the old man's `Quest` attachment picks
  his thanks once a boulder is broken. A boulder: an iris, the `aim` screen (the needle
  sweeps; on the gold mark the boulder breaks, off it the player's dice decide), an iris
  back, the boulder gone. A villager: it shouts, turns red, and chases along `tilemap_path`
  at twice the walk, a heart a touch.
- **Hearts**: the player's `Vitals` (five hearts, a second of blinking after each, and the
  hammer's dice, all sim state). The last heart fades to red and the `gameover` screen:
  continue from the save, or back to the title.
- **Screens** are one UI document, `assets/ui/arcanum.spui.json`: title, name, pause, level
  select, credits, dialogue, aim, shout, game over and the HUD's hearts (model values). The
  game opens and closes them and answers their `SpuiEvent`s by node name; modes change behind
  covers (`spui_cover`).
- **Save and load** (the `save` module). Saving writes the level's name, the hero's name,
  the boulders broken, the `.player`, `.npc` and `.rock` ktables slot for slot, and every
  level's record (`level_save`). Loading holds the sim while the file is read, gives the
  levels their records back, then asks for the saved level, and on `LevelBuild` restores
  the ktables instead of spawning the map's. Handles go by name: materials by their
  textures, clips by their sheets, the `Parent` links by `level.root`.
- **Drawing order**: the `objects` layer has `ysort`, so its trees and houses sort by their
  feet. The player and the villagers draw in that layer's sprite layer with
  `SpriteSort::Feet`.
- **The camera** follows the player at render rate, clamped to the map, at the largest
  integer zoom that shows 240 rows, with pixel snap.
- **The credits** are `assets/credits.json`, read as a blob and shown as one rich-text label
  in a scroll. A test checks that every author `CREDITS.md` names appears in them.
- **The maps** were first written by `tools/make_maps.lua` (`kormake run`; once Python). They are plain
  Tiled JSON: open `assets/maps/*.tmj` in Tiled, edit, save, then type `level reload` into
  the CLI (`--cli-stdin`) to see the change without restarting.

## Where things are

| file | what |
|---|---|
| `src/game.cc` | the module: ktables, the menus and screens, saving and loading, walking, talking and the hammer, villagers and the chase, the hearts, the camera, the level handlers, the demo |
| `assets/ui/arcanum.spui.json` | every screen and the HUD |
| `assets/actors/*.sheet.json` | the characters' walk, idle and attack clips |
| `src/main.cc` | the frame loop's config |
| `assets/maps/` | `village.tmj`, `field.tmj` and the four tilesets (`.tsj`) |
| `assets/credits.json`, `assets/CREDITS.md`, `assets/LICENSE-NinjaAdventure.txt` | who made what |
| `tools/make_maps.lua` | the maps' first versions |
| `tests/credits_test.cc` | the credits name everyone |

What the game wanted and the engine did not have is in `docs/design/arcanum-notes.md`.
