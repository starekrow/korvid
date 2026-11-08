#!/usr/bin/env python3
"""Writes arcanum's first maps and tilesets (assets/maps/*.tmj, *.tsj) as Tiled 1.10 JSON.

Run once to make them; after that they are edited in Tiled and committed as Tiled saves
them, and this script is only the record of where they came from. Stdlib only.
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "maps")
W, H = 40, 30

def tileset(name, image, iw, ih, tw, th, tiles=None):
    cols, rows = iw // tw, ih // th
    return {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": name,
            "image": "../tiles/" + image, "imagewidth": iw, "imageheight": ih,
            "tilewidth": tw, "tileheight": th, "columns": cols, "tilecount": cols * rows,
            "margin": 0, "spacing": 0, "tiles": tiles or []}

def solid(i):
    return {"id": i, "properties": [{"name": "solid", "type": "bool", "value": True}]}

def shaped(i, x, y, w, h):
    return {"id": i, "objectgroup": {"draworder": "index", "name": "", "opacity": 1, "type": "objectgroup", "visible": True, "x": 0, "y": 0,
            "objects": [{"id": 1, "name": "", "type": "", "rotation": 0, "visible": True, "x": x, "y": y, "width": w, "height": h}]}}

TILESETS = {
    "floor.tsj":    tileset("floor", "TilesetFloor.png", 352, 417, 16, 16),
    "water.tsj":    tileset("water", "TilesetWater.png", 448, 272, 16, 16, [solid(i) for i in (0, 1, 2, 28, 29, 30, 56, 57, 58)]),
    # Trees and a bush as 32 px tiles of the nature sheet, each solid at its trunk or base.
    "nature32.tsj": tileset("nature32", "TilesetNature.png", 384, 336, 32, 32,
                            [shaped(0, 10, 22, 12, 9), shaped(1, 10, 22, 12, 9), shaped(3, 10, 22, 12, 9), shaped(8, 4, 18, 24, 12)]),
    # The first two houses of the house sheet, 64 x 48, solid but for the eaves.
    "house.tsj":    tileset("house", "TilesetHouse.png", 528, 368, 64, 48, [shaped(0, 2, 14, 60, 34), shaped(1, 2, 14, 60, 34)]),
}
FIRST = {"floor.tsj": 1, "water.tsj": 1001, "nature32.tsj": 2001, "house.tsj": 3001}

GRASS, GRASS_VARIANTS = 264, (244, 245, 265, 266, 267)
PATH = {  # the dirt-on-grass autotile, by which neighbours are path
    "tl": 154, "t": 155, "tr": 156, "l": 176, "c": 177, "r": 178, "bl": 198, "b": 199, "br": 200}
POND = {"tl": 0, "t": 1, "tr": 2, "l": 28, "c": 29, "r": 30, "bl": 56, "b": 57, "br": 58}

class Rng:
    def __init__(self, seed): self.x = seed
    def next(self):
        self.x = (self.x * 1103515245 + 12345) % 2**31
        return self.x >> 16

def autotile(cells, x, y, table):
    """The 3x3 autotile piece for (x, y) in the set `cells`; off the map counts as inside."""
    def has(cx, cy): return (cx, cy) in cells or cx < 0 or cy < 0 or cx >= W or cy >= H
    n, s, w, e = has(x, y - 1), has(x, y + 1), has(x - 1, y), has(x + 1, y)
    key = ("t" if not n else "b" if not s else "") + ("l" if not w else "r" if not e else "")
    return table[key or "c"]

def rect(x0, y0, x1, y1):
    return {(x, y) for x in range(x0, x1) for y in range(y0, y1)}

def tile_layer(lid, name, gids, props=None):
    l = {"id": lid, "name": name, "type": "tilelayer", "width": W, "height": H, "x": 0, "y": 0, "opacity": 1, "visible": True, "data": gids}
    if props: l["properties"] = props
    return l

def objects_layer(lid, name, objects, visible=True, props=None):
    l = {"id": lid, "name": name, "type": "objectgroup", "draworder": "topdown", "x": 0, "y": 0, "opacity": 1, "visible": visible, "objects": objects}
    if props: l["properties"] = props
    return l

class Objects:
    def __init__(self): self.next_id, self.items = 1, []
    def add(self, **o):
        o.setdefault("rotation", 0); o.setdefault("visible", True); o.setdefault("type", ""); o.setdefault("name", "")
        o["id"] = self.next_id; self.next_id += 1
        self.items.append(o)
        return o
    def tile(self, gid, x, y, w, h, name=""): return self.add(gid=gid, x=x, y=y, width=w, height=h, name=name)
    def point(self, name, x, y, props=None):
        o = self.add(name=name, point=True, x=x, y=y, width=0, height=0)
        if props: o["properties"] = props
        return o
    def area(self, name, cls, x, y, w, h, props):
        return self.add(name=name, type=cls, x=x, y=y, width=w, height=h, properties=props)

def make_map(name, title, music, seed, paths, pond, trees, houses, spawns, exits, npcs):
    rng = Rng(seed)
    ground = []
    for y in range(H):
        for x in range(W):
            if (x, y) in paths: g = autotile(paths, x, y, PATH)
            else: g = GRASS_VARIANTS[rng.next() % len(GRASS_VARIANTS)] if rng.next() % 100 < 8 else GRASS
            ground.append(FIRST["floor.tsj"] + g)
    water = [FIRST["water.tsj"] + autotile(pond, x, y, POND) if (x, y) in pond else 0 for y in range(H) for x in range(W)]
    objs = Objects()
    for (gid_local, x, y) in trees: objs.tile(FIRST["nature32.tsj"] + gid_local, x, y, 32, 32)
    for (gid_local, x, y) in houses: objs.tile(FIRST["house.tsj"] + gid_local, x, y, 64, 48)
    for (who, x, y, route) in npcs:
        objs.point(who, x, y, [{"name": "sheet", "type": "string", "value": "example_arcanum/actors/" + who + ".png"},
                               {"name": "route", "type": "string", "value": route}])
    sp = Objects()
    for (n, x, y) in spawns: sp.point(n, x, y)
    ex = Objects()
    for (x, y, w, h, target, entry) in exits:
        ex.area("exit_" + target, "exit", x, y, w, h, [{"name": "target", "type": "string", "value": target},
                                                     {"name": "entry", "type": "string", "value": entry}])
    m = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal", "renderorder": "right-down",
         "infinite": False, "width": W, "height": H, "tilewidth": 16, "tileheight": 16, "compressionlevel": -1,
         "nextlayerid": 6, "nextobjectid": 1000,
         "properties": [{"name": "title", "type": "string", "value": title}, {"name": "music", "type": "string", "value": music}],
         "tilesets": [{"firstgid": FIRST[k], "source": k} for k in ("floor.tsj", "water.tsj", "nature32.tsj", "house.tsj")],
         "layers": [tile_layer(1, "ground", ground), tile_layer(2, "water", water),
                    objects_layer(3, "objects", objs.items, props=[{"name": "ysort", "type": "bool", "value": True}]),
                    objects_layer(4, "spawns", sp.items, visible=False), objects_layer(5, "exits", ex.items, visible=False)]}
    with open(os.path.join(OUT, name), "w") as f: json.dump(m, f, indent=1)

def border_trees(rng, skip):
    trees = []
    for x in range(0, W * 16, 32):
        for y in (32, H * 16):
            if not any(a <= x < b for a, b in skip.get(y, [])): trees.append((rng.next() % 2 * 3, x, y))
    for y in range(64, H * 16, 32):
        for x in (0, W * 16 - 32):
            if not any(a <= y < b for a, b in skip.get(("x", x), [])): trees.append((rng.next() % 2 * 3, x, y))
    return trees

def main():
    os.makedirs(OUT, exist_ok=True)
    for k, ts in TILESETS.items():
        with open(os.path.join(OUT, k), "w") as f: json.dump(ts, f, indent=1)
    # The village: a road from the middle out of the east edge, a lane north to the houses.
    road_y = 14
    v_paths = rect(8, road_y, W, road_y + 3) | rect(18, 6, 21, road_y)
    rng = Rng(7)
    skip = {("x", W * 16 - 32): [(road_y * 16 - 16, (road_y + 4) * 16)]}
    v_trees = border_trees(rng, skip) + [(0, 64, 200), (3, 96, 300), (8, 240, 320), (0, 400, 330), (1, 520, 360), (8, 120, 380), (0, 470, 110)]
    v_houses = [(0, 200, 96), (1, 368, 96), (0, 112, 200)]
    make_map("village.tmj", "Village", "example_arcanum/music/village.ogg", 7, v_paths, set(), v_trees, v_houses,
             [("player_start", 312, 248), ("east", W * 16 - 40, (road_y + 1) * 16 + 8)],
             [(W * 16 - 8, road_y * 16, 8, 48, "field", "west")],
             [("OldMan", 248, 120, ""), ("ManGreen", 260, 300, "0,0 96,0")])
    # The field: the road comes in from the west and meanders east; a pond; more trees.
    f_paths = rect(0, road_y, 22, road_y + 3) | rect(19, 8, 22, road_y) | rect(19, 8, 34, 11)
    f_pond = rect(26, 18, 34, 24)
    rng = Rng(11)
    skip = {("x", 0): [(road_y * 16 - 16, (road_y + 4) * 16)]}
    f_trees = border_trees(rng, skip) + [(0, 96, 120), (1, 160, 140), (3, 300, 330), (0, 360, 120), (8, 200, 300),
                                         (1, 380, 410), (0, 470, 260), (3, 560, 120), (8, 120, 420), (0, 250, 420)]
    make_map("field.tmj", "Field", "example_arcanum/music/road.ogg", 11, f_paths, f_pond, f_trees, [],
             [("player_start", 48, (road_y + 1) * 16 + 8), ("west", 40, (road_y + 1) * 16 + 8)],
             [(0, road_y * 16, 8, 48, "village", "east")],
             [])

if __name__ == "__main__":
    main()
