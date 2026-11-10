#!/usr/bin/env python3
"""Writes Shoreline's ground (assets/maps/shoreline.tmj and shoreline.tsj) as Tiled 1.10 JSON,
from Kenney's Pixel Shmup terrain tiles (assets/kenney/tiles.png). Stdlib only; run from
anywhere:

    python3 modules/example_arcfire/tools/make_maps.py

The map is 17 tiles wide (16 px tiles, drawn at 2x: 544 units over the 540-unit field) and
as many rows as the stage is long plus the field's height; row 0 is the top of the map,
the stage's end. Layers: `sea` (every cell water), `land` (islands of grass and of sand,
nine-sliced), `deco` (trees, houses, flags, the airfield's pads and crates, roads), and
`enemies`, point objects named for what they are (turret, tank, bunker) with their pattern,
health and score as properties (a tank's `route`, in units, a bunker's `path`, `count` and
`launch` pattern). An enemy is placed by the stage distance at which it comes into view
and its x on the field; the script puts an island under each, a road under each tank,
and scatters the rest of the sea with islands from a fixed seed, so the map is the same
every time it is written.
"""
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "maps")

COLS = 17
TILE = 16          # pixels
SCALE = 2          # world units a pixel
FIELD_H = 720
FIELD_TOP = 360
SPAWN_ABOVE = 24   # an enemy comes into view this far above the field's top
STAGE_LENGTH = 20000  # units: the full Shoreline (slice 3), with room past it
ROWS = (STAGE_LENGTH + FIELD_H + 64) // (TILE * SCALE) + 1

# Kenney's tiles_packed.png: 12 columns of 16 px tiles; a gid is the tile's index + 1.
def gid(col, row):
    return row * 12 + col + 1

SEA = gid(6, 3)
GRASS = {"tl": gid(1, 3), "t": gid(2, 3), "tr": gid(3, 3), "l": gid(1, 4), "c": gid(2, 4), "r": gid(3, 4),
         "bl": gid(1, 5), "b": gid(2, 5), "br": gid(3, 5), "one": gid(4, 5)}
SAND = {"tl": gid(7, 3), "t": gid(8, 3), "tr": gid(9, 3), "l": gid(7, 4), "c": gid(8, 4), "r": gid(9, 4),
        "bl": gid(7, 5), "b": gid(8, 5), "br": gid(9, 5), "one": gid(10, 5)}
ROAD = gid(2, 6)
TREES = [gid(0, 4), gid(0, 5), gid(0, 3)]
HOUSES = [gid(0, 6), gid(0, 7), gid(6, 6), gid(6, 7)]
FLAGS = [gid(0, 8), gid(6, 8)]
PAD, CRATE = gid(3, 9), gid(1, 9)

# The ground enemies of Shoreline: (distance, x, kind, properties). Slice 3's first
# milestone places the turrets slices 1 and 2 had as stage marks, two tanks and a bunker;
# later milestones extend the list with the stage.
ENEMIES = [
    (800, -150, "turret", {"pattern": "ring", "health": 14, "score": 500}),
    (1250, -60, "tank", {"pattern": "aimed_burst", "health": 10, "score": 400, "route": 180}),
    (1750, -130, "bunker", {"pattern": "aimed", "health": 24, "score": 800, "path": "bunker_rise", "count": 4, "launch": "aimed"}),
    (2050, 60, "tank", {"pattern": "aimed", "health": 10, "score": 400, "route": 160}),
    (2400, 140, "turret", {"pattern": "spiral", "health": 16, "score": 600}),
    (3000, 0, "turret", {"pattern": "cross", "health": 20, "score": 800}),
]
AIRFIELD = (3600, 4400)  # stage distances: a long island of sand with pads


def origin_y():
    """World y of the map's top edge when the stage's distance is 0."""
    return -FIELD_TOP + ROWS * TILE * SCALE


def map_px(distance, x):
    """The map pixel (x, y) of a point that comes into view at `distance` at field x `x`."""
    world_y = FIELD_TOP + SPAWN_ABOVE + distance
    return (x + COLS * TILE * SCALE / 2) / SCALE, (origin_y() - world_y) / SCALE


