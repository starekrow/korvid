-- Arcfire's art: the vendored files, copied unmodified under new names, and the pieces made
-- here: cuts from the packs and plain shapes. The bosses, the egg, the clouds, the beam and
-- the items come from the newer packs through tools/cut_art.lua. Run from anywhere:
--
--     kormake run examples/arcfire/tools/make_art.lua ~/code/asset-packs/arcfire
--
-- The argument is the directory the archives were downloaded and unpacked into (`pack/`
-- under it holds one directory per archive). Writes examples/arcfire/assets/: the vendored
-- files under their new names (VENDORED, below), and everything made here under `made/`,
-- then prints each archive's SHA-256. Ported from make_art.py (Pillow): its pictures come
-- out the same to the pixel.
--
-- What is made here, and from what:
--   made/bullets.png   64 x 32: two 32 px cells cut from M484's Bullet Collection 1 (the
--                      round bullet with the red rim, the cyan needle), its black ground
--                      keyed to transparent; nothing else changed
--   made/dot.png       8 x 8 white disc: the hitbox
--   made/ring.png      48 x 48 white ring, 1 px: the graze ring
--   made/soft.png      64 x 64 white disc fading to its edge: the shell, glows
--   made/spark.png     4 x 4 white: sparks
--   made/bombs.png     32 x 16 the HUD's bomb pips: Kenney's shield tile (tiles_packed,
--                      column 2, row 2), dimmed for a bomb spent, as it is for one in hand
--   made/vignette.png  135 x 180: the field's vignette, clear in the middle and darkening
--                      to its edges and corners (drawn 4 times up, over the field, under the HUD)
--   made/cursor.png    32 x 32: the menus' cursor, Kenney's player ship (ships_packed,
--                      column 0, row 0) turned to point right; nothing else changed

local HERE = (arg[0]:match("^(.*)[/\\][^/\\]*$") or ".") .. "/../assets"

-- (vendored file under the pack directory, where it goes)
local VENDORED = {
    { "pack/kenney_pixelshmup/Tilemap/ships_packed.png", "kenney/ships.png" },
    { "pack/kenney_pixelshmup/Tilemap/tiles_packed.png", "kenney/tiles.png" },
    { "pack/kenney_pixelshmup/License.txt", "LICENSE-Kenney.txt" },
    { "pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Starfields/Starfield 8 - 512x512.png", "space/stars.png" },
    { "pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Purple Nebula/Purple Nebula 4 - 512x512.png", "space/nebula.png" },
    { "pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/Small 512x512/Blue Nebula/Blue Nebula 5 - 512x512.png", "space/cloud.png" },
    { "pack/sbs_-_seamless_space_backgrounds_-_small_512x512_0/License.txt", "LICENSE-ScreamingBrainStudios.txt" },
    { "pack/public_pixel_font_1_24/PublicPixel.ttf", "font/PublicPixel.ttf" },
    { "pack/public_pixel_font_1_24/License.txt", "LICENSE-CC0.txt" },
}

-- Cells of M484's sheet (x, y, w, h), measured as the connected shapes on its black ground.
local M484_ROUND = { 462, 193, 18, 17 } -- white core, red rim
local M484_NEEDLE = { 13, 66, 6, 10 }   -- cyan, upright

-- Python's round: halves to even.
local function round(v)
    local f = math.floor(v)
    local d = v - f
    if d > 0.5 or (d == 0.5 and f % 2 == 1) then f = f + 1 end
    return math.tointeger(f)
end

local function dir_of(path) return path:match("^(.*)/[^/]*$") or "." end

