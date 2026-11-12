#!/usr/bin/env python3
"""Arcfire's bosses, kitbashed from Kenney's Pixel Shmup (assets/kenney/, CC0) into
assets/generated/. Deterministic: the same files every run. Run from anywhere:

    python3 examples/arcfire/tools/make_bosses.py

Each boss is two images, at the pack's pixel scale (drawn at 2x, as the pack's ships are),
and the missile boat (a midsize airship, one body) is one, generated/boat.png:

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
import math
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


def boat():
    """32 x 72: a missile boat, an airship, bow up as drawn (the game turns air enemies to
    face down): a grey hull with the enemy's orange band, two launchers of three missiles
    each, a bridge amidships, and two engines astern, lit."""
    w, h = 32, 72
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for x in (9, 19):  # the engines
        d.rectangle((x, 60, x + 4, 70), fill=OUTLINE)
        d.rectangle((x + 1, 64, x + 3, 69), fill=CYAN)
    outline_fill(d, [(16, 0), (25, 12), (27, 26), (27, 58), (22, 62), (10, 62), (5, 58), (5, 26), (7, 12)], GREY)
    # The orange band along the gunwales.
    for x0, x1 in ((7, 8), (24, 25)):
        d.rectangle((x0, 24, x1, 56), fill=ORANGE)
    d.line((16, 6, 16, 60), fill=LIGHT)
    # Two launchers, fore and aft of the bridge: a box of three missile tips each.
    for y0 in (14, 44):
        d.rectangle((10, y0, 22, y0 + 10), fill=OUTLINE)
        d.rectangle((11, y0 + 1, 21, y0 + 9), fill=DEEP)
        for x in (12, 15, 18):  # missiles, a pixel apart
            d.rectangle((x, y0 + 2, x + 1, y0 + 8), fill=LIGHT)
            d.rectangle((x, y0 + 2, x + 1, y0 + 3), fill=ORANGE)
    # The bridge, its windows lit.
    d.rectangle((11, 28, 21, 40), fill=OUTLINE)
    d.rectangle((12, 29, 20, 39), fill=LIGHT)
    d.line((13, 31, 19, 31), fill=CYAN_DARK)
    d.line((13, 34, 19, 34), fill=CYAN_DARK)
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


def gunship(accent, accent_dark):
    """48 x 80: a gunship, bow down: a narrow armored hull on two engine pods, a turret
    either side amidships and a heavy gun in the bow (its core). The pair differ by their
    band's colour (the left one orange, the right one cyan)."""
    w, h = 48, 80
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for x in (6, 36):  # engine pods astern (the top), lit
        d.rectangle((x, 0, x + 6, 10), fill=OUTLINE)
        d.rectangle((x + 2, 1, x + 4, 5), fill=CYAN)
    outline_fill(d, [(4, 8), (16, 8), (18, 30), (18, 50), (14, 58), (4, 50)], GREY)   # the pods' pylons
    outline_fill(d, [(32, 8), (44, 8), (44, 50), (34, 58), (30, 50), (30, 30)], GREY)
    outline_fill(d, [(16, 4), (32, 4), (38, 22), (38, 62), (30, 78), (18, 78), (10, 62), (10, 22)], GREY)
    for x0, x1 in ((12, 13), (35, 36)):  # the band along the hull's flanks
        d.rectangle((x0, 24, x1, 58), fill=accent)
        d.line((x0, 58, x1, 58), fill=accent_dark)
    d.line((24, 6, 24, 54), fill=LIGHT)  # a spine
    for y in (18, 40):
        d.line((14, y, 34, y), fill=OUTLINE)
    d.rectangle((19, 8, 29, 16), fill=OUTLINE)  # the bridge, astern
    d.rectangle((20, 9, 28, 15), fill=LIGHT)
    d.line((21, 12, 27, 12), fill=CYAN_DARK)
    for cx in (12, 36):  # turret sockets on the pylons
        socket(d, cx, 32, 6)
    socket(d, 24, 64, 9)  # the gun
    return im


