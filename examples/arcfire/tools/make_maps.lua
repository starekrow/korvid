-- Writes Shoreline's ground (assets/maps/shoreline.tmj and shoreline.tsj) as Tiled 1.10
-- JSON, from Kenney's Pixel Shmup terrain tiles (assets/kenney/tiles.png). Run from
-- anywhere:
--
--     kormake run examples/arcfire/tools/make_maps.lua
--
-- The map is 17 tiles wide (16 px tiles, drawn at 2x: 544 units over the 540-unit field)
-- and as many rows as MAP_LENGTH (the stage, and what scrolls by at its bosses) plus the
-- field's height; row 0 is the top of the map, past the stage's end. Layers: `sea` (every
-- cell water), `land` (islands of grass and of sand, nine-sliced), `deco` (trees, houses,
-- flags, the airfield's pads and crates, roads), and `enemies`, point objects named for
-- what they are (turret, tank, bunker) with their pattern, health and score as properties
-- (a tank's `route`, in units, a bunker's `path`, `count` and `launch` pattern). An enemy
-- is placed by the stage distance at which it comes into view and its x on the field; the
-- script puts an island under each, a road under each tank, and scatters the rest of the
-- sea with islands from a fixed seed (but for the battleship's open water at the end), so
-- the map is the same every time it is written. Ported from make_maps.py: kv.random draws
-- Python's random module's numbers (the engine's Mt19937, seeded as Python seeds it), so
-- the files come out byte for byte the same.

local OUT = (arg[0]:match("^(.*)[/\\][^/\\]*$") or ".") .. "/../assets/maps"

local COLS = 17
local TILE = 16        -- pixels
local SCALE = 2        -- world units a pixel
local FIELD_H = 720
local FIELD_TOP = 360
local SPAWN_ABOVE = 24 -- an enemy comes into view this far above the field's top
-- Units of map: the stage's 18,400, and what scrolls by while its marks wait at the two
-- bosses (the stage's clock holds, the scroll does not: at most about 3,300 at the carrier
-- and 4,900 at the battleship), as open water.
local MAP_LENGTH = 26000
local ROWS = (MAP_LENGTH + FIELD_H + 64) // (TILE * SCALE) + 1

-- Kenney's tiles_packed.png: 12 columns of 16 px tiles; a gid is the tile's index + 1.
local function gid(col, row) return row * 12 + col + 1 end

local SEA = gid(6, 3)
local GRASS = { tl = gid(1, 3), t = gid(2, 3), tr = gid(3, 3), l = gid(1, 4), c = gid(2, 4), r = gid(3, 4),
                bl = gid(1, 5), b = gid(2, 5), br = gid(3, 5), one = gid(4, 5) }
local SAND = { tl = gid(7, 3), t = gid(8, 3), tr = gid(9, 3), l = gid(7, 4), c = gid(8, 4), r = gid(9, 4),
               bl = gid(7, 5), b = gid(8, 5), br = gid(9, 5), one = gid(10, 5) }
local ROAD = gid(2, 6)
local TREES = { gid(0, 4), gid(0, 5), gid(0, 3) }
local HOUSES = { gid(0, 6), gid(0, 7), gid(6, 6), gid(6, 7) }
local FLAGS = { gid(0, 8), gid(6, 8) }
local PAD, CRATE = gid(3, 9), gid(1, 9)

-- The ground enemies of Shoreline: {distance, x, kind, properties}, by the map's distance
-- (a `drop` makes one an item carrier: what it leaves when it dies) (the stage's, plus
-- whatever scrolled by at a boss). The opening has the turrets slices 1 and 2 had as
-- stage marks, two tanks and a bunker; then the islands before the carrier; then the
-- second half, past the longest carrier fight, denser; then open water for the
-- battleship. Properties are name, value pairs, in the order they are written.
local ENEMIES = {
    { 800, -150, "turret", { "pattern", "ring", "health", 14, "score", 500 } },
    { 1250, -60, "tank", { "drop", "power", "pattern", "aimed_burst", "health", 10, "score", 400, "route", 180 } },
    { 1750, -130, "bunker", { "pattern", "aimed", "health", 24, "score", 800, "path", "bunker_rise", "count", 4, "launch", "aimed" } },
    { 2050, 60, "tank", { "pattern", "aimed", "health", 10, "score", 400, "route", 160 } },
    { 2400, 140, "turret", { "pattern", "spiral", "health", 16, "score", 600 } },
    { 3000, 0, "turret", { "pattern", "cross", "health", 20, "score", 800 } },
    -- The islands.
    { 4300, -140, "tank", { "pattern", "aimed_burst", "health", 12, "score", 400, "route", 200 } },
    { 4700, 120, "turret", { "pattern", "ring", "health", 16, "score", 600 } },
    { 5100, -60, "bunker", { "drop", "option", "pattern", "aimed", "health", 24, "score", 800, "path", "bunker_rise", "count", 4, "launch", "aimed" } },
    { 5500, 150, "tank", { "pattern", "aimed", "health", 12, "score", 400, "route", 160 } },
    { 5900, -150, "turret", { "pattern", "spiral", "health", 18, "score", 700 } },
    { 6300, 40, "bunker", { "pattern", "aimed_burst", "health", 24, "score", 800, "path", "bunker_rise", "count", 5, "launch", "aimed" } },
    { 6700, -100, "tank", { "pattern", "aimed_burst", "health", 12, "score", 400, "route", 220 } },
    { 7100, 160, "turret", { "drop", "power", "pattern", "cross", "health", 20, "score", 800 } },
    { 7500, -40, "turret", { "pattern", "ring", "health", 16, "score", 600 } },
    -- The second half.
    { 12400, -150, "turret", { "pattern", "spiral", "health", 20, "score", 800 } },
    { 12700, 150, "turret", { "pattern", "spiral", "health", 20, "score", 800 } },
    { 13100, 0, "bunker", { "drop", "option", "pattern", "aimed_burst", "health", 30, "score", 1000, "path", "bunker_rise", "count", 6, "launch", "aimed_burst" } },
    { 13500, -120, "tank", { "pattern", "needles", "health", 14, "score", 500, "route", 220 } },
    { 13900, 120, "tank", { "pattern", "aimed_burst", "health", 14, "score", 500, "route", 200 } },
    { 14300, -160, "turret", { "pattern", "cross", "health", 22, "score", 900 } },
    { 14600, 160, "turret", { "pattern", "cross", "health", 22, "score", 900 } },
    { 15000, 0, "bunker", { "pattern", "aimed", "health", 30, "score", 1000, "path", "bunker_rise", "count", 6, "launch", "aimed" } },
    { 15400, -80, "tank", { "pattern", "aimed", "health", 14, "score", 500, "route", 240 } },
    { 15800, 100, "turret", { "pattern", "rain", "health", 20, "score", 800 } },
    { 16200, -140, "turret", { "drop", "egg", "pattern", "spiral", "health", 22, "score", 900 } },
    { 16500, 140, "bunker", { "pattern", "aimed_burst", "health", 30, "score", 1000, "path", "bunker_rise", "count", 6, "launch", "aimed" } },
    { 17000, 0, "turret", { "pattern", "ring", "health", 24, "score", 1000 } },
    { 17400, -150, "tank", { "pattern", "aimed_burst", "health", 14, "score", 500, "route", 200 } },
    { 17800, 150, "tank", { "pattern", "aimed_burst", "health", 14, "score", 500, "route", 200 } },
}
local AIRFIELD = { 7900, 9700 }                   -- map distances: a long island of sand with pads, under the carrier
local OPEN_WATER = { 19000, MAP_LENGTH + FIELD_H } -- the battleship's: no islands

-- World y of the map's top edge when the stage's distance is 0.
local function origin_y() return -FIELD_TOP + ROWS * TILE * SCALE end

-- The map pixel (x, y) of a point that comes into view at `distance` at field x `x`.
local function map_px(distance, x)
    local world_y = FIELD_TOP + SPAWN_ABOVE + distance
    return (x + COLS * TILE * SCALE / 2) / SCALE, (origin_y() - world_y) / SCALE
end

local function grid(v)
    local g = {}
    for r = 0, ROWS - 1 do
        g[r] = {}
        for c = 0, COLS - 1 do g[r][c] = v end
    end
    return g
end

local rnd = kv.random(0x5407e) -- Python's random.Random(0x5407e): the islands stay where they were
local land, deco, taken = grid(0), grid(0), grid(false)

local function free(c0, r0, w, h)
    if c0 < 0 or r0 < 0 or c0 + w > COLS or r0 + h > ROWS then return false end
    for r = math.max(0, r0 - 1), math.min(ROWS, r0 + h + 1) - 1 do
        for c = math.max(0, c0 - 1), math.min(COLS, c0 + w + 1) - 1 do
            if taken[r][c] then return false end
        end
    end
    return true
end

-- The nine-slice piece for each edge: "t" or "b", then "l" or "r", or "c" alone.
local DECO = {}
for _, list in ipairs({ TREES, TREES, HOUSES, FLAGS }) do for _, g in ipairs(list) do DECO[#DECO + 1] = g end end

local function island(c0, r0, w, h, kind, decorate)
    kind = kind or GRASS
    if decorate == nil then decorate = true end
    for r = r0, r0 + h - 1 do
        for c = c0, c0 + w - 1 do
            taken[r][c] = true
            if w == 1 and h == 1 then
                land[r][c] = kind.one
            else
                local top, bottom, left, right = r == r0, r == r0 + h - 1, c == c0, c == c0 + w - 1
                local key = (top and "t" or bottom and "b" or "") .. (left and "l" or right and "r" or "")
                land[r][c] = kind[key == "" and "c" or key]
                if decorate and not (top or bottom or left or right) and rnd:random() < 0.18 then
                    deco[r][c] = kind == GRASS and rnd:choice(DECO) or CRATE
                end
            end
        end
    end
end

local json = kv.json
local objects = {}
for i, e in ipairs(ENEMIES) do
    local distance, x, kind, props = e[1], e[2], e[3], e[4]
    local px, py = map_px(distance, x)
    local c, r = math.tointeger(px // TILE), math.tointeger(py // TILE)
    local route
    for k = 1, #props, 2 do if props[k] == "route" then route = props[k + 1] end end
    if kind == "tank" then
        local w = math.max(5, math.tointeger(route / (TILE * SCALE) // 1) + 4)
        local c0 = math.max(0, math.min(COLS - w, c - w // 2))
        island(c0, r - 2, w, 5, nil, false)
        for cc = c0 + 1, c0 + w - 2 do deco[r][cc] = ROAD end
    else
        island(math.max(0, math.min(COLS - 5, c - 2)), r - 2, 5, 5, nil, false)
    end
    local properties = {}
    for k = 1, #props, 2 do
        local v = props[k + 1]
        properties[#properties + 1] = json.object("name", props[k], "type", math.type(v) == "integer" and "int" or "string", "value", v)
    end
    objects[#objects + 1] = json.object("id", i, "name", kind, "type", "", "point", true, "x", px, "y", py, "width", 0, "height", 0,
                                        "rotation", 0, "visible", true, "properties", properties)
end
-- The airfield: one long island of sand down the middle, pads and crates on it.
local _, top = map_px(AIRFIELD[2], 0)
local _, bottom = map_px(AIRFIELD[1], 0)
local r0, r1 = math.tointeger(top // TILE), math.tointeger(bottom // TILE)
island(5, r0, 7, r1 - r0, SAND, false)
for r = r0 + 2, r1 - 2, 4 do
    deco[r][8] = PAD
    deco[r + 1][6] = CRATE
end
-- The rest of the sea: islands where there is room, a few rocks of one tile.
local _, water_top = map_px(OPEN_WATER[2], 0)
local _, water_bottom = map_px(OPEN_WATER[1], 0)
for _ = 1, ROWS * 3 do
    local w, h = rnd:randint(3, 7), rnd:randint(3, 8)
    local c0, rr = rnd:randint(0, COLS - w), rnd:randint(0, ROWS - h)
    if rnd:random() < 0.3 then w, h = 1, 1 end
    if not (water_top // TILE - h <= rr and rr <= water_bottom // TILE) and free(c0, rr, w, h) then
        island(c0, rr, w, h, rnd:random() < 0.85 and GRASS or SAND)
    end
end

local function layer(id, name, data)
    local flat = {}
    for r = 0, ROWS - 1 do for c = 0, COLS - 1 do flat[#flat + 1] = data[r][c] end end
    return json.object("id", id, "name", name, "type", "tilelayer", "x", 0, "y", 0, "width", COLS, "height", ROWS,
                       "opacity", 1, "visible", true, "data", flat)
end

local tileset = json.object("type", "tileset", "version", "1.10", "tiledversion", "1.10.2", "name", "shoreline",
                            "image", "../kenney/tiles.png", "imagewidth", 192, "imageheight", 160, "tilewidth", TILE, "tileheight", TILE,
                            "columns", 12, "tilecount", 120, "margin", 0, "spacing", 0, "tiles", {})
local tmj = json.object("type", "map", "version", "1.10", "tiledversion", "1.10.2", "orientation", "orthogonal", "renderorder", "right-down",
                        "infinite", false, "width", COLS, "height", ROWS, "tilewidth", TILE, "tileheight", TILE, "compressionlevel", -1,
                        "nextlayerid", 5, "nextobjectid", #objects + 1,
                        "properties", { json.object("name", "title", "type", "string", "value", "Shoreline") },
                        "tilesets", { json.object("firstgid", 1, "source", "shoreline.tsj") },
                        "layers", { layer(1, "sea", grid(SEA)), layer(2, "land", land), layer(3, "deco", deco),
                                    json.object("id", 4, "name", "enemies", "type", "objectgroup", "draworder", "index", "x", 0, "y", 0, "opacity", 1,
                                                "visible", true, "objects", objects) })
kv.fs.mkdir(OUT)
assert(kv.fs.write(OUT .. "/shoreline.tsj", json.encode(tileset, { indent = 1 })))
assert(kv.fs.write(OUT .. "/shoreline.tmj", json.encode(tmj, { separators = { ",", ":" } })))
print(string.format("shoreline.tmj: %d x %d tiles, %d enemies", COLS, ROWS, #objects))
