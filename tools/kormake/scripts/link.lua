-- kormake: linked tools (docs/PRDs/Kormake-Link-Tools-PRD.md). Where the shim, the links
-- and the cache live, and the tool table: for one anchor, which binary each published
-- command means and how it is brought up to date. kormake writes the table on every gen of
-- the host target; the shim (src/kvlink.c) only reads it.
local util = kormake.load("util")
local M = {}

M.FORMAT = 1

local function env(name)
  local v = kfs.getenv(name)
  if v and v ~= "" then return (v:gsub("\\", "/")) end
  return nil
end

local function home()
  return env("HOME") or env("USERPROFILE") or util.fail("link: neither HOME nor USERPROFILE is set")
end

-- The link home (the shim and the registry), the cache (the tool tables) and the bin
-- directory (the links), each with its override. system: the --system locations.
function M.paths(system)
  local p = {}
  if KORMAKE_HOST == "windows" then
    local base = env("LOCALAPPDATA") or util.join(home(), "AppData/Local")
    p.home = util.join(base, "korvid-link")
    p.cache = util.join(base, "korvid-link/cache")
    p.bin = util.join(base, "korvid-link/bin")
  elseif KORMAKE_HOST == "macos" then
    p.home = util.join(home(), "Library/Application Support/korvid-link")
    p.cache = util.join(home(), "Library/Caches/korvid-link")
    p.bin = util.join(home(), ".local/bin")
  else
    p.home = util.join(env("XDG_DATA_HOME") or util.join(home(), ".local/share"), "korvid-link")
    p.cache = util.join(env("XDG_CACHE_HOME") or util.join(home(), ".cache"), "korvid-link")
    p.bin = util.join(home(), ".local/bin")
  end
  if system then
    if KORMAKE_HOST == "windows" then util.fail("link: --system is for Linux and macOS") end
    p.home, p.bin = "/usr/local/lib/korvid-link", "/usr/local/bin"
  end
  p.home = env("KORVID_LINK_HOME") or p.home
  p.cache = env("KORVID_LINK_CACHE") or p.cache
  p.bin = env("KORVID_LINK_BIN") or p.bin
  return p
end

-- FNV-1a, 64 bits, as 16 hex digits: the name of an anchor's table. Not a secret, only a
-- spread; the table repeats the anchor's path, and the shim checks it.
function M.hash(s)
  local h = 0xcbf29ce484222325
  for i = 1, #s do
    h = h ~ s:byte(i)
    h = h * 0x100000001b3
  end
  return string.format("%016x", h)
end

function M.table_path(cache, anchor)
  return util.join(cache, "roots/" .. M.hash(anchor) .. ".txt")
end

-- The engine's own kormake, which the bootstrap builds: what `kormake` means in its tree.
function M.kormake_of(root)
  return util.join(root, "tools/kormake/bin/" .. KORMAKE_HOST .. "/kormake" .. (KORMAKE_HOST == "windows" and ".exe" or ""))
end

local function field(v) return (tostring(v):gsub("[\t\r\n]+", " ")) end

-- The table's text for this collect and the host target's graph (g, built in `build`).
function M.table_text(c, g, build)
  local lines = {}
  local function line(...) lines[#lines + 1] = table.concat({ ... }, "\t") end
  line("korvid-link", tostring(M.FORMAT))
  line("anchor", c.anchor)
  line("engine", c.root_dir)
  if c.project then line("project", c.anchor) end
  line("kormake", M.kormake_of(c.root_dir))
  line("ninja", g.ninja or "ninja")
  -- What makes the table out of date when it changes: the files that say what the anchor
  -- is. A module's own file is not here: a change there regenerates build.ninja, which
  -- writes the table again.
  local watched = {}
  if c.config then watched[#watched + 1] = c.config end
  watched[#watched + 1] = util.join(c.root_dir, "kormake.lua")
  for _, f in ipairs(watched) do line("watch", f, tostring(kfs.size(f) or -1), tostring(kfs.mtime(f) or -1)) end
  line("tool", "kormake", "from=engine", "exe=" .. M.kormake_of(c.root_dir))
  local function where(rec)
    local key = rec.name .. "@" .. g.host .. "-" .. g.host_config
    local inst = g.by_key[key]
    if not inst or inst.dropped or inst.problem then
      return { "unavailable=" .. field((inst and (inst.dropped or inst.problem)) or g.unavailable[rec.name] or "not in this graph") }
    end
    local exe = g.tool_path(inst)
    return { "build=" .. build, "target=" .. rec.name, "exe=" .. util.join(build, exe) }
  end
  for _, name in ipairs(util.sorted_keys(c.commands or {})) do
    local cmd = c.commands[name]
    local winner
    for _, cand in ipairs(cmd.candidates) do
      local parts = { "tool", name, "from=" .. cand.from, "module=" .. cand.rec.name }
      if not cand.lost and not winner then
        winner = cand
        for _, w in ipairs(where(cand.rec)) do parts[#parts + 1] = w end
      else
        parts[1] = "shadowed"
        local by = cand.lost and cand.rec.shadowed_by or winner.rec
        parts[#parts + 1] = "by=" .. (by.project and "project" or "engine") .. ":" .. by.name
        if not cand.lost then for _, w in ipairs(where(cand.rec)) do parts[#parts + 1] = w end end
      end
      line(table.unpack(parts))
    end
  end
  return table.concat(lines, "\n") .. "\n"
end

-- Writes the anchor's table when it changed, by a temporary file and a rename, so a shim
-- never reads half of one. A cache that cannot be written is a warning: the build goes on.
function M.write_table(c, g, build)
  local ok, err = pcall(function()
    local p = M.paths()
    local path = M.table_path(p.cache, c.anchor)
    local text = M.table_text(c, g, build)
    if kfs.read(path) == text then return end
    kfs.mkdirp(util.dirname(path))
    local tmp = path .. ".new"
    kfs.write(tmp, text)
    kfs.rename(tmp, path)
  end)
  if not ok then io.stderr:write("kormake: warning: cannot write the tool table: " .. util.message(err) .. "\n") end
  return ok
end

return M
