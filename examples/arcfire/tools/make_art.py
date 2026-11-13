#!/usr/bin/env python3
"""Arcfire's art: the vendored files, copied unmodified under new names, and the pieces made
here: cuts from the packs and plain shapes. The bosses, the egg, the clouds, the beam and
the items come from the newer packs through tools/cut_art.py. Run from anywhere:

    python3 examples/arcfire/tools/make_art.py ~/code/asset-packs/arcfire

The argument is the directory the archives were downloaded and unpacked into (`pack/` under
it holds one directory per archive). Writes examples/arcfire/assets/: the vendored
files where CREDITS.md lists them, and everything made here under `made/`. Needs Pillow.

What is made here, and from what:
  made/bullets.png   64 x 32: two 32 px cells cut from M484's Bullet Collection 1 (the
                     round bullet with the red rim, the cyan needle), its black ground
                     keyed to transparent; nothing else changed
  made/dot.png       8 x 8 white disc: the hitbox
  made/ring.png      48 x 48 white ring, 1 px: the graze ring
  made/soft.png      64 x 64 white disc fading to its edge: the shell, glows
  made/spark.png     4 x 4 white: sparks
  made/bombs.png     32 x 16 the HUD's bomb pips: Kenney's shield tile (tiles_packed, column
                     2, row 2), dimmed for a bomb spent, as it is for one in hand
  made/vignette.png  135 x 180: the field's vignette, clear in the middle and darkening to
                     its edges and corners (drawn 4 times up, over the field, under the HUD)
  made/cursor.png    32 x 32: the menus' cursor, Kenney's player ship (ships_packed, column
                     0, row 0) turned to point right; nothing else changed
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


def vignette(w=135, h=180):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    for y in range(h):
        for x in range(w):
            dx, dy = (x + 0.5) / w * 2.0 - 1.0, (y + 0.5) / h * 2.0 - 1.0
            r = math.sqrt(dx * dx * 0.85 + dy * dy)  # an ellipse a little wider than the field
            a = max(0.0, min(1.0, (r - 0.62) / 0.6)) ** 1.6
            px[x, y] = (4, 6, 16, int(round(a * 150)))
    return im


def cursor(pack):
    ships = Image.open(pack / "pack/kenney_pixelshmup/Tilemap/ships_packed.png").convert("RGBA")
    return ships.crop((0, 0, 32, 32)).transpose(Image.Transpose.ROTATE_270)


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
    cursor(pack).save(made / "cursor.png")
    vignette().save(made / "vignette.png")
    tiles = Image.open(pack / "pack/kenney_pixelshmup/Tilemap/tiles_packed.png").convert("RGBA")
    shield = tiles.crop((32, 32, 48, 48))
    dim = shield.copy()
    px = dim.load()
    for y in range(16):
        for x in range(16):
            r, g, b, a = px[x, y]
            px[x, y] = (r // 3, g // 3, b // 3 + 20, a // 2)
    bombs = Image.new("RGBA", (32, 16), (0, 0, 0, 0))
    bombs.paste(dim, (0, 0))
    bombs.paste(shield, (16, 0))
    bombs.save(made / "bombs.png")
    for archive in sorted(pack.glob("*.zip")) + sorted(pack.glob("*.png")):
        print(f"{archive.name}: sha256 {hashlib.sha256(archive.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
