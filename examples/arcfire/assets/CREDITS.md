# Credits for arcfire's assets

Every asset taken from a pack is listed here with where it came from. Vendored files are
copied unmodified under new names by `tools/make_art.py`, which also cuts the pieces of
`made/` listed below; `tools/cut_art.py` cuts the bosses and the rest from the newer packs
(below). The archives were downloaded on 2026-11-09 from OpenGameArt.org and each
license was read from the archive's own file (or, where it has none, from the page). The
unpacked archives are kept at `~/code/asset-packs/arcfire` on the development machine.

## Pixel Shmup

By **Kenney** (https://www.kenney.nl). Released under **CC0 1.0 Universal**; the pack's
own license file is `LICENSE-Kenney.txt` beside this file, the CC0 legal code
`LICENSE-CC0.txt`. Credit is not required; it is given gladly.

- Pack page: https://kenney.nl/assets/pixel-shmup (version 1.0, 2021)
- Downloaded as `kenney_pixelshmup.zip` from
  https://opengameart.org/sites/default/files/kenney_pixelshmup.zip (117,687 bytes,
  sha256 32c6b991160510e94d2f59c061932d96302c143eed53d0b135e9d249176418f9)

| here | in the archive |
|---|---|
| `kenney/ships.png` | `Tilemap/ships_packed.png` |
| `kenney/tiles.png` | `Tilemap/tiles_packed.png` |
| `LICENSE-Kenney.txt` | `License.txt` |

## Seamless Space Backgrounds

