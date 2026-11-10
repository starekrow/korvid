#!/usr/bin/env python3
"""Arcfire's bosses, kitbashed from Kenney's Pixel Shmup (assets/kenney/, CC0) into
assets/generated/. Deterministic: the same files every run. Run from anywhere:

    python3 modules/example_arcfire/tools/make_bosses.py

Each boss is two images, at the pack's pixel scale (drawn at 2x, as the pack's ships are):

  generated/<boss>.png        the hull, bow down (toward the player), with sockets where
                              its parts sit, so a part shot away leaves its scar
  generated/<boss>_parts.png  a sheet of 24 px cells, the parts in their states (a
                              turret; a bay shut and open; a core armored and open);
                              the boss's .boss.json names each part's cell

What is drawn here and what is the pack's: the hulls are drawn by this script in the
pack's palette (its outline slate, its two greys, its white, its orange accent) and its
style (a two-pixel outline, flat fills, a highlight edge); the turrets are the pack's own
turret tiles (tiles_packed.png column 6 row 1, column 5 row 1), which face down as they are; the
deck machinery is its vent tile (column 4 row 1). Images made elsewhere can replace these
files as long as the cells stay where the .boss.json says. Needs Pillow.
"""
from pathlib import Path

from PIL import Image, ImageDraw

ASSETS = Path(__file__).resolve().parent.parent / "assets"
OUT = ASSETS / "generated"

# Kenney's palette, sampled from the sheets.
OUTLINE = (67, 74, 95)
GREY = (149, 154, 177)
LIGHT = (220, 225, 231)
WHITE = (255, 255, 255)
ORANGE = (252, 104, 59)
ORANGE_DARK = (222, 75, 30)
YELLOW = (255, 189, 32)
YELLOW_DARK = (226, 161, 6)
CYAN = (44, 197, 246)
CYAN_DARK = (17, 165, 212)
DEEP = (40, 44, 60)  # a socket's floor: the outline, darker (the one tone not in the pack)

CELL = 24


def tile(sheet, col, row):
    return sheet.crop((col * 16, row * 16, col * 16 + 16, row * 16 + 16))


def outline_fill(draw, poly, fill, width=2):
    draw.polygon(poly, fill=OUTLINE)
    # Inset by the outline's width: the polygons here are convex enough to shrink toward
    # their centroid without crossing.
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)
    inner = []
    for x, y in poly:
        dx, dy = x - cx, y - cy
        ix = x - width if dx > 0.5 else x + width if dx < -0.5 else x
        iy = y - width if dy > 0.5 else y + width if dy < -0.5 else y
        inner.append((ix, iy))
    draw.polygon(inner, fill=fill)


def socket(draw, cx, cy, r):
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=OUTLINE)
    draw.ellipse((cx - r + 2, cy - r + 2, cx + r - 2, cy + r - 2), fill=DEEP)


def carrier(sheet):
    """96 x 176: a flying carrier, bow down. Sponsons either side carry four turrets and
    two launch bays; the core sits in the bow under armor."""
    w, h = 96, 176
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Engines at the stern (the top): three nozzles, lit.
    for x in (34, 45, 56):
        d.rectangle((x, 0, x + 6, 8), fill=OUTLINE)
        d.rectangle((x + 2, 1, x + 4, 4), fill=CYAN)
    # The sponsons, then the hull over them.
    outline_fill(d, [(14, 40), (82, 40), (92, 52), (92, 116), (82, 128), (14, 128), (4, 116), (4, 52)], GREY)
    outline_fill(d, [(28, 6), (68, 6), (74, 20), (74, 148), (58, 174), (38, 174), (22, 148), (22, 20)], GREY)
    # Orange bands along the sponsons' outer edges, the pack's enemy colour.
    for x0, x1 in ((6, 9), (87, 90)):
        d.rectangle((x0, 54, x1, 114), fill=ORANGE)
        d.line((x0, 114, x1, 114), fill=ORANGE_DARK)
    # The flight deck: a light strip down the middle, a dashed white centre line.
    d.rectangle((40, 12, 56, 150), fill=LIGHT)
    d.line((40, 12, 40, 150), fill=WHITE)
    for y in range(18, 146, 10):
        d.rectangle((47, y, 49, y + 4), fill=WHITE)
    # Panel seams across the sponsons and the hull's flanks.
    for y in (60, 84, 108):
        d.line((8, y, 38, y), fill=OUTLINE)
        d.line((58, y, 88, y), fill=OUTLINE)
    for y in (30, 136):
        d.line((26, y, 38, y), fill=OUTLINE)
        d.line((58, y, 70, y), fill=OUTLINE)
    # The bridge on the starboard flank, its windows lit.
    d.rectangle((60, 18, 70, 34), fill=OUTLINE)
    d.rectangle((62, 20, 68, 32), fill=LIGHT)
    for y in (22, 26, 30):
        d.line((63, y, 67, y), fill=CYAN_DARK)
    # Deck machinery: the pack's vent tile, twice, aft.
    vent = tile(sheet, 4, 1)
    im.alpha_composite(vent, (24, 10))
    im.alpha_composite(vent.transpose(Image.FLIP_LEFT_RIGHT), (24, 120))
    # Sockets where the parts sit (the part covers its socket; shot away, the socket shows).
    for cx, cy in ((16, 52), (80, 52), (16, 108), (80, 108)):  # turrets
        socket(d, cx, cy, 7)
    for cx in (30, 66):  # bays
        d.rectangle((cx - 10, 72, cx + 10, 88), fill=OUTLINE)
        d.rectangle((cx - 8, 74, cx + 8, 86), fill=DEEP)
    socket(d, 48, 140, 11)  # the core
    return im


