-- Writes arcanum's first maps and tilesets (assets/maps/*.tmj, *.tsj) as Tiled 1.10 JSON.
--
--     kormake run examples/arcanum/tools/make_maps.lua
--
-- Run once to make them; after that they are edited in Tiled and committed as Tiled saves
-- them, and this script is only the record of where they came from. Ported from
-- make_maps.py: it writes what that wrote, byte for byte.

local OUT = (arg[0]:match("^(.*)[/\\][^/\\]*$") or ".") .. "/../assets/maps"
local W, H = 40, 30
local json = kv.json
local O = json.object

local function tileset(name, image, iw, ih, tw, th, tiles)
    local cols, rows = iw // tw, ih // th
    return O("type", "tileset", "version", "1.10", "tiledversion", "1.10.2", "name", name,
             "image", "../tiles/" .. image, "imagewidth", iw, "imageheight", ih,
             "tilewidth", tw, "tileheight", th, "columns", cols, "tilecount", cols * rows,
             "margin", 0, "spacing", 0, "tiles", tiles or {})
end

local function solid(i) return O("id", i, "properties", { O("name", "solid", "type", "bool", "value", true) }) end

local function shaped(i, x, y, w, h)
    return O("id", i, "objectgroup", O("draworder", "index", "name", "", "opacity", 1, "type", "objectgroup", "visible", true, "x", 0, "y", 0,
             "objects", { O("id", 1, "name", "", "type", "", "rotation", 0, "visible", true, "x", x, "y", y, "width", w, "height", h) }))
end

local function solids(ids)
    local t = {}
    for i, id in ipairs(ids) do t[i] = solid(id) end
    return t
end

-- The tilesets, in the order they are written.
local TILESETS = {
    { "floor.tsj", tileset("floor", "TilesetFloor.png", 352, 417, 16, 16) },
    { "water.tsj", tileset("water", "TilesetWater.png", 448, 272, 16, 16, solids({ 0, 1, 2, 28, 29, 30, 56, 57, 58 })) },
    -- Trees and a bush as 32 px tiles of the nature sheet, each solid at its trunk or base.
    { "nature32.tsj", tileset("nature32", "TilesetNature.png", 384, 336, 32, 32,
                              { shaped(0, 10, 22, 12, 9), shaped(1, 10, 22, 12, 9), shaped(3, 10, 22, 12, 9), shaped(8, 4, 18, 24, 12) }) },
    -- The first two houses of the house sheet, 64 x 48, solid but for the eaves.
    { "house.tsj", tileset("house", "TilesetHouse.png", 528, 368, 64, 48, { shaped(0, 2, 14, 60, 34), shaped(1, 2, 14, 60, 34) }) },
}
local FIRST = { ["floor.tsj"] = 1, ["water.tsj"] = 1001, ["nature32.tsj"] = 2001, ["house.tsj"] = 3001 }

local GRASS, GRASS_VARIANTS = 264, { 244, 245, 265, 266, 267 }
local PATH = { -- the dirt-on-grass autotile, by which neighbours are path
    tl = 154, t = 155, tr = 156, l = 176, c = 177, r = 178, bl = 198, b = 199, br = 200 }
local POND = { tl = 0, t = 1, tr = 2, l = 28, c = 29, r = 30, bl = 56, b = 57, br = 58 }

-- The script's own generator (not Python's random): x = (x * 1103515245 + 12345) mod 2^31.
local function Rng(seed)
    local x = seed
    return { next = function() x = (x * 1103515245 + 12345) % (1 << 31); return x >> 16 end }
end

-- A set of cells, keyed "x,y".
local function key(x, y) return x .. "," .. y end
local function rect(x0, y0, x1, y1, into)
    local s = into or {}
    for x = x0, x1 - 1 do for y = y0, y1 - 1 do s[key(x, y)] = true end end
    return s
end

-- The 3x3 autotile piece for (x, y) in the set `cells`; off the map counts as inside.
local function autotile(cells, x, y, table_)
    local function has(cx, cy) return cells[key(cx, cy)] or cx < 0 or cy < 0 or cx >= W or cy >= H end
    local n, s, w, e = has(x, y - 1), has(x, y + 1), has(x - 1, y), has(x + 1, y)
    local k = (not n and "t" or not s and "b" or "") .. (not w and "l" or not e and "r" or "")
    return table_[k == "" and "c" or k]
