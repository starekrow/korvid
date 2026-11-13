-- Arcfire's sounds and music: the vendored effects, copied unmodified under new names, and
-- the two music tracks, resampled. Run from anywhere:
--
--     kormake run examples/arcfire/tools/make_sounds.lua ~/code/asset-packs/arcfire
--
-- The argument is the directory the archives were downloaded and unpacked into (`pack/`
-- under it holds one directory per archive). Writes examples/arcfire/assets/sound/
-- (Kenney's Sci-Fi Sounds and Impact Sounds, CC0, as they are) and assets/music/ (Juhani
-- Junkala's 5 Chiptunes (Action), CC0: four tracks, Shoreline's and Orbit's, folded to mono
-- and halved in rate, 22,050 Hz 16-bit WAV, about a quarter of the size; the engine
-- streams only Ogg Vorbis, and nothing here encodes it). assets/CREDITS.md names the packs
-- and their authors. Ported from make_sounds.py: the WAVs come out byte for byte the same.

local ASSETS = (arg[0]:match("^(.*)[/\\][^/\\]*$") or ".") .. "/../assets"
local SCIFI = "pack/kenney_sci-fi-sounds/Audio"
local IMPACT = "pack/kenney_impact-sounds/Audio"
local MUSIC = "pack/juhani_5_action_chiptunes"

-- (here, the pack, in the pack): what each sound is for in the game.
local EFFECTS = {
    { "shot.ogg", SCIFI, "laserSmall_000.ogg" },                 -- the ship's volley
    { "hit.ogg", IMPACT, "impactPlate_light_000.ogg" },          -- a shot on an enemy
    { "ping.ogg", IMPACT, "impactMetal_light_001.ogg" },         -- a shot on armor
    { "kill.ogg", SCIFI, "explosionCrunch_000.ogg" },            -- popcorn shot down
    { "kill_big.ogg", SCIFI, "explosionCrunch_002.ogg" },        -- a ground row or a boss's part
    { "boss_down.ogg", SCIFI, "explosionCrunch_004.ogg" },       -- a boss's core
    { "rumble.ogg", SCIFI, "lowFrequency_explosion_000.ogg" },   -- the bomb's blast, a hull breaking
    { "graze.ogg", SCIFI, "laserRetro_000.ogg" },                -- a bullet through the ring
    { "chip.ogg", SCIFI, "laserRetro_002.ogg" },                 -- an item picked up
    { "bomb.ogg", SCIFI, "forceField_000.ogg" },                 -- the bomb's flash
    { "egg_break.ogg", SCIFI, "lowFrequency_explosion_001.ogg" }, -- the ship breaks
    { "egg_open.ogg", IMPACT, "impactMetal_heavy_000.ogg" },     -- the egg falls away in halves
    { "egg_bloom.ogg", SCIFI, "forceField_002.ogg" },            -- the new ship, the shell
    { "warning.ogg", SCIFI, "computerNoise_000.ogg" },           -- a boss comes; a boss stomps
    { "ui_move.ogg", SCIFI, "laserRetro_001.ogg" },              -- a menu's focus moves
    { "ui_select.ogg", SCIFI, "forceField_001.ogg" },            -- a menu item chosen
    { "ui_cancel.ogg", SCIFI, "doorClose_001.ogg" },             -- back out of a menu
}

local TRACKS = {
    { "stage.wav", "Juhani Junkala [Retro Game Music Pack] Level 1.wav" },
    { "boss.wav", "Juhani Junkala [Retro Game Music Pack] Level 3.wav" },
    { "orbit.wav", "Juhani Junkala [Retro Game Music Pack] Level 2.wav" },
    { "orbit_boss.wav", "Juhani Junkala [Retro Game Music Pack] Ending.wav" },
}

local function copy(from, to)
    local bytes = assert(kv.fs.read(from))
    assert(kv.fs.write(to, bytes))
end