local function bullets(pack)
    local src = kv.image.load(pack .. "/M484BulletCollection1.png")
    local sheet = kv.image.new(64, 32)
    for cell, b in ipairs({ M484_ROUND, M484_NEEDLE }) do
        local x, y, w, h = b[1], b[2], b[3], b[4]
        -- The black ground keyed out: the sheet is opaque, its shapes never pure black. Then
        -- pasted through its own alpha, which is all or nothing: the same as over.
        local piece = src:crop(x, y, x + w, y + h):key(0, 0, 0)
        sheet:composite(piece, (cell - 1) * 32 + (32 - w) // 2, (32 - h) // 2)
    end
    return sheet
end

local function disc(size, radius, soft)
    local im = kv.image.new(size, size)
    local c = (size - 1) / 2.0
    for y = 0, size - 1 do
        for x = 0, size - 1 do
            local d = math.sqrt((x - c) ^ 2 + (y - c) ^ 2)
            if soft then
                local a = math.max(0.0, 1.0 - d / radius)
                im:set(x, y, 255, 255, 255, math.floor(255 * a * a))
            elseif d <= radius then
                im:set(x, y, 255, 255, 255, 255)
            end
        end
    end
    return im
end

local function ring(size)
    local im = kv.image.new(size, size)
    local c = (size - 1) / 2.0
    local r = size / 2.0 - 1.0
    for y = 0, size - 1 do
        for x = 0, size - 1 do
            if math.abs(math.sqrt((x - c) ^ 2 + (y - c) ^ 2) - r) < 0.6 then im:set(x, y, 255, 255, 255, 255) end
        end
    end
    return im
end

local function vignette(w, h)
    local im = kv.image.new(w, h)
    for y = 0, h - 1 do
        for x = 0, w - 1 do
            local dx, dy = (x + 0.5) / w * 2.0 - 1.0, (y + 0.5) / h * 2.0 - 1.0
            local r = math.sqrt(dx * dx * 0.85 + dy * dy) -- an ellipse a little wider than the field
            local a = math.max(0.0, math.min(1.0, (r - 0.62) / 0.6)) ^ 1.6
            im:set(x, y, 4, 6, 16, round(a * 150))
        end
    end
    return im
end

local function cursor(pack)
    local ships = kv.image.load(pack .. "/pack/kenney_pixelshmup/Tilemap/ships_packed.png")
    return ships:crop(0, 0, 32, 32):rotate(270, "nearest", true)
end

local pack = arg[1]
if not pack or arg[2] then error("usage: kormake run examples/arcfire/tools/make_art.lua <asset-packs/arcfire>", 0) end
pack = pack:gsub("^~", os.getenv("HOME") or os.getenv("USERPROFILE") or "~")
for _, v in ipairs(VENDORED) do
    local out = HERE .. "/" .. v[2]
    kv.fs.mkdir(dir_of(out))
    assert(kv.fs.write(out, assert(kv.fs.read(pack .. "/" .. v[1]))))
end
local made = HERE .. "/made"
kv.fs.mkdir(made)
bullets(pack):save(made .. "/bullets.png")
disc(8, 3.6):save(made .. "/dot.png")
ring(48):save(made .. "/ring.png")
disc(64, 31.5, true):save(made .. "/soft.png")
kv.image.new(4, 4, 255, 255, 255, 255):save(made .. "/spark.png")
cursor(pack):save(made .. "/cursor.png")
vignette(135, 180):save(made .. "/vignette.png")
local tiles = kv.image.load(pack .. "/pack/kenney_pixelshmup/Tilemap/tiles_packed.png")
local shield = tiles:crop(32, 32, 48, 48)
local dim = shield:copy()
for y = 0, 15 do
    for x = 0, 15 do
        local r, g, b, a = dim:get(x, y)
        dim:set(x, y, r // 3, g // 3, b // 3 + 20, a // 2)
    end
end
local bombs = kv.image.new(32, 16)
bombs:paste(dim, 0, 0)
bombs:paste(shield, 16, 0)
bombs:save(made .. "/bombs.png")
-- Each archive's SHA-256: the zips, then the loose pictures, by name.
local zips, pngs = {}, {}
for _, name in ipairs(assert(kv.fs.list(pack))) do
    if not name:find("/", 1, true) then
        if name:match("%.zip$") then zips[#zips + 1] = name elseif name:match("%.png$") then pngs[#pngs + 1] = name end
    end
end
table.sort(zips)
table.sort(pngs)
for _, list in ipairs({ zips, pngs }) do
    for _, name in ipairs(list) do print(name .. ": sha256 " .. kv.sha256(assert(kv.fs.read(pack .. "/" .. name)))) end
end