def turret(sheet, col, row):
    return tile(sheet, col, row)


def bay(open_):
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((1, 4, 22, 19), fill=OUTLINE)
    if open_:
        d.rectangle((3, 6, 20, 17), fill=DEEP)
        d.rectangle((3, 6, 4, 17), fill=GREY)   # the doors, slid back
        d.rectangle((19, 6, 20, 17), fill=GREY)
        d.rectangle((10, 10, 13, 13), fill=CYAN)
        d.point((11, 11), fill=WHITE)
    else:
        d.rectangle((3, 6, 20, 17), fill=GREY)
        d.line((11, 6, 11, 17), fill=OUTLINE)    # the doors' seam
        d.line((12, 6, 12, 17), fill=LIGHT)
        for x in range(3, 21, 4):               # hazard edge
            d.rectangle((x, 15, x + 1, 17), fill=YELLOW)
            d.rectangle((x + 2, 15, x + 3, 17), fill=YELLOW_DARK)
    return im


def core(open_):
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((1, 1, 22, 22), fill=OUTLINE)
    if open_:
        d.ellipse((4, 4, 19, 19), fill=CYAN_DARK)
        d.ellipse((6, 6, 17, 17), fill=CYAN)
        d.ellipse((9, 9, 14, 14), fill=WHITE)
        for x, y in ((4, 11), (19, 11), (11, 4), (11, 19)):
            d.point((x, y), fill=LIGHT)
    else:
        d.ellipse((3, 3, 20, 20), fill=GREY)
        d.line((3, 11, 20, 11), fill=OUTLINE)    # the shutters' seams
        d.line((11, 3, 11, 20), fill=OUTLINE)
        d.line((4, 10, 10, 4), fill=LIGHT)
        d.point((11, 11), fill=CYAN)            # a glint of what is inside
    return im


def carrier_parts(sheet):
    """4 x 2 cells of 24 px: (0,0) turret, (1,0) twin turret, (2,0) bay shut, (3,0) bay
    open, (0,1) core armored, (1,1) core open."""
    im = Image.new("RGBA", (CELL * 4, CELL * 2), (0, 0, 0, 0))
    im.alpha_composite(turret(sheet, 6, 1), (4, 4))
    im.alpha_composite(turret(sheet, 5, 1), (CELL + 4, 4))
    im.alpha_composite(bay(False), (CELL * 2, 0))
    im.alpha_composite(bay(True), (CELL * 3, 0))
    im.alpha_composite(core(False), (0, CELL))
    im.alpha_composite(core(True), (CELL, CELL))
    return im


def battleship(sheet):
    """224 x 96: a battleship broadside to the player, bow right. Two main guns on the
    centre line, four anti-air mounts at its corners, a missile bay aft of the bridge, two
    deck hatches between the guns and the bridge, the bridge (its core) amidships."""
    w, h = 224, 96
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # A bow wave and a wake on the water, the pack's white.
    for x, y in ((218, 44), (220, 48), (218, 52), (2, 40), (0, 46), (2, 56), (4, 50)):
        d.rectangle((x - 2, y - 1, x + 2, y + 1), fill=WHITE)
    outline_fill(d, [(30, 12), (188, 12), (218, 48), (188, 84), (30, 84), (8, 72), (8, 24)], GREY)
    # The waterline: an orange band along both sides, the pack's enemy colour.
    d.rectangle((30, 14, 186, 17), fill=ORANGE)
    d.rectangle((30, 79, 186, 82), fill=ORANGE)
    d.line((30, 17, 186, 17), fill=ORANGE_DARK)
    d.line((30, 82, 186, 82), fill=ORANGE_DARK)
    # The deck: a light walkway down the middle, planks across it.
    d.rectangle((20, 42, 200, 54), fill=LIGHT)
    for x in range(26, 198, 8):
        d.line((x, 42, x, 54), fill=GREY)
    # Panel seams across the hull.
    for x in (52, 76, 100, 124, 148, 172):
        d.line((x, 20, x, 38), fill=OUTLINE)
        d.line((x, 58, x, 76), fill=OUTLINE)
    # Funnels aft: the pack's vent tile, twice.
    vent = tile(sheet, 4, 1)
    im.alpha_composite(vent, (14, 24))
    im.alpha_composite(vent.transpose(Image.FLIP_TOP_BOTTOM), (14, 56))
    # Sockets: main guns, anti-air mounts, the missile bay, the deck hatches, the bridge.
    for cx in (64, 160):
        socket(d, cx, 48, 10)
    for cx, cy in ((40, 28), (184, 28), (40, 68), (184, 68)):
        socket(d, cx, cy, 7)
    d.rectangle((102, 62, 122, 78), fill=OUTLINE)
    d.rectangle((104, 64, 120, 76), fill=DEEP)
    for cx in (88, 136):
        d.rectangle((cx - 9, 39, cx + 9, 57), fill=OUTLINE)
        d.rectangle((cx - 7, 41, cx + 7, 55), fill=DEEP)
    d.rectangle((99, 19, 125, 41), fill=OUTLINE)
    d.rectangle((101, 21, 123, 39), fill=DEEP)
    return im