def heavy_gun(open_):
    """The gunship's bow gun: shuttered until its phase, then three barrels down."""
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if open_:
        for x in (6, 11, 16):
            d.rectangle((x, 12, x + 2, 23), fill=OUTLINE)
            d.rectangle((x + 1, 13, x + 1, 22), fill=LIGHT)
    d.ellipse((2, 2, 21, 21), fill=OUTLINE)
    d.ellipse((4, 4, 19, 19), fill=GREY)
    if open_:
        d.ellipse((7, 7, 16, 16), fill=ORANGE)
        d.ellipse((9, 9, 14, 14), fill=YELLOW)
    else:
        d.line((4, 11, 19, 11), fill=OUTLINE)
        d.line((11, 4, 11, 19), fill=OUTLINE)
        d.point((11, 11), fill=ORANGE)
    d.line((5, 8, 8, 5), fill=LIGHT)
    return im


def gunship_parts(sheet):
    """2 x 2 cells of 24 px: (0,0) and (1,0) the pack's turrets, (0,1) the gun shuttered,
    (1,1) the gun open."""
    im = Image.new("RGBA", (CELL * 2, CELL * 2), (0, 0, 0, 0))
    im.alpha_composite(turret(sheet, 6, 1), (4, 4))
    im.alpha_composite(turret(sheet, 5, 1), (CELL + 4, 4))
    im.alpha_composite(heavy_gun(False), (0, CELL))
    im.alpha_composite(heavy_gun(True), (CELL, CELL))
    return im


def station():
    """128 x 128: the station core, seen from above: an outer ring of six shield emitters
    on spokes, an inner ring of four weapon mounts, the core in the middle under its
    shutters. The ring is the pack's grey with the enemy's orange in its seams."""
    w = h = 128
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = 64
    d.ellipse((4, 4, 123, 123), fill=OUTLINE)       # the outer ring
    d.ellipse((7, 7, 120, 120), fill=GREY)
    d.ellipse((16, 16, 111, 111), fill=OUTLINE)
    d.ellipse((18, 18, 109, 109), fill=(0, 0, 0, 0))
    for k in range(6):  # spokes, between the emitters
        a = math.pi / 6 + k * math.pi / 3
        x0, y0 = c + math.cos(a) * 40, c + math.sin(a) * 40
        x1, y1 = c + math.cos(a) * 58, c + math.sin(a) * 58
        d.line((x0, y0, x1, y1), fill=OUTLINE, width=6)
        d.line((x0, y0, x1, y1), fill=LIGHT, width=2)
    for k in range(12):  # the outer ring's seams, orange
        a = k * math.pi / 6
        x0, y0 = c + math.cos(a) * 53, c + math.sin(a) * 53
        x1, y1 = c + math.cos(a) * 59, c + math.sin(a) * 59
        d.line((x0, y0, x1, y1), fill=ORANGE_DARK, width=2)
    d.ellipse((22, 22, 105, 105), fill=OUTLINE)     # the inner ring
    d.ellipse((25, 25, 102, 102), fill=GREY)
    d.ellipse((26, 26, 101, 101), outline=LIGHT)
    d.ellipse((46, 46, 81, 81), fill=OUTLINE)
    for k in range(6):  # emitter sockets on the outer ring
        a = k * math.pi / 3
        socket(d, round(c + math.cos(a) * 56), round(c + math.sin(a) * 56), 8)
    for k in range(4):  # weapon sockets on the inner ring
        a = math.pi / 4 + k * math.pi / 2
        socket(d, round(c + math.cos(a) * 30), round(c + math.sin(a) * 30), 8)
    socket(d, c, c, 12)  # the core
    return im


def emitter():
    """A shield emitter: a cyan lens in a grey housing."""
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(12, 1), (22, 7), (22, 17), (12, 23), (2, 17), (2, 7)], fill=OUTLINE)
    d.polygon([(12, 3), (20, 8), (20, 16), (12, 21), (4, 16), (4, 8)], fill=GREY)
    d.ellipse((7, 7, 16, 16), fill=CYAN_DARK)
    d.ellipse((9, 9, 14, 14), fill=CYAN)
    d.point((10, 10), fill=WHITE)
    return im