def main():
    rnd = random.Random(0x5407e)
    land = [[0] * COLS for _ in range(ROWS)]
    deco = [[0] * COLS for _ in range(ROWS)]
    taken = [[False] * COLS for _ in range(ROWS)]

    def free(c0, r0, w, h):
        if c0 < 0 or r0 < 0 or c0 + w > COLS or r0 + h > ROWS:
            return False
        for r in range(max(0, r0 - 1), min(ROWS, r0 + h + 1)):
            for c in range(max(0, c0 - 1), min(COLS, c0 + w + 1)):
                if taken[r][c]:
                    return False
        return True

    def island(c0, r0, w, h, kind=GRASS, decorate=True):
        for r in range(r0, r0 + h):
            for c in range(c0, c0 + w):
                taken[r][c] = True
                if w == 1 and h == 1:
                    land[r][c] = kind["one"]
                    continue
                top, bottom, left, right = r == r0, r == r0 + h - 1, c == c0, c == c0 + w - 1
                key = ("t" if top else "b" if bottom else "") + ("l" if left else "r" if right else "")
                land[r][c] = kind[{"": "c", "t": "t", "b": "b", "l": "l", "r": "r"}.get(key, key)]
                if decorate and not (top or bottom or left or right) and rnd.random() < 0.18:
                    deco[r][c] = rnd.choice(TREES + TREES + HOUSES + FLAGS) if kind is GRASS else CRATE

    objects = []
    for i, (distance, x, kind, props) in enumerate(ENEMIES):
        px, py = map_px(distance, x)
        c, r = int(px // TILE), int(py // TILE)
        if kind == "tank":
            w = max(5, int(props["route"] / (TILE * SCALE)) + 4)
            c0 = max(0, min(COLS - w, c - w // 2))
            island(c0, r - 2, w, 5, decorate=False)
            for cc in range(c0 + 1, c0 + w - 1):
                deco[r][cc] = ROAD
        else:
            island(max(0, min(COLS - 5, c - 2)), r - 2, 5, 5, decorate=False)
        objects.append({"id": i + 1, "name": kind, "type": "", "point": True, "x": px, "y": py, "width": 0, "height": 0, "rotation": 0,
                        "visible": True, "properties": [
                            {"name": k, "type": "int" if isinstance(v, int) else "string", "value": v} for k, v in props.items()]})
    # The airfield: one long island of sand down the middle, pads and crates on it.
    _, top = map_px(AIRFIELD[1], 0)
    _, bottom = map_px(AIRFIELD[0], 0)
    r0, r1 = int(top // TILE), int(bottom // TILE)
    island(5, r0, 7, r1 - r0, kind=SAND, decorate=False)
    for r in range(r0 + 2, r1 - 1, 4):
        deco[r][8] = PAD
        deco[r + 1][6] = CRATE
    # The rest of the sea: islands where there is room, a few rocks of one tile.
    for _ in range(ROWS * 3):
        w, h = rnd.randint(3, 7), rnd.randint(3, 8)
        c0, r0 = rnd.randint(0, COLS - w), rnd.randint(0, ROWS - h)
        if rnd.random() < 0.3:
            w = h = 1
        if free(c0, r0, w, h):
            island(c0, r0, w, h, kind=GRASS if rnd.random() < 0.85 else SAND)

    def layer(i, name, data):
        return {"id": i, "name": name, "type": "tilelayer", "x": 0, "y": 0, "width": COLS, "height": ROWS,
                "opacity": 1, "visible": True, "data": [v for row in data for v in row]}

    tileset = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "shoreline",
               "image": "../kenney/tiles.png", "imagewidth": 192, "imageheight": 160, "tilewidth": TILE, "tileheight": TILE,
               "columns": 12, "tilecount": 120, "margin": 0, "spacing": 0, "tiles": []}
    tmj = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal", "renderorder": "right-down",
           "infinite": False, "width": COLS, "height": ROWS, "tilewidth": TILE, "tileheight": TILE, "compressionlevel": -1,
           "nextlayerid": 5, "nextobjectid": len(objects) + 1,
           "properties": [{"name": "title", "type": "string", "value": "Shoreline"}],
           "tilesets": [{"firstgid": 1, "source": "shoreline.tsj"}],
           "layers": [layer(1, "sea", [[SEA] * COLS for _ in range(ROWS)]), layer(2, "land", land), layer(3, "deco", deco),
                      {"id": 4, "name": "enemies", "type": "objectgroup", "draworder": "index", "x": 0, "y": 0, "opacity": 1,
                       "visible": True, "objects": objects}]}
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "shoreline.tsj"), "w") as f:
        json.dump(tileset, f, indent=1)
    with open(os.path.join(OUT, "shoreline.tmj"), "w") as f:
        json.dump(tmj, f, separators=(",", ":"))
    print(f"shoreline.tmj: {COLS} x {ROWS} tiles, {len(objects)} enemies")


if __name__ == "__main__":
    main()
