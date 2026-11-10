#!/usr/bin/env python3
"""Arcfire's art: the vendored files, copied unmodified under new names, and the pieces made
here, drawn by this script. Run from anywhere:

    python3 modules/example_arcfire/tools/make_art.py ~/code/asset-packs/arcfire

The argument is the directory the archives were downloaded and unpacked into (`pack/` under
it holds one directory per archive). Writes modules/example_arcfire/assets/: the vendored
files where CREDITS.md lists them, and everything made here under `made/`. Needs Pillow.

What is made here, and from what:
  made/bullets.png   64 x 32: two 32 px cells cut from M484's Bullet Collection 1 (the
                     round bullet with the red rim, the cyan needle), its black ground
                     keyed to transparent; nothing else changed
  made/dot.png       8 x 8 white disc: the hitbox
  made/ring.png      48 x 48 white ring, 1 px: the graze ring
  made/soft.png      64 x 64 white disc fading to its edge: the shell, glows
  made/spark.png     4 x 4 white: sparks
  made/egg.png       16 x 16 the egg: a spherical machine that builds the new ship
  made/eggs.png      32 x 16 the HUD's pips: an egg spent (dim), an egg in hand
"""
import hashlib
import math
import shutil
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent.parent / "assets"

# (vendored file under the pack directory, where it goes)
VENDORED = [
    ("pack/kenney_pixelshmup/Tilemap/ships_packed.png", "kenney/ships.png"),
    ("pack/kenney_pixelshmup/Tilemap/tiles_packed.png", "kenney/tiles.png"),
    ("pack/kenney_pixelshmup/License.txt", "LICENSE-Kenney.txt"),
    ("pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Starfields/Starfield 8 - 512x512.png", "space/stars.png"),
    ("pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Purple Nebula/Purple Nebula 4 - 512x512.png", "space/nebula.png"),
    ("pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Blue Nebula/Blue Nebula 5 - 512x512.png", "space/cloud.png"),
    ("pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/License.txt", "LICENSE-ScreamingBrainStudios.txt"),
    ("pack/public_pixel_font_1_24/PublicPixel.ttf", "font/PublicPixel.ttf"),
    ("pack/public_pixel_font_1_24/License.txt", "LICENSE-CC0.txt"),
]

# Cells of M484's sheet (x, y, w, h), measured as the connected shapes on its black ground.
M484_ROUND = (462, 193, 18, 17)  # white core, red rim
M484_NEEDLE = (13, 66, 6, 10)    # cyan, upright


def keyed(im):
    """Black ground to transparent: the sheet is opaque, its shapes never pure black."""
    im = im.convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if r == 0 and g == 0 and b == 0:
                px[x, y] = (0, 0, 0, 0)
    return im


def bullets(pack):
    src = Image.open(pack / "M484BulletCollection1.png")
    sheet = Image.new("RGBA", (64, 32), (0, 0, 0, 0))
    for cell, (x, y, w, h) in enumerate([M484_ROUND, M484_NEEDLE]):
        piece = keyed(src.crop((x, y, x + w, y + h)))
        sheet.paste(piece, (cell * 32 + (32 - w) // 2, (32 - h) // 2), piece)
    return sheet


def disc(size, radius, soft=False):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = im.load()
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c)
            if soft:
                a = max(0.0, 1.0 - d / radius)
                px[x, y] = (255, 255, 255, int(255 * a * a))
            elif d <= radius:
                px[x, y] = (255, 255, 255, 255)
    return im


def ring(size):
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = im.load()
    c = (size - 1) / 2.0
    r = size / 2.0 - 1.0
    for y in range(size):
        for x in range(size):
            if abs(math.hypot(x - c, y - c) - r) < 0.6:
                px[x, y] = (255, 255, 255, 255)
    return im


# The egg is a machine, not a chicken's: a steel sphere, lit from the upper left, banded at
# its seam (where it opens), with a cyan core in the seam that builds the ship. Drawn to
# Kenney's palette: a dark outline, four steel tones, the core's two cyans.
STEEL = [(46, 52, 72), (82, 92, 118), (130, 142, 170), (196, 206, 226)]
OUTLINE = (24, 26, 38)
SEAM = (34, 38, 54)
CORE = [(60, 170, 220), (150, 240, 255), (235, 255, 255)]


def egg():
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = im.load()
    c, r = 7.5, 7.0
    light = (-0.5, -0.6, 0.62)
    for y in range(16):
        for x in range(16):
            dx, dy = (x - c) / r, (y - c) / r
            d2 = dx * dx + dy * dy
            if d2 > 1.0:
                continue
            if d2 > 0.78:
                px[x, y] = OUTLINE + (255,)
                continue
            nz = math.sqrt(1.0 - d2)
            lit = max(0.0, dx * light[0] + dy * light[1] + nz * light[2])
            tone = STEEL[min(3, int(lit * 4.2))]
            if y in (7, 8) and abs(x - c) > 2.5:
                tone = SEAM if y == 8 else STEEL[0]  # the seam: a groove under a dark lip
            px[x, y] = tone + (255,)
    # The core, in the seam: bright at its middle, cyan to its edge.
    for x, y, k in [(6, 7, 1), (7, 7, 2), (8, 7, 2), (9, 7, 1), (6, 8, 0), (7, 8, 1), (8, 8, 1), (9, 8, 0)]:
        px[x, y] = CORE[k] + (255,)
    # Two rivets either side of the seam and a glint.
    for x, y in [(3, 5), (12, 5), (3, 10), (12, 10)]:
        px[x, y] = STEEL[0] + (255,)
    px[5, 3] = (236, 242, 255, 255)
    return im


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pack = Path(sys.argv[1]).expanduser()
    for src, dst in VENDORED:
        out = HERE / dst
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(pack / src, out)
    made = HERE / "made"
    made.mkdir(parents=True, exist_ok=True)
    bullets(pack).save(made / "bullets.png")
    disc(8, 3.6).save(made / "dot.png")
    ring(48).save(made / "ring.png")
    disc(64, 31.5, soft=True).save(made / "soft.png")
    Image.new("RGBA", (4, 4), (255, 255, 255, 255)).save(made / "spark.png")
    egg().save(made / "egg.png")
    pips = Image.new("RGBA", (32, 16), (0, 0, 0, 0))
    spent = egg()
    px = spent.load()
    for y in range(16):
        for x in range(16):
            r, g, b, a = px[x, y]
            px[x, y] = (r // 3, g // 3, b // 3 + 20, a // 2)
    pips.paste(spent, (0, 0))
    pips.paste(egg(), (16, 0))
    pips.save(made / "eggs.png")
    for archive in sorted(pack.glob("*.zip")) + sorted(pack.glob("*.png")):
        print(f"{archive.name}: sha256 {hashlib.sha256(archive.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
