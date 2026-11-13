#!/usr/bin/env python3
"""Cuts arcfire's boss, enemy and effect art out of the CC0 packs, replacing what
make_art.py and the old make_bosses.py drew. Run from anywhere:

    python3 examples/arcfire/tools/cut_art.py ~/code/asset-packs

The argument is the directory that holds the packs as received (assets/CREDITS.md lists
each with its size and sha256). Writes assets/generated/ (the bosses' hulls and parts, the
missile boat, Orbit's debris, segments and capital ship) and these in assets/made/: egg,
eggs, items, beam, clouds_low, clouds_high. Every texture keeps the size and cell layout
the game and the boss files expect, so nothing in the sim changes. Needs Pillow.
"""
import io
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent.parent / "assets"


# ---- reading the packs ----------------------------------------------------------------

def from_zip(path, member):
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if n.endswith(member) and "__MACOSX" not in n)
        return Image.open(io.BytesIO(z.read(name))).convert("RGBA")


def keyed(im, color):
    """The sheet's ground color made transparent."""
    im = im.copy()
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            if px[x, y][:3] == color:
                px[x, y] = (0, 0, 0, 0)
    return im


def trimmed(im, box, pad=8):
    """The largest shape in box (grown by pad), alone, cut to its bounds: a neighbour that
    reaches into the box is left out."""
    x0, y0, x1, y1 = box
    c = im.crop((max(0, x0 - pad), max(0, y0 - pad), min(im.width, x1 + pad), min(im.height, y1 + pad)))
    b = components(c, 1)[0]
    mask = Image.new("L", c.size, 0)
    mask.paste(255, b)
    out = Image.new("RGBA", c.size, (0, 0, 0, 0))
    out.paste(c, (0, 0), mask)
    return out.crop(b)