-- A PCM WAV's format and its samples (the data chunk's bytes).
local function read_wav(path)
    local s = assert(kv.fs.read(path))
    assert(s:sub(1, 4) == "RIFF" and s:sub(9, 12) == "WAVE", path .. ": not a WAV")
    local at, fmt, data = 13, nil, nil
    while at + 8 <= #s + 1 do
        local id, size = s:sub(at, at + 3), string.unpack("<I4", s, at + 4)
        local body = at + 8
        if id == "fmt " then
            local tag, channels, rate, _, _, bits = string.unpack("<I2I2I4I4I2I2", s, body)
            fmt = { tag = tag, channels = channels, rate = rate, bits = bits }
        elseif id == "data" then
            data = s:sub(body, body + size - 1)
        end
        at = body + size + (size % 2) -- chunks are padded to even sizes
    end
    assert(fmt and data, path .. ": no fmt or data chunk")
    return fmt, data
end

-- 16-bit stereo to 16-bit mono at half the rate: each output sample the mean of two
-- frames' four samples (a box filter before the decimation), floored as Python's // is.
local function halve_to_mono(src, dst)
    local fmt, pcm = read_wav(src)
    assert(fmt.tag == 1 and fmt.bits == 16 and fmt.channels == 2, src)
    local n = #pcm // 8 * 8 -- whole groups of four samples
    local out, batch = {}, 256 -- groups unpacked and packed at a time
    local unpack, pack = string.unpack, string.pack
    local ufmt = "<" .. string.rep("i2", 4 * batch)
    local pfmt = "<" .. string.rep("i2", batch)
    local at = 1
    while at <= n do
        local groups = math.min(batch, (n - at + 1) // 8)
        local v = { unpack(groups == batch and ufmt or "<" .. string.rep("i2", 4 * groups), pcm, at) }
        local m = {}
        for g = 0, groups - 1 do
            local b = g * 4
            m[g + 1] = (v[b + 1] + v[b + 2] + v[b + 3] + v[b + 4]) // 4
        end
        out[#out + 1] = pack(groups == batch and pfmt or "<" .. string.rep("i2", groups), table.unpack(m))
        at = at + groups * 8
    end
    local samples = table.concat(out)
    local rate = fmt.rate // 2
    -- The header Python's wave module writes: RIFF, a 16-byte fmt chunk, data.
    local header = "RIFF" .. pack("<I4", 36 + #samples) .. "WAVE" .. "fmt " ..
        pack("<I4I2I2I4I4I2I2", 16, 1, 1, rate, rate * 2, 2, 16) .. "data" .. pack("<I4", #samples)
    assert(kv.fs.write(dst, header .. samples))
end

local pack = arg[1]
if not pack or arg[2] then error("usage: kormake run examples/arcfire/tools/make_sounds.lua <asset-packs/arcfire>", 0) end
pack = pack:gsub("^~", os.getenv("HOME") or os.getenv("USERPROFILE") or "~")
kv.fs.mkdir(ASSETS .. "/sound")
kv.fs.mkdir(ASSETS .. "/music")
for _, e in ipairs(EFFECTS) do copy(pack .. "/" .. e[2] .. "/" .. e[3], ASSETS .. "/sound/" .. e[1]) end
copy(pack .. "/" .. SCIFI .. "/../License.txt", ASSETS .. "/LICENSE-Kenney-SciFiSounds.txt")
copy(pack .. "/" .. IMPACT .. "/../License.txt", ASSETS .. "/LICENSE-Kenney-ImpactSounds.txt")
for _, t in ipairs(TRACKS) do halve_to_mono(pack .. "/" .. MUSIC .. "/" .. t[2], ASSETS .. "/music/" .. t[1]) end
copy(pack .. "/" .. MUSIC .. "/INFO.txt", ASSETS .. "/LICENSE-JuhaniJunkala.txt")
print(string.format("%d effects, %d tracks", #EFFECTS, #TRACKS))
