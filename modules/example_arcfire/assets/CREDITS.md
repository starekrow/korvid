# Credits for example_arcfire's assets

Every file under this directory is listed here with where it came from. Vendored files
are copied unmodified under new names by `tools/make_art.py`, which also draws everything
in `made/`. The archives were downloaded on 2026-11-09 from OpenGameArt.org and each
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

## Made here

Drawn by `tools/make_art.py` on 2026-11-09, Korvid's, under the repository's license
(Apache 2.0, `LICENSE` at the root). No image generation was used.

| file | what |
|---|---|
| `made/bullets.png` | two 32 px cells cut from M484's Bullet Collection 1 (the round bullet with the red rim at 462,193; the cyan needle at 13,66), its black ground keyed to transparent; otherwise unchanged |
| `made/dot.png` | the hitbox: a white disc |
| `made/ring.png` | the graze ring: a one-pixel white circle |
| `made/soft.png` | the shell and glows: a white disc fading to its edge |
| `made/spark.png` | sparks: a white square |
| `made/egg.png` | the egg: a spherical machine (a shaded steel sphere, its seam and core), drawn by the script |
| `made/eggs.png` | the HUD's pips: the egg, spent (dimmed) and in hand |
| `made/clouds_low.png` | the low clouds, 512 x 512 seamless: cumulus drawn by the script in the pack's whites and greys (and, darkened by the game, their shadows) |
| `made/clouds_high.png` | the high wisps, 512 x 512 seamless: tapered strokes in the same whites, half transparent |
| `made/beam.png` | the Beam's strip, 16 x 64: a white core in cyan with a deep blue edge and pulses along it |
| `made/items.png` | the items' badges, 16 px: Spread, Beam, Storm and Lance (their letters on their colors), power and option |
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

## Generated: the bosses

Kitbashed by `tools/make_bosses.py` on 2026-11-09 from Kenney's Pixel Shmup (above, CC0),
deterministically (the same files every run). Korvid's, under the repository's license
(Apache 2.0). No image generation was used. Images made another way can replace these
files without code changes, as long as each part's cell stays where its `.boss.json` says.

| file | what |
|---|---|
| `generated/carrier.png` | the carrier's hull, 96 x 176: drawn by the script in the pack's palette and style (its outline slate, greys, white and orange accent), with two copies of the pack's vent tile (`kenney/tiles.png` column 4, row 1) as deck machinery and sockets where the parts sit |
| `generated/carrier_parts.png` | its parts, 24 px cells: the pack's turret tiles (`kenney/tiles.png` column 6 row 1, column 5 row 1) as they are; launch bays shut and open, and the core armored and open, drawn by the script in the pack's palette |
| `generated/battleship.png` | the battleship's hull, 224 x 96, broadside, bow right: drawn by the script in the pack's palette and style, with the pack's vent tile twice as funnels, a bow wave and wake, and sockets where the parts sit |
| `generated/battleship_parts.png` | its parts, 24 px cells: a main gun, the pack's two turret tiles as anti-air mounts, a missile bay shut and open, a deck hatch shut and open, the bridge armored and open, the rest drawn by the script in the pack's palette |

The boss files (`bosses/*.boss.json`) are written by hand for the game, as are the patterns
`fan_storm`, `carrier_stomp`, `broadside`, `core_burst` and `wall_walk`
(`patterns/*.xml`).

`maps/shoreline.tmj` and `maps/shoreline.tsj` are written by `tools/make_maps.py`
(2026-11-09, Korvid's, Apache 2.0): a Tiled map whose tiles are all Kenney's
(`kenney/tiles.png`, Pixel Shmup, above), placed by the script from a fixed seed; the
tileset only names that image.

The stage (`stages/shoreline.stage.json`), the UI document (`ui/arcfire.ui.json`) and the
BulletML patterns (`patterns/*.xml`) are written by hand for the game. No pattern is
adapted from another source.