def weapon_mount(open_):
    """A weapon on the inner ring: four barrels round an orange breech, or its armor."""
    im = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle((2, 2, 21, 21), fill=OUTLINE)
    if open_:
        d.rectangle((4, 4, 19, 19), fill=DEEP)
        for x, y in ((5, 5), (15, 5), (5, 15), (15, 15)):
            d.rectangle((x, y, x + 3, y + 3), fill=LIGHT)
        d.rectangle((9, 9, 14, 14), fill=ORANGE)
        d.point((11, 11), fill=YELLOW)
    else:
        d.rectangle((4, 4, 19, 19), fill=GREY)
        for y in (8, 12, 16):
            d.line((4, y, 19, y), fill=OUTLINE)
        d.line((4, 4, 19, 4), fill=LIGHT)
    return im


def station_parts():
    """4 x 2 cells of 24 px: (0,0) emitter, (1,0) weapon armored, (2,0) weapon open, (0,1)
    core armored, (1,1) core open."""
    im = Image.new("RGBA", (CELL * 4, CELL * 2), (0, 0, 0, 0))
    im.alpha_composite(emitter(), (0, 0))
    im.alpha_composite(weapon_mount(False), (CELL, 0))
    im.alpha_composite(weapon_mount(True), (CELL * 2, 0))
    im.alpha_composite(core(False), (0, CELL))
    im.alpha_composite(core(True), (CELL, CELL))
    return im


def segments(sheet):
    """96 x 96: two station segments drifting through Orbit's middle planes, a cell of
    96 x 48 each: (0) a truss with two turret pads, (1) a module with a pad at each end and
    a lit window band. Turrets are the game's ground rows on the pads."""
    im = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # A truss: two rails, cross bracing between, a pad at each third.
    for y in (8, 38):
        outline_fill(d, [(2, y), (94, y), (94, y + 4), (2, y + 4)], GREY, 1)
    for x in range(4, 92, 12):
        d.line((x, 12, x + 12, 38), fill=OUTLINE, width=2)
        d.line((x + 12, 12, x, 38), fill=OUTLINE, width=2)
    for cx in (28, 68):
        d.rectangle((cx - 11, 13, cx + 11, 37), fill=OUTLINE)
        d.rectangle((cx - 9, 15, cx + 9, 35), fill=LIGHT)
        socket(d, cx, 25, 7)
    # A module: a long pressure hull with end caps and solar vanes.
    y0 = 48
    outline_fill(d, [(14, y0 + 8), (82, y0 + 8), (90, y0 + 16), (90, y0 + 32), (82, y0 + 40), (14, y0 + 40), (6, y0 + 32), (6, y0 + 16)], GREY)
    d.rectangle((24, y0 + 22, 72, y0 + 26), fill=OUTLINE)
    for x in range(26, 72, 6):
        d.rectangle((x, y0 + 23, x + 3, y0 + 25), fill=CYAN)
    for x0, x1 in ((0, 6), (90, 96)):
        d.rectangle((x0, y0 + 2, x1 - 1, y0 + 45), fill=OUTLINE)
        d.rectangle((x0 + 1, y0 + 4, x1 - 2, y0 + 43), fill=CYAN_DARK)
    d.rectangle((20, y0 + 10, 76, y0 + 12), fill=ORANGE)
    for cx in (22, 74):
        socket(d, cx, y0 + 32, 6)
    vent = tile(sheet, 4, 1)
    im.alpha_composite(vent, (40, y0 + 28))
    return im


def debris():
    """72 x 24: three pieces of wreckage, 24 px cells: a torn plate, a strut, a scrap of
    hull with its band. They tumble as they drift."""
    im = Image.new("RGBA", (CELL * 3, CELL), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(3, 6), (15, 2), (21, 9), (18, 20), (8, 21), (2, 14)], fill=OUTLINE)
    d.polygon([(5, 7), (14, 4), (19, 10), (16, 18), (9, 19), (4, 13)], fill=GREY)
    d.line((6, 12, 16, 8), fill=LIGHT)
    d.point((12, 15), fill=DEEP)
    x = CELL
    d.polygon([(x + 2, 17), (x + 17, 2), (x + 22, 7), (x + 7, 22)], fill=OUTLINE)
    d.polygon([(x + 5, 17), (x + 17, 5), (x + 19, 7), (x + 7, 19)], fill=LIGHT)
    for k in range(3):
        d.point((x + 9 + 3 * k, 13 - 3 * k), fill=OUTLINE)
    x = CELL * 2
    d.polygon([(x + 2, 4), (x + 20, 3), (x + 22, 12), (x + 16, 21), (x + 4, 19)], fill=OUTLINE)
    d.polygon([(x + 4, 6), (x + 18, 5), (x + 20, 12), (x + 15, 18), (x + 6, 17)], fill=GREY)
    d.rectangle((x + 5, 10, x + 19, 12), fill=ORANGE)
    d.line((x + 5, 12, x + 19, 12), fill=ORANGE_DARK)
    return im


