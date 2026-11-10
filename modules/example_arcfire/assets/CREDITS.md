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

The stage (`stages/shoreline.stage.json`) and the UI document (`ui/arcfire.ui.json`) are
written by hand for the game.
