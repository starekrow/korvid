-- kormake: linked tools (docs/PRDs/Kormake-Link-Tools-PRD.md). Where the shim, the links
-- and the cache live, and the tool table: for one anchor, which binary each published
-- command means and how it is brought up to date. kormake writes the table on every gen of
-- the host target; the shim (src/kvbinlink.c) only reads it.
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
  local function where(rec, script)
    if script and not c.by_name.kvscript then return { "unavailable=this engine has no kvscript module (tools/kvscript), which runs scripts" } end
    local key = rec.name .. "@" .. g.host .. "-" .. g.host_config
    local inst = g.by_key[key]
    if not inst or inst.dropped or inst.problem then
      return { "unavailable=" .. field((inst and (inst.dropped or inst.problem)) or g.unavailable[rec.name] or "not in this graph") }
    end
    local exe = g.tool_path(inst)
    local out = { "build=" .. build, "target=" .. rec.name, "exe=" .. util.join(build, exe) }
    if script then out[#out + 1] = "script=" .. script end
    return out
  end
  for _, name in ipairs(util.sorted_keys(c.commands or {})) do
    local cmd = c.commands[name]
    local winner
    for _, cand in ipairs(cmd.candidates) do
      local parts = { "tool", name, "from=" .. cand.from, "module=" .. cand.rec.name }
      if cand.script then parts[4] = "config=" .. cand.rec.file end
      if not cand.lost and not winner then
        winner = cand
        for _, w in ipairs(where(cand.rec, cand.script)) do parts[#parts + 1] = w end
      else
        parts[1] = "shadowed"
        local by = cand.lost and cand.rec.shadowed_by or winner.rec
        local by_name = (not cand.lost and winner.script) and "korvid.config" or by.name
        parts[#parts + 1] = "by=" .. (by.project and "project" or "engine") .. ":" .. by_name
        if not cand.lost then for _, w in ipairs(where(cand.rec, cand.script)) do parts[#parts + 1] = w end end
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

-- ---- the registry: what `kormake link tools` vouched for ---------------------------
-- <home>/registry.txt, tab-separated, read by the shim: engine PATH and project PATH (the
-- most recent last), link NAME BINDIR, default NAME ANCHOR.

local REGISTRY_HEADER = "korvid-link-registry\t1"

function M.read_registry(home)
  local reg = { entries = {} }
  local text = kfs.read(util.join(home, "registry.txt"))
  if not text then return reg end
  for line in text:gmatch("[^\r\n]+") do
    local f = {}
    for x in (line .. "\t"):gmatch("([^\t]*)\t") do f[#f + 1] = x end
    if f[1] ~= "korvid-link-registry" and f[2] then reg.entries[#reg.entries + 1] = { kind = f[1], a = f[2], b = f[3] } end
  end
  return reg
end

function M.write_registry(home, reg)
  local lines = { REGISTRY_HEADER }
  for _, e in ipairs(reg.entries) do lines[#lines + 1] = e.kind .. "\t" .. e.a .. (e.b and ("\t" .. e.b) or "") end
  kfs.mkdirp(home)
  local path = util.join(home, "registry.txt")
  kfs.write(path .. ".new", table.concat(lines, "\n") .. "\n")
  kfs.rename(path .. ".new", path)
end

local function find(reg, kind, a)
  for i, e in ipairs(reg.entries) do if e.kind == kind and e.a == a then return i, e end end
end

-- Adds an entry, or moves it to the end (the most recent); b replaces what it held.
local function put(reg, kind, a, b)
  local i = find(reg, kind, a)
  if i then table.remove(reg.entries, i) end
  reg.entries[#reg.entries + 1] = { kind = kind, a = a, b = b }
end

local function drop(reg, pred)
  local keep = {}
  for _, e in ipairs(reg.entries) do if not pred(e) then keep[#keep + 1] = e end end
  reg.entries = keep
end

-- ---- the shim ----------------------------------------------------------------------

local EXE = KORMAKE_HOST == "windows" and ".exe" or ""

-- The shim's version in this engine's source (src/kvbinlink.c): { protocol, revision }.
local function source_version(root)
  local src = kfs.read(util.join(root, "tools/kormake/src/kvbinlink.c")) or ""
  local p = tonumber(src:match("#define KVBINLINK_PROTOCOL (%d+)"))
  local r = tonumber(src:match("#define KVBINLINK_REVISION (%d+)"))
  if not p or not r then return nil end
  return { p, r }
end

local function version_text(v) return v and (v[1] .. "." .. v[2]) or "none" end

local function installed_version(home)
  local t = kfs.read(util.join(home, "kvbinlink.version"))
  local p, r = (t or ""):match("^(%d+)%.(%d+)")
  if not p then return nil end
  return { tonumber(p), tonumber(r) }
end

local function newer(a, b) return not b or a[1] > b[1] or (a[1] == b[1] and a[2] > b[2]) end

-- Puts this engine's shim in the link home unless the one there is as new.
local function install_shim(root, home)
  local built = util.join(root, "tools/kormake/bin/" .. KORMAKE_HOST .. "/kvbinlink" .. EXE)
  if not kfs.isfile(built) then
    util.fail("link: %s is not built; run %s", built, util.join(root, KORMAKE_HOST == "windows" and "tools/kormake/bootstrap.bat" or "tools/kormake/bootstrap.sh"))
  end
  local have, want = installed_version(home), source_version(root)
  if not want then util.fail("link: cannot read the shim's version in %s/tools/kormake/src/kvbinlink.c", root) end
  local shim = util.join(home, "kvbinlink" .. EXE)
  -- An older shim is never put over a newer one; one of the same version is replaced when
  -- it is another build (a fix within a revision).
  if kfs.isfile(shim) and (newer(have or { 0, 0 }, want) or kfs.read(shim) == kfs.read(built)) then return shim, false, have end
  kfs.mkdirp(home)
  kfs.copy(built, shim .. ".new")
  kfs.rename(shim .. ".new", shim)
  kfs.write(util.join(home, "kvbinlink.version"), version_text(want) .. "\n")
  return shim, true, want
end

-- ---- commands ----------------------------------------------------------------------

-- The anchor's collect and its host graph, with the table written: what link tools and
-- link default read the published names from.
local function context(dir)
  local collect, resolve, emit = kormake.load("collect"), kormake.load("resolve"), kormake.load("emit")
  local c = collect.run(collect.discover(dir))
  local g = resolve.run(c, KORMAKE_HOST, c.master.host_config or "dev", {})
  local build = emit.run(c, g, {})
  M.write_table(c, g, build)
  local names = { "kormake" }
  for _, n in ipairs(util.sorted_keys(c.commands or {})) do names[#names + 1] = n end
  return c, names
end

-- Whether path is a link to the shim: a symbolic link that resolves to it, or on Windows
-- a file the registry says is one.
local function ours(path, shim, reg, name, bin)
  if kfs.readlink(path) then return kfs.realpath(path) == kfs.realpath(shim) end
  if KORMAKE_HOST == "windows" then
    local _, e = find(reg, "link", name)
    return e ~= nil and e.b == bin and kfs.isfile(path)
  end
  return false
end

local function exists(path) return kfs.isfile(path) or kfs.isdir(path) or kfs.readlink(path) ~= nil end

local function on_path(bin)
  local want = kfs.realpath(bin) or bin
  local sep = KORMAKE_HOST == "windows" and ";" or ":"
  for d in ((kfs.getenv("PATH") or "") .. sep):gmatch("([^" .. sep .. "]*)" .. sep) do
    if d ~= "" and (kfs.realpath(d:gsub("\\", "/")) or d) == want then return true end
  end
  return false
end

local function path_hint(bin)
  if on_path(bin) then return end
  if KORMAKE_HOST == "windows" then
    print(string.format("kormake: %s is not on PATH; add it (Settings, or kormake link tools --path)", bin))
    return
  end
  local shell = util.basename(kfs.getenv("SHELL") or "sh")
  if shell == "fish" then
    print(string.format("kormake: %s is not on PATH; add it with: fish_add_path %s", bin, bin))
  else
    local rc = shell == "zsh" and "~/.zshrc" or shell == "bash" and "~/.bashrc" or "your shell's startup file"
    print(string.format("kormake: %s is not on PATH; add this line to %s:  export PATH=\"%s:$PATH\"", bin, rc, bin))
  end
end

local function link_tools(dir, names, opts)
  local p = M.paths(opts.system)
  if opts.bin then p.bin = util.is_abs(opts.bin) and opts.bin or util.join(kfs.cwd(), opts.bin) end
  local c, published = context(dir)
  local want = {}
  if #names == 0 then want = published else
    for _, n in ipairs(names) do
      if not util.contains(published, n) then util.fail("link tools: %s is not published here (%s)", n, table.concat(published, ", ")) end
      want[#want + 1] = n
    end
  end
  local shim, installed, v = install_shim(c.root_dir, p.home)
  print(string.format("kormake: shim %s %s in %s", version_text(v), installed and "installed" or "already", p.home))
  local reg = M.read_registry(p.home)
  put(reg, "engine", c.root_dir)
  if c.project then put(reg, "project", c.anchor) end
  kfs.mkdirp(p.bin)
  local made, kept, skipped = {}, {}, {}
  for _, n in ipairs(want) do
    local path = util.join(p.bin, n .. EXE)
    if exists(path) and ours(path, shim, reg, n, p.bin) then
      kept[#kept + 1] = n
    elseif exists(path) and not opts.force then
      skipped[#skipped + 1] = string.format("%s (%s is not a link to the shim; --force replaces it)", n, path)
    else
      if exists(path) then kfs.rmtree(path) end
      kfs.link(shim, path)
      made[#made + 1] = n
    end
    if not skipped[#skipped] or not skipped[#skipped]:find("^" .. n .. " ") then
      put(reg, "link", n, p.bin)
      drop(reg, function(e) return e.kind == "noted" and e.a == n end)
    end
  end
  M.write_registry(p.home, reg)
  local whose = c.project and ("published by " .. util.basename(c.anchor) .. " and its engine") or ("published by the engine at " .. c.root_dir)
  if #made > 0 then print(string.format("kormake: linked %s in %s (%s)", table.concat(made, " "), p.bin, whose)) end
  if #kept > 0 then print(string.format("kormake: already linked: %s", table.concat(kept, " "))) end
  for _, s in ipairs(skipped) do print("kormake: skipped " .. s) end
  path_hint(p.bin)
  return #skipped > 0 and 1 or 0
end

local function link_list()
  local p = M.paths()
  local reg = M.read_registry(p.home)
  local shim = util.join(p.home, "kvbinlink" .. EXE)
  print(string.format("shim      %s (%s)", kfs.isfile(shim) and shim or "not installed", version_text(installed_version(p.home))))
  local kinds = { { "link", "linked" }, { "engine", "engine" }, { "project", "project" }, { "default", "default" } }
  for _, k in ipairs(kinds) do
    for _, e in ipairs(reg.entries) do
      if e.kind == k[1] then
        local gone = (k[1] == "engine" or k[1] == "project") and not kfs.isdir(e.a) and "  (gone)" or ""
        if k[1] == "link" then print(string.format("%-9s %s  in %s", k[2], e.a, e.b or "?"))
        elseif k[1] == "default" then print(string.format("%-9s %s  from %s%s", k[2], e.a, e.b or "?", kfs.isdir(e.b or "") and "" or "  (gone)"))
        else print(string.format("%-9s %s%s", k[2], e.a, gone)) end
      end
    end
  end
  return 0
end

-- The shim's own report, from dir: the installed shim, or this engine's when none is.
local function link_which(dir, names)
  if #names == 0 then util.fail("link which NAME...") end
  local p = M.paths()
  local shim = util.join(p.home, "kvbinlink" .. EXE)
  if not kfs.isfile(shim) then util.fail("link which: no shim installed in %s; run kormake link tools", p.home) end
  local worst = 0
  kfs.setenv("KORVID_LINK_DIR", dir)
  for i, n in ipairs(names) do
    if i > 1 then print("") end
    local rc = kfs.exec(util.shell_quote(util.native(shim)) .. " " .. util.shell_quote(n) .. " --diagnose-tool-lookup")
    if rc ~= 0 then worst = rc end
  end
  return worst
end

-- Out of tree, NAME runs this context's tool (every name it publishes, with no NAME); with
-- --clear, it falls through to PATH again. A context not linked yet is linked first: a
-- default is a choice to trust it.
local function link_default(dir, names, opts)
  local p = M.paths(opts.system)
  if opts.clear then
    local reg = M.read_registry(p.home)
    local cleared = {}
    for _, e in ipairs(reg.entries) do
      if e.kind == "default" and (#names == 0 or util.contains(names, e.a)) then cleared[#cleared + 1] = e.a end
    end
    for _, n in ipairs(names) do if not util.contains(cleared, n) then util.fail("link default --clear: %s has no default", n) end end
    drop(reg, function(e) return e.kind == "default" and util.contains(cleared, e.a) end)
    M.write_registry(p.home, reg)
    if #cleared > 0 then print("kormake: no default now for " .. table.concat(cleared, " ")) end
    return 0
  end
  local c, published = context(dir)
  local want = #names > 0 and names or published
  local here = c.project and ("project " .. c.anchor) or ("engine " .. c.root_dir)
  for _, n in ipairs(want) do
    if not util.contains(published, n) then
      util.fail("link default: %s is not published here (%s publishes %s)", n, here, table.concat(published, ", "))
    end
  end
  local reg = M.read_registry(p.home)
  if not find(reg, "engine", c.root_dir) or (c.project and not find(reg, "project", c.anchor)) then
    local rc = link_tools(dir, {}, opts)
    if rc ~= 0 then return rc end
    reg = M.read_registry(p.home)
  end
  for _, n in ipairs(want) do put(reg, "default", n, c.anchor) end
  M.write_registry(p.home, reg)
  print(string.format("kormake: outside any tree, %s now run%s from %s", table.concat(want, " "), #want == 1 and "s" or "", here))
  return 0
end

-- gen's note, once a name: a command published here that has no link, where linked tools
-- are in use (this engine or project is linked). The registry remembers it was said.
function M.note_unlinked(c)
  local ok = pcall(function()
    local p = M.paths()
    local reg = M.read_registry(p.home)
    if not find(reg, "engine", c.root_dir) then return end
    if c.project and not find(reg, "project", c.anchor) then return end
    local fresh = {}
    for _, n in ipairs(util.sorted_keys(c.commands or {})) do
      if not find(reg, "link", n) and not find(reg, "noted", n) then fresh[#fresh + 1] = n end
    end
    if #fresh == 0 then return end
    for _, n in ipairs(fresh) do put(reg, "noted", n, c.anchor) end
    M.write_registry(p.home, reg)
    io.stderr:write(string.format("kormake: %s %s published but not linked (kormake link tools)\n", table.concat(fresh, ", "), #fresh == 1 and "is" or "are"))
  end)
  return ok
end

local function link_remove(dir, names, opts)
  local p = M.paths(opts.system)
  local reg = M.read_registry(p.home)
  local shim = util.join(p.home, "kvbinlink" .. EXE)
  local removed = {}
  for _, e in ipairs(reg.entries) do
    if e.kind == "link" and (#names == 0 or util.contains(names, e.a)) then
      local path = util.join(e.b or p.bin, e.a .. EXE)
      if exists(path) and ours(path, shim, reg, e.a, e.b) then kfs.rmtree(path) end
      removed[#removed + 1] = e.a
    end
  end
  for _, n in ipairs(names) do
    if not util.contains(removed, n) then util.fail("link remove: %s is not linked", n) end
  end
  drop(reg, function(e) return e.kind == "link" and util.contains(removed, e.a) end)
  if opts.forget then
    local collect = kormake.load("collect")
    local found = collect.discover(dir)
    local c = collect.run(found)
    local gone = { [c.root_dir] = true }
    if c.project then gone[c.anchor] = true end
    drop(reg, function(e)
      return ((e.kind == "engine" or e.kind == "project") and gone[e.a]) or (e.kind == "default" and gone[e.b or ""])
    end)
  end
  if #removed > 0 then print("kormake: unlinked " .. table.concat(removed, " ")) end
  local any = false
  for _, e in ipairs(reg.entries) do if e.kind == "link" then any = true end end
  if not any then
    kfs.rmtree(p.home)
    print("kormake: no link is left; removed " .. p.home)
    return 0
  end
  M.write_registry(p.home, reg)
  return 0
end

-- kormake link tools|list|which|remove [NAME...] [options]
function M.command(args)
  local sub = args[2]
  local names, opts = {}, {}
  local i = 3
  while i <= #args do
    local a = args[i]
    if a == "--bin" then i = i + 1; opts.bin = args[i] or util.fail("--bin needs a directory")
    elseif a == "-C" then i = i + 1; opts.dir = args[i] or util.fail("-C needs a directory")
    elseif a == "--system" then opts.system = true
    elseif a == "--force" then opts.force = true
    elseif a == "--forget" then opts.forget = true
    elseif a == "--path" then opts.path = true
    elseif a == "--clear" then opts.clear = true
    elseif a:sub(1, 1) == "-" then util.fail("link: unknown option '%s'", a)
    else names[#names + 1] = a end
    i = i + 1
  end
  local dir = opts.dir or kfs.cwd()
  if sub == "tools" then return link_tools(dir, names, opts) end
  if sub == "list" then return link_list() end
  if sub == "which" then return link_which(kfs.realpath(dir) or dir, names) end
  if sub == "remove" then return link_remove(dir, names, opts) end
  if sub == "default" then return link_default(dir, names, opts) end
  util.fail("usage: kormake link tools|list|which|default|remove [NAME...] (tools/kormake/README.md)")
end

return M