def main_gun():
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for x in (8, 13):  # two barrels, down the screen
        d.rectangle((x, 12, x + 3, 23), fill=OUTLINE)
        d.rectangle((x + 1, 13, x + 2, 22), fill=LIGHT)
    d.ellipse((2, 1, 21, 18), fill=OUTLINE)
    d.ellipse((4, 3, 19, 16), fill=GREY)
    d.rectangle((6, 6, 17, 13), fill=ORANGE)
    d.line((6, 13, 17, 13), fill=ORANGE_DARK)
    d.line((5, 7, 9, 3), fill=LIGHT)
    return im


def missile_bay(open_):
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((3, 5, 20, 18), fill=OUTLINE)
    if open_:
        d.rectangle((5, 7, 18, 16), fill=DEEP)
        for x in (7, 11, 15):  # three missiles' heads
            d.rectangle((x, 9, x + 1, 14), fill=WHITE)
            d.point((x, 9), fill=ORANGE)
            d.point((x + 1, 9), fill=ORANGE)
    else:
        d.rectangle((5, 7, 18, 16), fill=GREY)
        for y in (9, 12, 15):
            d.line((5, y, 18, y), fill=OUTLINE)
        d.line((5, 7, 18, 7), fill=LIGHT)
    return im


def hatch(open_):
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((3, 3, 20, 20), fill=OUTLINE)
    if open_:
        d.rectangle((5, 5, 18, 18), fill=DEEP)
        d.ellipse((7, 7, 16, 16), fill=CYAN_DARK)  # a gun come up through the deck
        d.rectangle((10, 12, 13, 20), fill=GREY)
        d.ellipse((9, 9, 14, 14), fill=CYAN)
    else:
        d.rectangle((5, 5, 18, 18), fill=GREY)
        d.line((5, 5, 18, 18), fill=OUTLINE)
        d.line((18, 5, 5, 18), fill=OUTLINE)
        d.line((5, 5, 18, 5), fill=LIGHT)
    return im


def bridge(open_):
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((1, 2, 22, 21), fill=OUTLINE)
    d.rectangle((3, 4, 20, 19), fill=GREY)
    d.rectangle((3, 4, 20, 5), fill=LIGHT)
    if open_:
        d.rectangle((5, 8, 18, 15), fill=CYAN_DARK)  # the core behind the glass
        d.rectangle((7, 10, 16, 13), fill=CYAN)
        d.rectangle((10, 11, 13, 12), fill=WHITE)
    else:
        for y in (9, 12, 15):  # shutters down
            d.line((5, y, 18, y), fill=OUTLINE)
        d.point((11, 8), fill=CYAN)
    return im


def battleship_parts(sheet):
    """4 x 3 cells of 24 px: (0,0) main gun, (1,0) and (2,0) the pack's turrets as
    anti-air mounts, (3,0) missile bay shut, (0,1) missile bay open, (1,1) deck hatch
    shut, (2,1) deck hatch open, (3,1) bridge armored, (0,2) bridge open."""
    im = Image.new("RGBA", (CELL * 4, CELL * 3), (0, 0, 0, 0))
    im.alpha_composite(main_gun(), (0, 0))
    im.alpha_composite(turret(sheet, 6, 1), (CELL + 4, 4))
    im.alpha_composite(turret(sheet, 5, 1), (CELL * 2 + 4, 4))
    im.alpha_composite(missile_bay(False), (CELL * 3, 0))
    im.alpha_composite(missile_bay(True), (0, CELL))
    im.alpha_composite(hatch(False), (CELL, CELL))
    im.alpha_composite(hatch(True), (CELL * 2, CELL))
    im.alpha_composite(bridge(False), (CELL * 3, CELL))
    im.alpha_composite(bridge(True), (0, CELL * 2))
    return im


def main():
    sheet = Image.open(ASSETS / "kenney" / "tiles.png").convert("RGBA")
    OUT.mkdir(parents=True, exist_ok=True)
    carrier(sheet).save(OUT / "carrier.png")
    carrier_parts(sheet).save(OUT / "carrier_parts.png")
    battleship(sheet).save(OUT / "battleship.png")
    battleship_parts(sheet).save(OUT / "battleship_parts.png")
    print("generated/: carrier.png, carrier_parts.png, battleship.png, battleship_parts.png")


if __name__ == "__main__":
    main()
