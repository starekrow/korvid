-- Cuts arcfire's boss, enemy and effect art out of the CC0 packs. Run from anywhere:
--
--     kormake run examples/arcfire/tools/cut_art.lua ~/code/asset-packs
--
-- The argument is the directory that holds the packs as received (assets/CREDITS.md names
-- each pack and its authors). Writes assets/generated/ (the bosses' hulls and parts, the
-- missile boat, Orbit's debris, segments and capital ship) and these in assets/made/: egg,
-- eggs, items, beam, clouds_low, clouds_high. Every texture keeps the size and cell layout
-- the game and the boss files expect, so nothing in the sim changes. Ported from
-- cut_art.py (Pillow): the operations are kvscript's, which do as Pillow does.

local HERE = (arg[0]:match("^(.*)[/\\][^/\\]*$") or ".") .. "/../assets"

-- ---- small things Python had --------------------------------------------------------------

-- Python's round: halves to even.
local function round(v)
    local f = math.floor(v)
    local d = v - f
    if d > 0.5 or (d == 0.5 and f % 2 == 1) then f = f + 1 end
    return math.tointeger(f)
end

local function ends_with(s, tail) return tail == "" or s:sub(-#tail) == tail end

-- ---- reading the packs ----------------------------------------------------------------

local function from_zip(path, member)
    local z = kv.zip.open(path)
    for _, name in ipairs(z:names()) do
        if ends_with(name, member) and not name:find("__MACOSX", 1, true) then return kv.image.decode(z:read(name)) end
    end
    error(path .. ": no " .. member)
end

-- im scaled to fit size, keeping its aspect, centred on a clear canvas.
local function fit(im, sw, sh, smooth)
    if smooth == nil then smooth = true end
    local r = math.min(sw / im.width, sh / im.height)
    local w, h = math.max(1, round(im.width * r)), math.max(1, round(im.height * r))
    im = im:resize(w, h, smooth and "lanczos" or "nearest")
    return kv.image.new(sw, sh):composite(im, (sw - w) // 2, (sh - h) // 2)
end

local function centred(im, sw, sh, dx, dy)
    return kv.image.new(sw, sh):composite(im, (sw - im.width) // 2 + (dx or 0), (sh - im.height) // 2 + (dy or 0))
end

-- Bounding boxes {x0, y0, x1, y1} of the opaque shapes in im (alpha over 16), largest first;
-- shapes of fewer than min_px pixels are left out.
local function components(im, min_px)
    min_px = min_px or 200
    local w, h = im.width, im.height
    local alpha = im:channel(4)
    local byte = string.byte
    local seen = {}
    local boxes = {}
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            local i = y * w + x
            if byte(alpha, i + 1) > 16 and not seen[i] then
                local stack = { i }
                seen[i] = true
                local x0, x1, y0, y1, n = x, x, y, y, 0
                while #stack > 0 do
                    local p = table.remove(stack)
                    local cx, cy = p % w, p // w
                    n = n + 1
                    if cx < x0 then x0 = cx end
                    if cx > x1 then x1 = cx end
                    if cy < y0 then y0 = cy end
                    if cy > y1 then y1 = cy end
                    local function try(nx, ny)
                        if nx >= 0 and nx < w and ny >= 0 and ny < h then
                            local q = ny * w + nx
                            if not seen[q] and byte(alpha, q + 1) > 16 then
                                seen[q] = true
                                stack[#stack + 1] = q
                            end
                        end
                    end
                    try(cx + 1, cy)
                    try(cx - 1, cy)
                    try(cx, cy + 1)
                    try(cx, cy - 1)
                end
                if n >= min_px then boxes[#boxes + 1] = { box = { x0, y0, x1 + 1, y1 + 1 }, n = n, order = #boxes } end
            end
        end
    end
    -- Largest first; Python's sort is stable, so ties keep the order they were found in.
    table.sort(boxes, function(a, b) if a.n ~= b.n then return a.n > b.n end return a.order < b.order end)
    local out = {}
    for i, b in ipairs(boxes) do out[i] = b.box end
    return out
end

-- The largest shape in box (grown by pad), cut to its bounds.
local function trimmed(im, box, pad)
    pad = pad or 8
    local x0, y0, x1, y1 = box[1], box[2], box[3], box[4]
    local c = im:crop(math.max(0, x0 - pad), math.max(0, y0 - pad), math.min(im.width, x1 + pad), math.min(im.height, y1 + pad))
    local b = components(c, 1)[1]
    return c:crop(b[1], b[2], b[3], b[4])
end

-- ---- surt's modular ships: the parts, the gunships, the missile boat --------------------

-- surt's sheet is four colorways of one kit: orange at the top left, blue at the top right
-- (x + 288), white below the orange (y + 144); the ships he assembled from it below them.
-- A piece is named by its place in the orange quadrant.
local BLUE, WHITE = { 288, 0 }, { 0, 144 }
local PIECE = {
    turret_twin  = { 112, 136, 16, 16 }, -- a turret with two barrels
    turret_one   = { 144, 136, 16, 16 }, -- a turret with one barrel
    turret_small = { 72, 136, 8, 16 },   -- a small gun with two barrels
    block        = { 152, 56, 16, 16 },  -- a plain hull block: armored, shut
    block_b      = { 168, 56, 16, 16 },
    block_c      = { 184, 56, 16, 16 },
    dome         = { 64, 32, 16, 16 },   -- a cockpit under a dome: a core laid open
    dome_tall    = { 112, 32, 16, 16 },  -- a taller dome
    dome_small   = { 32, 40, 8, 8 },     -- a small domed pod
    grille       = { 264, 112, 16, 16 }, -- a frame of white struts: a bay or hatch open
    vent         = { 280, 112, 16, 16 }, -- a block over vents
    thruster     = { 248, 112, 16, 16 }, -- a block with its works showing
    flat         = { 96, 64, 16, 8 },    -- a flat plate: a missile bay shut
}
local SHIP_BLUE = { 208, 328, 256, 392 }  -- the long blue ship with the red dome, 48 x 64
local SHIP_WHITE = { 152, 336, 200, 400 } -- the white ship with two pods, 48 x 64
local SHIP_TALL = { 208, 16, 224, 48 }    -- the tall orange ship, 16 x 32

local function piece(sheet, name, cw)
    cw = cw or { 0, 0 }
    local p = PIECE[name]
    local x, y = p[1] + cw[1], p[2] + cw[2]
    return sheet:crop(x, y, x + p[3], y + p[4])
end

-- A parts sheet of 24 px cells: cells is a list of {col, row, image}, each centred, flipped
-- top to bottom for a boss that faces down.
local function parts_sheet(cols, rows, cells, flip)
    local out = kv.image.new(cols * 24, rows * 24)
    for _, cell in ipairs(cells) do
        local im = cell[3]
        if flip then im = im:flip("vertical") end
        out:composite(centred(im, 24, 24), cell[1] * 24, cell[2] * 24)
    end
    return out
end

-- ---- the bosses ---------------------------------------------------------------------------

local packs = arg[1]
if not packs or arg[2] then error("usage: kormake run examples/arcfire/tools/cut_art.lua <asset-packs directory>", 0) end
packs = packs:gsub("^~", os.getenv("HOME") or os.getenv("USERPROFILE") or "~")
local gen, made = HERE .. "/generated", HERE .. "/made"

local surt = kv.image.load(packs .. "/modular_ships/modular_ships.png"):key(0, 128, 192)
local ares_green = from_zip(packs .. "/arcfire/ARES sprites.zip", "ARES Spritesheet.png")
local ares_red = from_zip(packs .. "/arcfire/ARES sprites.zip", "ARES Spritesheet2.png")
local tip = kv.image.load(packs .. "/arcfire/darkships/tipone.png")
local remastered = packs .. "/arcfire/kenney_space-shooter-remastered.zip"
local extension = packs .. "/arcfire/kenney_spaceShooterExtension.zip"
local boulders = kv.image.load(packs .. "/shiny-boulders/boulders.png"):key(32, 23, 32)
local clouds = kv.image.load(packs .. "/clouds/clouds1/clouds.png")
local function p(name, cw) return piece(surt, name, cw) end

-- ARES (Binngi): the capsule hull and the ring station, in green and red; rocks.
local capsule = { 1200, 1212, 1576, 2008 }
local ring = { 68, 96, 1160, 1200 }
local rocks = { { 1852, 696, 1944, 804 }, { 1752, 788, 1820, 848 }, { 1944, 792, 2036, 888 } }

-- The carrier: ARES's red capsule, bow down (the core sits at its cabin).
local hull = trimmed(ares_red, capsule):flip("vertical")
fit(hull, 96, 176):save(gen .. "/carrier.png")
parts_sheet(4, 2, { { 0, 0, p("turret_twin") }, { 1, 0, p("turret_twin") }, { 2, 0, p("block") }, { 3, 0, p("grille") },
                    { 0, 1, p("block_b") }, { 1, 1, p("dome") } }, true):save(gen .. "/carrier_parts.png")

-- The battleship: ARES's green capsule, broadside, bow right.
hull = trimmed(ares_green, capsule):rotate(-90, "nearest", true)
fit(hull, 224, 96):save(gen .. "/battleship.png")
parts_sheet(4, 3, { { 0, 0, p("turret_twin") }, { 1, 0, p("turret_one") }, { 2, 0, p("turret_one") }, { 3, 0, p("flat") },
                    { 0, 1, p("vent") }, { 1, 1, p("block_c") }, { 2, 1, p("thruster") }, { 3, 1, p("block") },
                    { 0, 2, p("dome_tall") } }):save(gen .. "/battleship_parts.png")

-- The gunships: surt's white and blue ships, bow down; their parts from their colorways.
for _, g in ipairs({ { "left", SHIP_WHITE, WHITE }, { "right", SHIP_BLUE, BLUE } }) do
    local side, box, cw = g[1], g[2], g[3]
    local ship = surt:crop(box[1], box[2], box[3], box[4]):flip("vertical")
    centred(ship, 48, 80):save(gen .. "/gunship_" .. side .. ".png")
    parts_sheet(2, 2, { { 0, 0, p("turret_small", cw) }, { 1, 0, p("turret_small", cw) },
                        { 0, 1, p("block", cw) }, { 1, 1, p("dome", cw) } }, true):save(gen .. "/gunship_" .. side .. "_parts.png")
end

-- The station: ARES's red ring, turned an eighth so its spokes run under the weapons.
local station = trimmed(ares_red, ring):rotate(45, "bicubic", true)
fit(station:crop(station:bbox()), 128, 128):save(gen .. "/station.png")
parts_sheet(4, 2, { { 0, 0, p("dome_small") }, { 1, 0, p("block") }, { 2, 0, p("turret_twin") },
                    { 0, 1, p("block_b") }, { 1, 1, p("dome") } }):save(gen .. "/station_parts.png")

-- The missile boat: surt's tall orange ship, nose up, doubled.
local boat = surt:crop(SHIP_TALL[1], SHIP_TALL[2], SHIP_TALL[3], SHIP_TALL[4]):resize(32, 64, "nearest")
centred(boat, 32, 72):save(gen .. "/boat.png")

-- Orbit's debris: three of ARES's small rocks.
local deb = kv.image.new(72, 24)
for i, box in ipairs(rocks) do deb:composite(fit(trimmed(ares_green, box), 22, 22), (i - 1) * 24 + 1, 1) end
deb:save(gen .. "/debris.png")

-- Orbit's segments: Kenney's station pieces, a truss with solar panels and a module.
local seg = kv.image.new(96, 96)
seg:composite(fit(from_zip(extension, "PNG/Sprites/Station/spaceStation_022.png"), 96, 48), 0, 0)
seg:composite(fit(from_zip(extension, "PNG/Sprites/Station/spaceStation_001.png"), 96, 48), 0, 48)
seg:save(gen .. "/segments.png")

-- The capital ship: Commander's long ship, broadside, bow left.
local cap = trimmed(tip, { 295, 27, 393, 489 }):rotate(90, "nearest", true)
fit(cap, 224, 40):save(gen .. "/capital.png")

-- ---- the effects ---------------------------------------------------------------------

-- The egg: SpiderDave's blue boulder; the HUD's pips the same, spent (dim) and in hand.
local egg = boulders:crop(16, 0, 32, 16)
egg:save(made .. "/egg.png")
local eggs = kv.image.new(32, 16)
eggs:composite(egg:scale(0.4, 0.4, 0.4, 1), 0, 0)
eggs:composite(egg, 16, 0)
eggs:save(made .. "/eggs.png")

-- The items: Kenney's badges with the weapons' letters in Public Pixel, then the power
-- (Kenney's gold bolt) and the option (his silver star).
local font = kv.font.load(HERE .. "/font/PublicPixel.ttf", 8)
local items = kv.image.new(96, 16)
for i, b in ipairs({ { "S", "Red" }, { "B", "Blue" }, { "T", "Green" }, { "L", "Yellow" } }) do
    local badge = fit(from_zip(remastered, "PNG/Power-ups/powerup" .. b[2] .. ".png"), 16, 16)
    font:draw(badge, b[1], 8, 8, 40, 40, 48, 255, "mm")
    font:draw(badge, b[1], 8, 7, 255, 255, 255, 255, "mm")
    items:composite(badge, (i - 1) * 16, 0)
end
items:composite(fit(from_zip(remastered, "PNG/Power-ups/bolt_gold.png"), 16, 16), 64, 0)
items:composite(fit(from_zip(remastered, "PNG/Power-ups/star_silver.png"), 16, 16), 80, 0)
items:save(made .. "/items.png")

-- The Beam's strip: Kenney's blue laser, stood in the strip's middle.
local laser = from_zip(remastered, "PNG/Lasers/laserBlue01.png"):resize(12, 64, "lanczos")
centred(laser, 16, 64):save(made .. "/beam.png")

-- The clouds: Sam and 魔大农's five clouds, laid on seamless 512 px tiles (each drawn at
-- the wrap's nine offsets). Low: the clouds whole; high: smaller, stretched thin, half
-- transparent.
local shapes = {}
for i, b in ipairs(components(clouds, 4000)) do
    if i > 5 then break end
    shapes[i] = clouds:crop(b[1], b[2], b[3], b[4])
end

local function tile(placements, alpha)
    local big = kv.image.new(1536, 1536)
    for _, pl in ipairs(placements) do
        local s, x, y, scale, stretch = shapes[pl[1] + 1], pl[2], pl[3], pl[4], pl[5]
        s = s:resize(math.max(1, round(s.width * scale * stretch)), math.max(1, round(s.height * scale)), "lanczos")
        if alpha < 1 then s = s:scale(1, 1, 1, alpha) end
        for _, ox in ipairs({ 0, 512, 1024 }) do
            for _, oy in ipairs({ 0, 512, 1024 }) do big:composite(s, x + ox, y + oy) end
        end
    end
    return big:crop(512, 512, 1024, 1024)
end

-- Scaled so they cover what the drawn ones did (about 6% of the low tile, 4% of the
-- high): the field must stay readable under them.
tile({ { 0, 40, 40, 0.22, 1.0 }, { 1, 300, 150, 0.20, 1.0 }, { 3, 110, 320, 0.26, 1.0 }, { 2, 360, 420, 0.22, 1.0 } }, 1.0):save(made .. "/clouds_low.png")
tile({ { 2, 20, 80, 0.20, 1.8 }, { 4, 330, 20, 0.22, 1.6 }, { 1, 200, 260, 0.18, 1.9 }, { 3, 420, 380, 0.17, 1.7 } }, 0.5):save(made .. "/clouds_high.png")
-- Kenney's two packs' own license files, beside the credits.
for _, l in ipairs({ { remastered, "LICENSE-Kenney-SpaceShooter.txt" }, { extension, "LICENSE-Kenney-SpaceShooterExtension.txt" } }) do
    local z = kv.zip.open(l[1])
    for _, name in ipairs(z:names()) do
        if ends_with(name:lower(), "license.txt") then
            assert(kv.fs.write(HERE .. "/" .. l[2], z:read(name)))
            break
        end
    end
end
print("cut_art: generated/ and made/ written")