end

local function tile_layer(lid, name, gids, props)
    local l = O("id", lid, "name", name, "type", "tilelayer", "width", W, "height", H, "x", 0, "y", 0, "opacity", 1, "visible", true, "data", gids)
    if props then l.properties = props end
    return l
end

local function objects_layer(lid, name, objects, visible, props)
    if visible == nil then visible = true end
    local l = O("id", lid, "name", name, "type", "objectgroup", "draworder", "topdown", "x", 0, "y", 0, "opacity", 1, "visible", visible, "objects", objects)
    if props then l.properties = props end
    return l
end

-- Objects in the order Python's Objects.add left their keys: what was given, then rotation,
-- visible, type and name if they were not, then id.
local function Objects()
    local self = { next_id = 1, items = {} }
    function self.add(...)
        local o = O(...)
        if o.rotation == nil then o.rotation = 0 end
        if o.visible == nil then o.visible = true end
        if o.type == nil then o.type = "" end
        if o.name == nil then o.name = "" end
        o.id = self.next_id
        self.next_id = self.next_id + 1
        self.items[#self.items + 1] = o
        return o
    end
    function self.tile(gid, x, y, w, h, name) return self.add("gid", gid, "x", x, "y", y, "width", w, "height", h, "name", name or "") end
    function self.point(name, x, y, props)
        local o = self.add("name", name, "point", true, "x", x, "y", y, "width", 0, "height", 0)
        if props then o.properties = props end
        return o
    end
    function self.area(name, cls, x, y, w, h, props)
        return self.add("name", name, "type", cls, "x", x, "y", y, "width", w, "height", h, "properties", props)
    end
    return self
end

local function prop(name, type_, value) return O("name", name, "type", type_, "value", value) end

local function make_map(name, title, music, seed, paths, pond, trees, houses, spawns, exits, npcs, rocks)
    local rng = Rng(seed)
    local ground = {}
    for y = 0, H - 1 do
        for x = 0, W - 1 do
            local g
            if paths[key(x, y)] then g = autotile(paths, x, y, PATH)
            elseif rng.next() % 100 < 8 then g = GRASS_VARIANTS[rng.next() % #GRASS_VARIANTS + 1]
            else g = GRASS end
            ground[#ground + 1] = FIRST["floor.tsj"] + g
        end
    end
    local water = {}
    for y = 0, H - 1 do
        for x = 0, W - 1 do water[#water + 1] = pond[key(x, y)] and FIRST["water.tsj"] + autotile(pond, x, y, POND) or 0 end
    end
    local objs = Objects()
    for _, t in ipairs(trees) do objs.tile(FIRST["nature32.tsj"] + t[1], t[2], t[3], 32, 32) end
    for _, h in ipairs(houses) do objs.tile(FIRST["house.tsj"] + h[1], h[2], h[3], 64, 48) end
    for _, n in ipairs(npcs) do
        objs.point(n[1], n[2], n[3], { prop("sheet", "string", "arcanum/actors/" .. n[1] .. ".png"), prop("route", "string", n[4]) })
    end
    for _, r in ipairs(rocks) do objs.point("rock", r[1], r[2]) end -- a pushable boulder from the nature sheet, the point its center
    local sp = Objects()
    for _, s in ipairs(spawns) do sp.point(s[1], s[2], s[3]) end
    local ex = Objects()
    for _, e in ipairs(exits) do
        ex.area("exit_" .. e[5], "exit", e[1], e[2], e[3], e[4], { prop("target", "string", e[5]), prop("entry", "string", e[6]) })
    end
    local tilesets = {}
    for _, k in ipairs({ "floor.tsj", "water.tsj", "nature32.tsj", "house.tsj" }) do tilesets[#tilesets + 1] = O("firstgid", FIRST[k], "source", k) end
    local m = O("type", "map", "version", "1.10", "tiledversion", "1.10.2", "orientation", "orthogonal", "renderorder", "right-down",
                "infinite", false, "width", W, "height", H, "tilewidth", 16, "tileheight", 16, "compressionlevel", -1,
                "nextlayerid", 6, "nextobjectid", 1000,
                "properties", { prop("title", "string", title), prop("music", "string", music) },
                "tilesets", tilesets,
                "layers", { tile_layer(1, "ground", ground), tile_layer(2, "water", water),
                            objects_layer(3, "objects", objs.items, true, { prop("ysort", "bool", true) }),
                            objects_layer(4, "spawns", sp.items, false), objects_layer(5, "exits", ex.items, false) })
    assert(kv.fs.write(OUT .. "/" .. name, json.encode(m, { indent = 1 })))
end

-- Trees along the edges, but where skip leaves a gap: skip[y] (a row of trees) or skip["x" .. x]
-- (a column), as lists of {from, to} ranges.
local function border_trees(rng, skip)
    local trees = {}
    local function skipped(ranges, v)
        for _, r in ipairs(ranges or {}) do if r[1] <= v and v < r[2] then return true end end
        return false
    end
    for x = 0, W * 16 - 1, 32 do
        for _, y in ipairs({ 32, H * 16 }) do
            if not skipped(skip[y], x) then trees[#trees + 1] = { rng.next() % 2 * 3, x, y } end
        end
    end
    for y = 64, H * 16 - 1, 32 do
        for _, x in ipairs({ 0, W * 16 - 32 }) do
            if not skipped(skip["x" .. x], y) then trees[#trees + 1] = { rng.next() % 2 * 3, x, y } end
        end
    end
    return trees
end

local function concat(a, b)
    local t = {}
    for _, v in ipairs(a) do t[#t + 1] = v end
    for _, v in ipairs(b) do t[#t + 1] = v end
    return t
end

kv.fs.mkdir(OUT)
for _, t in ipairs(TILESETS) do assert(kv.fs.write(OUT .. "/" .. t[1], json.encode(t[2], { indent = 1 }))) end
-- The village: a road from the middle out of the east edge, a lane north to the houses.
local road_y = 14
local v_paths = rect(18, 6, 21, road_y, rect(8, road_y, W, road_y + 3))
local rng = Rng(7)
local skip = { ["x" .. (W * 16 - 32)] = { { road_y * 16 - 16, (road_y + 4) * 16 } } }
local v_trees = concat(border_trees(rng, skip), { { 0, 64, 200 }, { 3, 96, 300 }, { 8, 240, 320 }, { 0, 400, 330 }, { 1, 520, 360 }, { 8, 120, 380 }, { 0, 470, 110 } })
local v_houses = { { 0, 200, 96 }, { 1, 368, 96 }, { 0, 112, 200 } }
make_map("village.tmj", "Village", "arcanum/music/village.ogg", 7, v_paths, {}, v_trees, v_houses,
         { { "player_start", 312, 248 }, { "east", W * 16 - 40, (road_y + 1) * 16 + 8 } },
         { { W * 16 - 8, road_y * 16, 8, 48, "field", "west" } },
         { { "OldMan", 248, 120, "" }, { "ManGreen", 260, 300, "0,0 96,0" } },
         { { 312, 196 }, { 232, 176 }, { 440, 344 }, { 168, 344 } })
-- The field: the road comes in from the west and meanders east; a pond; more trees.
local f_paths = rect(19, 8, 34, 11, rect(19, 8, 22, road_y, rect(0, road_y, 22, road_y + 3)))
local f_pond = rect(26, 18, 34, 24)
rng = Rng(11)
skip = { ["x0"] = { { road_y * 16 - 16, (road_y + 4) * 16 } } }
local f_trees = concat(border_trees(rng, skip), { { 0, 96, 120 }, { 1, 160, 140 }, { 3, 300, 330 }, { 0, 360, 120 }, { 8, 200, 300 },
                                                  { 1, 380, 410 }, { 0, 470, 260 }, { 3, 560, 120 }, { 8, 120, 420 }, { 0, 250, 420 } })
make_map("field.tmj", "Field", "arcanum/music/road.ogg", 11, f_paths, f_pond, f_trees, {},
         { { "player_start", 48, (road_y + 1) * 16 + 8 }, { "west", 40, (road_y + 1) * 16 + 8 } },
         { { 0, road_y * 16, 8, 48, "village", "east" } },
         {},
         { { 152, 196 }, { 264, 300 }, { 424, 248 } })
