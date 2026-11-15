-- kormake: collect. Find the engine root (a kormake.lua, or a korvid.config naming one),
-- evaluate the master file, scan the module paths, evaluate every module.lua, apply the
-- directory convention. Output: a list of module records and where things go.
local util = kormake.load("util")
local sandbox = kormake.load("sandbox")
local M = {}

local function convention(rec)
  local dir = rec.dir
  rec.include = kfs.isdir(util.join(dir, "include")) and util.join(dir, "include") or nil
  rec.src_dir = kfs.isdir(util.join(dir, "src")) and util.join(dir, "src") or nil
  rec.tests_dir = kfs.isdir(util.join(dir, "tests")) and util.join(dir, "tests") or nil
  rec.shaders_dir = kfs.isdir(util.join(dir, "shaders")) and util.join(dir, "shaders") or nil
  rec.assets_dir = kfs.isdir(util.join(dir, "assets")) and util.join(dir, "assets") or nil
end

-- Whether path is dir or lies under it (both real paths).
function M.under(path, dir)
  if path == dir then return true end
  local d = dir:sub(-1) == "/" and dir or (dir .. "/")
  return path:sub(1, #d) == d
end

-- Walks up from start: the first directory with a korvid.config or a kormake.lua settles
-- it. A kormake.lua is the engine root itself; a korvid.config names it (engine "..").
-- The anchor is the directory the file was found in.
function M.discover(start)
  local from = kfs.realpath(start) or util.fail("no directory '%s'", start)
  local dir = from
  while true do
    local cfg = util.join(dir, "korvid.config")
    if kfs.isfile(cfg) then
      local decl = sandbox.eval_config(cfg, kfs.read(cfg))
      if not decl.engine then util.fail("%s names no engine; add engine \"<path to the engine's root>\"", cfg) end
      local e = util.expand_home(decl.engine)
      local root = kfs.realpath(util.is_abs(e) and e or util.join(dir, e))
      if not root or not kfs.isfile(util.join(root, "kormake.lua")) then
        util.fail("%s: engine '%s' is not a directory with a kormake.lua", cfg, decl.engine)
      end
      return { root = root, anchor = dir, start = from, config = cfg, decl = decl }
    end
    if kfs.isfile(util.join(dir, "kormake.lua")) then return { root = dir, anchor = dir, start = from } end
    local parent = util.dirname(dir)
    if parent == dir or parent == "" or parent == "." or dir == "/" or dir:match("^%a:/?$") then
      util.fail("no korvid.config or kormake.lua in %s or any parent; a project outside the engine needs a " ..
        "korvid.config that names it (engine \"../korvid\")", from)
    end
    dir = parent
  end
end

-- The cook when nothing names one: goldberg, built for the host, over every asset root
-- of the program at once.
M.DEFAULT_COOK = "goldberg cook --roots {roots} --stamp {stamp} --quiet"

-- found: what discover returned.
function M.run(found)
  local root_dir = found.root
  local decl = found.decl or {}
  local master_path = util.join(root_dir, "kormake.lua")
  local text = kfs.read(master_path)
  if not text then util.fail("no master file at %s", master_path) end
  local master = sandbox.eval_master(master_path, text)
  if master.requires then
    local want_major = tonumber(master.requires:match("^(%d+)")) or 0
    local have_major = tonumber(KORMAKE_VERSION:match("^(%d+)")) or 0
    if want_major > have_major then util.fail("%s requires kormake %s, this is %s", master_path, master.requires, KORMAKE_VERSION) end
  end

  -- Module paths: the master file's (relative to the root), the config's (relative to it;
  -- its own directory unless it says otherwise), then KORMAKE_MODULE_PATH. Each directory
  -- once, by its real path.
  local paths, seen_path = {}, {}
  local project_paths = {} -- the paths the config adds that the master file does not list
  local function add(p, base, from_config)
    p = util.expand_home(p)
    local full = util.is_abs(p) and p or util.join(base, p)
    local real = kfs.realpath(full)
    if not real or not kfs.isdir(real) then util.fail("module path '%s' is not a directory", p) end
    if seen_path[real] then return end
    seen_path[real] = true
    paths[#paths + 1] = real
    if from_config then project_paths[real] = true end
  end
  for _, p in ipairs(master.modulepaths) do add(p, root_dir) end
  if found.config then for _, p in ipairs(decl.modulepaths or { "." }) do add(p, found.anchor, true) end end
  local envpath = kfs.getenv("KORMAKE_MODULE_PATH")
  -- Entries are separated as in PATH: ':' on POSIX, ';' on Windows (where ':' is a drive's).
  local sep = KORMAKE_HOST == "windows" and ";" or ":"
  if envpath then for p in envpath:gmatch("[^" .. sep .. "]+") do add(p, root_dir) end end

  -- What a command line may call a target or a phony: never a module's name.
  local reserved = { host = true, all = true, tests = true, check = true, ["build.ninja"] = true }
  for _, cf in ipairs(master.configs) do
    reserved[cf] = true
    reserved["host-" .. cf] = true
    for _, p in ipairs(master.platforms) do reserved[p .. "-" .. cf] = true end
  end

  local modules, by_name = {}, {}
  for _, full in ipairs(paths) do
    local names = kfs.listdir(full)
    table.sort(names)
    for _, name in ipairs(names) do
      local dir = util.join(full, name)
      local mfile = util.join(dir, "module.lua")
      if kfs.isdir(dir) and kfs.isfile(mfile) then
        local mtext = kfs.read(mfile)
        local mdecl = sandbox.eval_module(mfile, mtext)
        local rec = { name = mdecl.name, dir = dir, file = mfile, fields = mdecl.fields, project = project_paths[full],
                      modulepath = full }
        if reserved[rec.name] then util.fail("%s: a module may not be called '%s' (a target or a build name)", mfile, rec.name) end
        local override = master.overrides[mdecl.name]
        if override and override.path then
          -- An override names the directory that wins; other copies are ignored.
          local want = util.is_abs(override.path) and override.path or util.join(root_dir, override.path)
          if kfs.realpath(want) ~= kfs.realpath(dir) then rec = nil end
        end
        if rec then
          if by_name[rec.name] then
            util.fail("module '%s' is declared in both %s and %s; add override \"%s\" { path = ... } to the master file",
              rec.name, by_name[rec.name].dir, dir, rec.name)
          end
          convention(rec)
          by_name[rec.name] = rec
          modules[#modules + 1] = rec
        end
      end
    end
  end
  -- A project: a config whose paths bring in modules the master file's do not. A config
  -- that adds none (in examples/, or at the root to set a target) is the engine's.
  local added = false
  for _, rec in ipairs(modules) do if rec.project then added = true end end
  -- Where outputs go: the engine root's build/ and dist/ while the module paths are the
  -- master file's (the configs in examples/ and tools/); beside the config when it adds
  -- one (a project of its own, whose modules are then the roots: resolve.lua). build "dir"
  -- names the directory that holds <target>/.
  local base = added and found.anchor or root_dir
  local build_root = util.join(base, "build")
  if decl.build then
    local b = util.expand_home(decl.build)
    build_root = util.is_abs(b) and b or util.join(found.anchor, b)
    -- clean removes what is under it, so it must not hold the project or the engine.
    local real = kfs.realpath(build_root) or build_root
    if M.under(found.anchor, real) or M.under(root_dir, real) then
      util.fail("%s: build '%s' would hold %s; name a directory of its own", found.config, decl.build,
        M.under(found.anchor, real) and found.anchor or root_dir)
    end
  end
  return { master = master, modules = modules, by_name = by_name, root_dir = root_dir, base_dir = base,
           build_root = build_root, anchor = found.anchor, start = found.start, config = found.config,
           default_target = decl.target, project = added,
           -- Where a regeneration starts: the config's directory when the config changes the
           -- build (its paths, its build directory), else the root, so a build file written
           -- from examples/ is the one written from the root.
           regen_dir = (added or decl.build or decl.cook) and found.anchor or root_dir,
           regen_config = (added or decl.build or decl.cook) and found.config or nil,
           -- What cooks a program's assets: the config's cook, the master file's, or goldberg.
           -- `dir` is where a script it names is looked for.
           cook = decl.cook and { text = decl.cook, dir = found.anchor, file = found.config }
             or master.cook and { text = master.cook, dir = root_dir, file = master_path }
             or { text = M.DEFAULT_COOK, dir = root_dir, file = "kormake's default" } }
end

return M