By **Screaming Brain Studios** (https://screamingbrainstudios.itch.io). Released under
**CC0 1.0 Universal** ("All Screaming Brain Studios assets have been released under the
CC0/Public Domain License"); the pack's license file is `LICENSE-ScreamingBrainStudios.txt`.

- Pack page: https://screamingbrainstudios.itch.io/seamless-space-backgrounds, mirrored at
  https://opengameart.org/content/seamless-space-backgrounds
- Downloaded as `sbs_-_seamless_space_backgrounds_-_small_512x512_0.zip` from
  OpenGameArt (7,872,632 bytes, sha256
  401a73e243bc0c21df662577e1fb477ff2ac90ff01dae0050506fc0db859e0ab)

| here | in the archive |
|---|---|
| `space/stars.png` | `Small 512x512/Starfields/Starfield 8 - 512x512.png` |
| `space/nebula.png` | `Small 512x512/Purple Nebula/Purple Nebula 4 - 512x512.png` |
| `space/cloud.png` | `Small 512x512/Blue Nebula/Blue Nebula 5 - 512x512.png` |
| `LICENSE-ScreamingBrainStudios.txt` | `License.txt` |

## Bullet Collection 1 (M484)

By **Master484** (M484 Games). Released into the public domain as **CC0**: the page says
"These graphics are in the Public Domain. Attribution is not needed." The download is
one image with no license file of its own; `LICENSE-CC0.txt` is the CC0 legal code.

- Page: https://opengameart.org/content/bullet-collection-1-m484
- Downloaded as `M484BulletCollection1.png` from
  https://opengameart.org/sites/default/files/M484BulletCollection1.png (22,490 bytes,
  sha256 52f52ce30f2b932184bce2df03aea9f9d895090707e73730d22cdbc1e1a99124)

Nothing of it is vendored as it is: `made/bullets.png` is cut from it (below).

## Public Pixel

By **GGBotNet** (https://ggbot.itch.io). Released under **CC0 1.0 Universal**; the
archive's `License.txt` is the CC0 legal code, kept here as `LICENSE-CC0.txt`.

- Font page: https://ggbot.itch.io/public-pixel-font (version 1.24), mirrored at
  https://opengameart.org/content/public-pixel-font
- Downloaded as `public_pixel_font_1_24.zip` from
  https://opengameart.org/sites/default/files/public_pixel_font_1_24.zip (483,597 bytes,
  sha256 6643467db8d15eea7c89b6612d3abc5e160fe02705806f47a4073c350dfb5c78)

| here | in the archive |
|---|---|
| `font/PublicPixel.ttf` | `PublicPixel.ttf` |
| `LICENSE-CC0.txt` | `License.txt` |

## Cut from the packs above

By `tools/make_art.py` on 2026-11-09.

| file | what |
|---|---|
| `made/bullets.png` | two 32 px cells cut from M484's Bullet Collection 1 (the round bullet with the red rim at 462,193; the cyan needle at 13,66), its black ground keyed to transparent; otherwise unchanged |
| `made/cursor.png` | the menus' cursor: Kenney's player ship (Pixel Shmup `Tilemap/ships_packed.png`, column 0, row 0) turned to point right |
| `made/bombs.png` | the HUD's bomb pips: Kenney's shield tile (`kenney/tiles.png`, column 2, row 2), dimmed for a bomb spent and as it is for one in hand |

## Sci-Fi Sounds and Impact Sounds

By **Kenney** (https://www.kenney.nl). Released under **CC0 1.0 Universal**; each archive's
license file is beside this file as `LICENSE-Kenney-SciFiSounds.txt` and
`LICENSE-Kenney-ImpactSounds.txt`. Copied unmodified under new names by
`tools/make_sounds.py`. Downloaded on 2026-11-09.

- Sci-Fi Sounds 1.0: https://kenney.nl/assets/sci-fi-sounds, mirrored at
  https://opengameart.org/content/sci-fi-sounds; downloaded as `sci-fi_sounds.zip` from
  https://opengameart.org/sites/default/files/sci-fi_sounds.zip (5,875,104 bytes, sha256
  119340f351a5098ad814f78719438c0da355a9ce8a4c8a3af6a8d48aa3d49e04)
- Impact Sounds 1.0: https://kenney.nl/assets/impact-sounds; downloaded as
  `kenney_impact-sounds.zip` from that page (800,850 bytes, sha256
  029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8)

| here | in the archive |
|---|---|
| `sound/shot.ogg` | Sci-Fi Sounds `Audio/laserSmall_000.ogg` |
| `sound/hit.ogg` | Impact Sounds `Audio/impactPlate_light_000.ogg` |
| `sound/ping.ogg` | Impact Sounds `Audio/impactMetal_light_001.ogg` |
| `sound/kill.ogg` | Sci-Fi Sounds `Audio/explosionCrunch_000.ogg` |
| `sound/kill_big.ogg` | Sci-Fi Sounds `Audio/explosionCrunch_002.ogg` |
| `sound/boss_down.ogg` | Sci-Fi Sounds `Audio/explosionCrunch_004.ogg` |
| `sound/rumble.ogg` | Sci-Fi Sounds `Audio/lowFrequency_explosion_000.ogg` |
| `sound/graze.ogg` | Sci-Fi Sounds `Audio/laserRetro_000.ogg` |
| `sound/chip.ogg` | Sci-Fi Sounds `Audio/laserRetro_002.ogg` |
| `sound/bomb.ogg` | Sci-Fi Sounds `Audio/forceField_000.ogg` |
| `sound/egg_break.ogg` | Sci-Fi Sounds `Audio/lowFrequency_explosion_001.ogg` |
| `sound/egg_open.ogg` | Impact Sounds `Audio/impactMetal_heavy_000.ogg` |
| `sound/egg_bloom.ogg` | Sci-Fi Sounds `Audio/forceField_002.ogg` |
| `sound/warning.ogg` | Sci-Fi Sounds `Audio/computerNoise_000.ogg` |
| `sound/ui_move.ogg` | Sci-Fi Sounds `Audio/laserRetro_001.ogg` |
| `sound/ui_select.ogg` | Sci-Fi Sounds `Audio/forceField_001.ogg` |
| `sound/ui_cancel.ogg` | Sci-Fi Sounds `Audio/doorClose_001.ogg` |
| `LICENSE-Kenney-SciFiSounds.txt`, `LICENSE-Kenney-ImpactSounds.txt` | each archive's `License.txt` |

## 5 Chiptunes (Action)

By **Juhani Junkala** (SubspaceAudio). Released under **CC0**: the page lists CC0 and the
archive's `INFO.txt` (beside this file as `LICENSE-JuhaniJunkala.txt`) says "released under
CC0 creative commons license".

- Page: https://opengameart.org/content/5-chiptunes-action
- Downloaded on 2026-11-09 as `5 Action Chiptunes By Juhani Junkala.zip` from
  https://opengameart.org/sites/default/files/5%20Action%20Chiptunes%20By%20Juhani%20Junkala.zip
  (49,639,688 bytes, sha256 ea189990c0fb9a700187ccc740eda72400341a082305a163867467d96346460d)

| here | in the archive |
|---|---|
| `music/stage.wav` | `Juhani Junkala [Retro Game Music Pack] Level 1.wav`, folded to mono and halved in rate (22,050 Hz) by `tools/make_sounds.py`; otherwise unchanged |
| `music/boss.wav` | `Juhani Junkala [Retro Game Music Pack] Level 3.wav`, likewise |
| `music/orbit.wav` | `Juhani Junkala [Retro Game Music Pack] Level 2.wav`, likewise (Orbit's stage) |
| `music/orbit_boss.wav` | `Juhani Junkala [Retro Game Music Pack] Ending.wav`, likewise (Orbit's bosses) |

## Modular ships

By **surt**. Released under **CC0 1.0 Universal** (`LICENSE-CC0.txt`). Received as
`modular_ships.png` (640 x 480, 23,190 bytes, sha256
7dc502b0b46e2f222fef8eaa10bc33a2982508d57baddf82f737889ec7cafdcc): a kit of ship parts in
four colorways on a solid blue ground, and ships assembled from it.

## ARES sprites

By **Binngi**. Released under **CC0 1.0 Universal** (`LICENSE-CC0.txt`). Received as
`ARES sprites.zip` (14,810,557 bytes, sha256
30901455eb763c55189c445b1aa20e94d53a778f006010e2b222a8b85162c889); used:
`ARES Spritesheet.png` (green) and `ARES Spritesheet2.png` (red).

## Ships (Commander)

By **Commander**. Released under **CC0 1.0 Universal** (`LICENSE-CC0.txt`). Received as
`tipone.png` (900 x 1100, 836,671 bytes, sha256
30b44757f31a8889e2d5d256b82c0dcc046d236a54cc77c6785ae0d1ac147127), one of three
colorings of the sheet.

## Space Shooter (Remastered) and Space Shooter Extension

By **Kenney** (https://www.kenney.nl). Released under **CC0 1.0 Universal**; the packs'
own license files are `LICENSE-Kenney-SpaceShooter.txt` and
`LICENSE-Kenney-SpaceShooterExtension.txt`. Received as
`kenney_space-shooter-remastered.zip` (1,108,777 bytes, sha256
0edbe0ab5cda6c44901d8c42f150268fdfa0c8d48492098669f37e9c296929b5) and
`kenney_spaceShooterExtension.zip` (2,840,182 bytes, sha256
77efd54b343bbd123af8cc7f534446706731b9cf5a34f38d8fcc54e7245f9c91).

## Shiny boulders

By **SpiderDave**. Released under **CC0 1.0 Universal** (`LICENSE-CC0.txt`). Received as
`boulders.png` (1024 x 768, 510,541 bytes, sha256
4de06744276069a8892b7988b468273ab35d1f44777b13fba5ce0516d29679d1), 16 px boulders on a
solid ground.

## Clouds

By **Sam** and **魔大农**. Released under **CC0 1.0 Universal** (`LICENSE-CC0.txt`).
Received as `clouds.png` (1024 x 1024, 437,221 bytes, sha256
94859924c791878bd615fc6cd15db0d76c648047453d4a8c4a2c67be12bb8673), five clouds.

The packs above were received on 2026-11-12 and are kept under `~/code/asset-packs` on the
development machine; their source pages are not recorded here yet.

## Cut from the packs: the bosses and the rest

By `tools/cut_art.py` (2026-11-12), deterministically, from the packs as received. Every
texture keeps the size and cell layout the game and the `.boss.json` files expect. The
parts are pieces of surt's kit (each 24 px cell holds one piece, centred; flipped top to
bottom on a boss that faces down); `tools/cut_art.py` names each piece by its place on the
sheet.

| file | from |
|---|---|
| `generated/carrier.png` | ARES's red capsule hull, turned bow down (its cabin where the core sits), scaled into 96 x 176 |
| `generated/carrier_parts.png` | surt: the twin-barrelled turret (both turret cells), a plain block (bay shut), a strut frame (bay open), a block (core armored), a domed cockpit (core open) |
| `generated/battleship.png` | ARES's green capsule hull, broadside, bow right, scaled into 224 x 96 |
| `generated/battleship_parts.png` | surt: the twin turret (main guns), the single-barrelled turret (anti-air), a flat plate and a vent block (missile bay shut and open), a block and an open thruster block (hatches armored and open), a block and the tall dome (bridge armored and open) |
| `generated/gunship_left.png`, `generated/gunship_left_parts.png` | surt's white ship (two pods), turned bow down and set in 48 x 80; its parts from the white colorway: the small twin gun, a block (gun armored), a domed cockpit (gun open) |
| `generated/gunship_right.png`, `generated/gunship_right_parts.png` | surt's long blue ship (red dome), likewise, its parts from the blue colorway |
| `generated/station.png` | ARES's red ring station, turned an eighth so its spokes run under the weapon mounts, scaled into 128 x 128 |
| `generated/station_parts.png` | surt: a small domed pod (emitters), a block and the twin turret (weapons armored and open), a block and a domed cockpit (core armored and open) |
| `generated/boat.png` | surt's tall orange ship, doubled (nearest neighbour), nose up, in 32 x 72 |
| `generated/debris.png` | three of ARES's small rocks, scaled into 24 px cells |
| `generated/segments.png` | Kenney's Extension `PNG/Sprites/Station/spaceStation_022.png` (a hub between solar panels: the truss) and `spaceStation_001.png` (a module with a window band), each scaled into a 96 x 48 cell |
| `generated/capital.png` | Commander's long ship (the sheet's second shape, 98 x 462 at 295,27), broadside, bow left, scaled into 224 x 40 |
| `made/egg.png` | SpiderDave's boulder at column 1, row 0, its ground made transparent |
| `made/eggs.png` | the HUD's pips: the same boulder, dimmed (spent) and as it is (in hand) |
| `made/items.png` | Kenney's `PNG/Power-ups/powerupRed.png`, `powerupBlue.png`, `powerupGreen.png` and `powerupYellow.png` scaled to 16 px with the weapons' letters S, B, T and L over them in Public Pixel (above); then `bolt_gold.png` (power) and `star_silver.png` (option) |
| `made/beam.png` | Kenney's `PNG/Lasers/laserBlue01.png`, scaled to 12 x 64 in the strip's middle |
| `made/clouds_low.png` | Sam and 魔大农's clouds, four of the five scaled to about a fifth and laid on a seamless 512 px tile (the game darkens a copy for their shadows) |
| `made/clouds_high.png` | the same clouds smaller, stretched thin and half transparent, on a seamless 512 px tile |

The boss files (`bosses/*.boss.json`) are written by hand for the game, as are the patterns
`fan_storm`, `carrier_stomp`, `broadside`, `core_burst` and `wall_walk`, and Orbit's
`tight_ring`, `cross_spiral`, `curtain`, `aimed_walls` and `core_finale`
(`patterns/*.xml`). The station core's file was laid out by a few lines of Python (its parts
on circles); it is the file that is the source.

`maps/shoreline.tmj` and `maps/shoreline.tsj` are written by `tools/make_maps.py`
(2026-11-09, Korvid's, Apache 2.0): a Tiled map whose tiles are all Kenney's
(`kenney/tiles.png`, Pixel Shmup, above), placed by the script from a fixed seed; the
tileset only names that image.

The stages (`stages/shoreline.stage.json`, `stages/orbit.stage.json`), the UI document (`ui/arcfire.ui.json`) and the
BulletML patterns (`patterns/*.xml`) are written by hand for the game. No pattern is
adapted from another source.

The shaders (`shaders/*.slang`, outside `assets/`: the nebula, the dissolve, their value
noise, and the shimmer) and the materials that name them (`materials/*.material.json`:
the nebula, each boss's dissolve, the capital ship's) are written for the game
(2026-11-10, Korvid's, Apache 2.0). The noise is a plain value noise summed over
octaves, its hash the common fract-and-dot idiom; no shader is adapted from another source.