def fit(im, size, smooth=True):
    """im scaled to fit size, keeping its aspect, centred on a clear canvas."""
    r = min(size[0] / im.width, size[1] / im.height)
    w, h = max(1, round(im.width * r)), max(1, round(im.height * r))
    im = im.resize((w, h), Image.LANCZOS if smooth else Image.NEAREST)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.alpha_composite(im, ((size[0] - w) // 2, (size[1] - h) // 2))
    return out


def centred(im, size, dx=0, dy=0):
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.alpha_composite(im, ((size[0] - im.width) // 2 + dx, (size[1] - im.height) // 2 + dy))
    return out


def dimmed(im, k):
    r, g, b, a = im.split()
    return Image.merge("RGBA", (r.point(lambda v: int(v * k)), g.point(lambda v: int(v * k)), b.point(lambda v: int(v * k)), a))


def components(im, min_px=200):
    """Bounding boxes of the opaque shapes in im, largest first."""
    a = im.split()[3].point(lambda v: 255 if v > 16 else 0)
    w, h = a.size
    px = a.load()
    seen = bytearray(w * h)
    boxes = []
    for y in range(h):
        for x in range(w):
            if not px[x, y] or seen[y * w + x]:
                continue
            stack = [(x, y)]
            seen[y * w + x] = 1
            x0 = x1 = x
            y0 = y1 = y
            n = 0
            while stack:
                cx, cy = stack.pop()
                n += 1
                x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and px[nx, ny] and not seen[ny * w + nx]:
                        seen[ny * w + nx] = 1
                        stack.append((nx, ny))
            if n >= min_px:
                boxes.append(((x0, y0, x1 + 1, y1 + 1), n))
    boxes.sort(key=lambda b: -b[1])
    return [b for b, _ in boxes]


# ---- surt's modular ships: the parts, the gunships, the missile boat --------------------

# surt's sheet is four colorways of one kit: orange at the top left, blue at the top right
# (x + 288), white below the orange (y + 144); the ships he assembled from it below them.
# A piece is named by its place in the orange quadrant.
BLUE, WHITE = (288, 0), (0, 144)
PIECE = {
    "turret_twin":  (112, 136, 16, 16),  # a turret with two barrels
    "turret_one":   (144, 136, 16, 16),  # a turret with one barrel
    "turret_small": (72, 136, 8, 16),    # a small gun with two barrels
    "block":        (152, 56, 16, 16),   # a plain hull block: armored, shut
    "block_b":      (168, 56, 16, 16),
    "block_c":      (184, 56, 16, 16),
    "dome":         (64, 32, 16, 16),    # a cockpit under a dome: a core laid open
    "dome_tall":    (112, 32, 16, 16),   # a taller dome
    "dome_small":   (32, 40, 8, 8),      # a small domed pod
    "grille":       (264, 112, 16, 16),  # a frame of white struts: a bay or hatch open
    "vent":         (280, 112, 16, 16),  # a block over vents
    "thruster":     (248, 112, 16, 16),  # a block with its works showing
    "flat":         (96, 64, 16, 8),     # a flat plate: a missile bay shut
}
SHIP_BLUE = (208, 328, 256, 392)    # the long blue ship with the red dome, 48 x 64
SHIP_WHITE = (152, 336, 200, 400)   # the white ship with two pods, 48 x 64
SHIP_TALL = (208, 16, 224, 48)      # the tall orange ship, 16 x 32


def piece(sheet, name, colorway=(0, 0)):
    x, y, w, h = PIECE[name]
    return sheet.crop((x + colorway[0], y + colorway[1], x + colorway[0] + w, y + colorway[1] + h))


def parts_sheet(cols, rows, cells, flip=False):
    """A parts sheet of 24 px cells: cells maps (col, row) to a piece, centred, flipped
    top to bottom for a boss that faces down."""
    out = Image.new("RGBA", (cols * 24, rows * 24), (0, 0, 0, 0))
    for (c, r), im in cells.items():
        if flip:
            im = im.transpose(Image.FLIP_TOP_BOTTOM)
        out.alpha_composite(centred(im, (24, 24)), (c * 24, r * 24))
    return out


# ---- the bosses ---------------------------------------------------------------------------

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    packs = Path(sys.argv[1]).expanduser()
    gen, made = HERE / "generated", HERE / "made"

    surt = keyed(Image.open(packs / "modular_ships/modular_ships.png").convert("RGBA"), (0, 128, 192))
    ares_green = from_zip(packs / "arcfire/ARES sprites.zip", "ARES Spritesheet.png")
    ares_red = from_zip(packs / "arcfire/ARES sprites.zip", "ARES Spritesheet2.png")
    tip = Image.open(packs / "arcfire/tipone.png").convert("RGBA")
    remastered = packs / "arcfire/kenney_space-shooter-remastered.zip"
    extension = packs / "arcfire/kenney_spaceShooterExtension.zip"
    boulders = keyed(Image.open(packs / "shiny-boulders/boulders.png").convert("RGBA"), (32, 23, 32))
    clouds = Image.open(packs / "clouds/clouds1/clouds.png").convert("RGBA")
    p = lambda name, cw=(0, 0): piece(surt, name, cw)

    # ARES (Binngi): the capsule hull and the ring station, in green and red; rocks.
    capsule = (1200, 1212, 1576, 2008)
    ring = (68, 96, 1160, 1200)
    rocks = [(1852, 696, 1944, 804), (1752, 788, 1820, 848), (1944, 792, 2036, 888)]

    # The carrier: ARES's red capsule, bow down (the core sits at its cabin).
    hull = trimmed(ares_red, capsule).transpose(Image.FLIP_TOP_BOTTOM)
    fit(hull, (96, 176)).save(gen / "carrier.png")
    parts_sheet(4, 2, {(0, 0): p("turret_twin"), (1, 0): p("turret_twin"), (2, 0): p("block"), (3, 0): p("grille"),
                       (0, 1): p("block_b"), (1, 1): p("dome")}, flip=True).save(gen / "carrier_parts.png")

    # The battleship: ARES's green capsule, broadside, bow right.
    hull = trimmed(ares_green, capsule).rotate(-90, expand=True)
    fit(hull, (224, 96)).save(gen / "battleship.png")
    parts_sheet(4, 3, {(0, 0): p("turret_twin"), (1, 0): p("turret_one"), (2, 0): p("turret_one"), (3, 0): p("flat"),
                       (0, 1): p("vent"), (1, 1): p("block_c"), (2, 1): p("thruster"), (3, 1): p("block"),
                       (0, 2): p("dome_tall")}).save(gen / "battleship_parts.png")

    # The gunships: surt's white and blue ships, bow down; their parts from their colorways.
    for side, box, cw in (("left", SHIP_WHITE, WHITE), ("right", SHIP_BLUE, BLUE)):
        ship = surt.crop(box).transpose(Image.FLIP_TOP_BOTTOM)
        centred(ship, (48, 80)).save(gen / f"gunship_{side}.png")
        parts_sheet(2, 2, {(0, 0): p("turret_small", cw), (1, 0): p("turret_small", cw),
                           (0, 1): p("block", cw), (1, 1): p("dome", cw)}, flip=True).save(gen / f"gunship_{side}_parts.png")

    # The station: ARES's red ring, turned an eighth so its spokes run under the weapons.
    station = trimmed(ares_red, ring).rotate(45, resample=Image.BICUBIC, expand=True)
    fit(station.crop(station.getbbox()), (128, 128)).save(gen / "station.png")
    parts_sheet(4, 2, {(0, 0): p("dome_small"), (1, 0): p("block"), (2, 0): p("turret_twin"),
                       (0, 1): p("block_b"), (1, 1): p("dome")}).save(gen / "station_parts.png")

    # The missile boat: surt's tall orange ship, nose up, doubled.
    boat = surt.crop(SHIP_TALL).resize((32, 64), Image.NEAREST)
    centred(boat, (32, 72)).save(gen / "boat.png")

    # Orbit's debris: three of ARES's small rocks.
    deb = Image.new("RGBA", (72, 24), (0, 0, 0, 0))
    for i, box in enumerate(rocks):
        deb.alpha_composite(fit(trimmed(ares_green, box), (22, 22)), (i * 24 + 1, 1))
    deb.save(gen / "debris.png")

    # Orbit's segments: Kenney's station pieces, a truss with solar panels and a module.
    seg = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    seg.alpha_composite(fit(from_zip(extension, "PNG/Sprites/Station/spaceStation_022.png"), (96, 48)), (0, 0))
    seg.alpha_composite(fit(from_zip(extension, "PNG/Sprites/Station/spaceStation_001.png"), (96, 48)), (0, 48))
    seg.save(gen / "segments.png")

    # The capital ship: Commander's long ship, broadside, bow left.
    cap = trimmed(tip, (295, 27, 393, 489)).rotate(90, expand=True)
    fit(cap, (224, 40)).save(gen / "capital.png")

    # ---- the effects ---------------------------------------------------------------------

    # The egg: SpiderDave's blue boulder; the HUD's pips the same, spent (dim) and in hand.
    egg = boulders.crop((16, 0, 32, 16))
    egg.save(made / "egg.png")
    eggs = Image.new("RGBA", (32, 16), (0, 0, 0, 0))
    eggs.alpha_composite(dimmed(egg, 0.4), (0, 0))
    eggs.alpha_composite(egg, (16, 0))
    eggs.save(made / "eggs.png")

    # The items: Kenney's badges with the weapons' letters in Public Pixel, then the power
    # (Kenney's gold bolt) and the option (his silver star).
    font = ImageFont.truetype(str(HERE / "font/PublicPixel.ttf"), 8)
    items = Image.new("RGBA", (96, 16), (0, 0, 0, 0))
    for i, (letter, color) in enumerate((("S", "Red"), ("B", "Blue"), ("T", "Green"), ("L", "Yellow"))):
        badge = fit(from_zip(remastered, f"PNG/Power-ups/powerup{color}.png"), (16, 16))
        d = ImageDraw.Draw(badge)
        d.text((8, 8), letter, font=font, fill=(40, 40, 48, 255), anchor="mm")
        d.text((8, 7), letter, font=font, fill=(255, 255, 255, 255), anchor="mm")
        items.alpha_composite(badge, (i * 16, 0))
    items.alpha_composite(fit(from_zip(remastered, "PNG/Power-ups/bolt_gold.png"), (16, 16)), (64, 0))
    items.alpha_composite(fit(from_zip(remastered, "PNG/Power-ups/star_silver.png"), (16, 16)), (80, 0))
    items.save(made / "items.png")

    # The Beam's strip: Kenney's blue laser, stood in the strip's middle.
    laser = from_zip(remastered, "PNG/Lasers/laserBlue01.png").resize((12, 64), Image.LANCZOS)
    centred(laser, (16, 64)).save(made / "beam.png")

    # The clouds: Sam and 魔大农's five clouds, laid on seamless 512 px tiles (each drawn at
    # the wrap's nine offsets). Low: the clouds whole; high: smaller, stretched thin, half
    # transparent.
    shapes = [clouds.crop(b) for b in components(clouds, 4000)[:5]]

    def tile(placements, alpha):
        big = Image.new("RGBA", (1536, 1536), (0, 0, 0, 0))
        for i, x, y, scale, stretch in placements:
            s = shapes[i]
            s = s.resize((max(1, round(s.width * scale * stretch)), max(1, round(s.height * scale))), Image.LANCZOS)
            if alpha < 1:
                s.putalpha(s.split()[3].point(lambda v: int(v * alpha)))
            for ox in (0, 512, 1024):
                for oy in (0, 512, 1024):
                    big.alpha_composite(s, (x + ox, y + oy))
        return big.crop((512, 512, 1024, 1024))

    # Scaled so they cover what the drawn ones did (about 6% of the low tile, 4% of the
    # high): the field must stay readable under them.
    tile([(0, 40, 40, 0.22, 1.0), (1, 300, 150, 0.20, 1.0), (3, 110, 320, 0.26, 1.0), (2, 360, 420, 0.22, 1.0)], 1.0).save(made / "clouds_low.png")
    tile([(2, 20, 80, 0.20, 1.8), (4, 330, 20, 0.22, 1.6), (1, 200, 260, 0.18, 1.9), (3, 420, 380, 0.17, 1.7)], 0.5).save(made / "clouds_high.png")
    # Kenney's two packs' own license files, beside the credits.
    for zpath, out in ((remastered, "LICENSE-Kenney-SpaceShooter.txt"), (extension, "LICENSE-Kenney-SpaceShooterExtension.txt")):
        with zipfile.ZipFile(zpath) as z:
            name = next(n for n in z.namelist() if n.lower().endswith("license.txt"))
            (HERE / out).write_bytes(z.read(name))
    print("cut_art: generated/ and made/ written")


if __name__ == "__main__":
    main()