def capital(sheet):
    """224 x 40: the capital ship over Orbit, broadside, bow left, far off: four sections of
    56 x 40 side by side (the game draws each as a sprite of its own and breaks them away one
    at a time): the bow with its main battery, the bridge tower, the hangar decks, the
    engines. The pack's grey and its slate, with the friendly cyan where the enemy has
    orange: it is the other side's flagship, losing."""
    w, h = 224, 40
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    outline_fill(d, [(2, 22), (18, 12), (210, 10), (220, 16), (220, 30), (210, 34), (18, 32)], GREY)
    d.rectangle((20, 14, 208, 15), fill=LIGHT)             # the deck's edge, lit
    d.rectangle((24, 27, 206, 28), fill=CYAN_DARK)         # a band along the hull
    for x in range(30, 206, 6):                            # portholes
        d.point((x, 22), fill=CYAN)
    for x in (56, 112, 168):                               # the sections' seams
        d.line((x, 11, x, 33), fill=OUTLINE)
    for x in (16, 30, 44):                                 # the bow's main battery
        d.rectangle((x - 4, 6, x + 4, 12), fill=OUTLINE)
        d.rectangle((x - 3, 7, x + 3, 11), fill=GREY)
        d.line((x - 10, 8, x - 4, 8), fill=OUTLINE)         # barrels toward the bow
    outline_fill(d, [(70, 12), (74, 0), (98, 0), (102, 12)], GREY)  # the bridge tower
    d.rectangle((78, 3, 94, 6), fill=CYAN)
    d.line((86, 0, 86, 3), fill=LIGHT)
    for x in (124, 146):                                   # hangar mouths
        d.rectangle((x - 8, 18, x + 8, 26), fill=OUTLINE)
        d.rectangle((x - 6, 20, x + 6, 25), fill=DEEP)
        d.rectangle((x - 2, 22, x + 2, 23), fill=CYAN_DARK)
    vent = tile(sheet, 4, 1)
    im.alpha_composite(vent, (176, 12))
    for y in (14, 22, 28):                                 # the engines, astern, lit
        d.rectangle((216, y, 223, y + 4), fill=OUTLINE)
        d.rectangle((219, y + 1, 223, y + 3), fill=CYAN)
    return im


def main():
    sheet = Image.open(ASSETS / "kenney" / "tiles.png").convert("RGBA")
    OUT.mkdir(parents=True, exist_ok=True)
    carrier(sheet).save(OUT / "carrier.png")
    carrier_parts(sheet).save(OUT / "carrier_parts.png")
    battleship(sheet).save(OUT / "battleship.png")
    battleship_parts(sheet).save(OUT / "battleship_parts.png")
    boat().save(OUT / "boat.png")
    gunship(ORANGE, ORANGE_DARK).save(OUT / "gunship_left.png")
    gunship(CYAN, CYAN_DARK).save(OUT / "gunship_right.png")
    gunship_parts(sheet).save(OUT / "gunship_left_parts.png")
    gunship_parts(sheet).save(OUT / "gunship_right_parts.png")
    station().save(OUT / "station.png")
    station_parts().save(OUT / "station_parts.png")
    segments(sheet).save(OUT / "segments.png")
    debris().save(OUT / "debris.png")
    capital(sheet).save(OUT / "capital.png")
    print("generated/: carrier, battleship (and their parts), boat, gunship_left, gunship_right (and their parts), "
          "station (and its parts), segments, debris, capital")


if __name__ == "__main__":
    main()
