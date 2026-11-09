-- kormake: collect. Evaluate the master file, scan the module paths, evaluate every
-- module.lua, apply the directory convention. Output: a list of module records.
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

function M.run(root_dir)
  local master_path = util.join(root_dir, "kormake.lua")
  local text = kfs.read(master_path)
  if not text then util.fail("no master file at %s", master_path) end
  local master = sandbox.eval_master(master_path, text)
  if master.requires then
    local want_major = tonumber(master.requires:match("^(%d+)")) or 0
    local have_major = tonumber(KORMAKE_VERSION:match("^(%d+)")) or 0
    if want_major > have_major then util.fail("%s requires kormake %s, this is %s", master_path, master.requires, KORMAKE_VERSION) end
  end

  local paths = {}
  for _, p in ipairs(master.modulepaths) do paths[#paths + 1] = util.expand_home(p) end
  local envpath = kfs.getenv("KORMAKE_MODULE_PATH")
  -- Entries are separated as in PATH: ':' on POSIX, ';' on Windows (where ':' is a drive's).
  local sep = KORMAKE_HOST == "windows" and ";" or ":"
  if envpath then for p in envpath:gmatch("[^" .. sep .. "]+") do paths[#paths + 1] = util.expand_home(p) end end

  local modules, by_name = {}, {}
  for _, path in ipairs(paths) do
    local full = util.is_abs(path) and path or util.join(root_dir, path)
    if not kfs.isdir(full) then util.fail("module path '%s' is not a directory", path) end
    local names = kfs.listdir(full)
    table.sort(names)
    for _, name in ipairs(names) do
      local dir = util.join(full, name)
      local mfile = util.join(dir, "module.lua")
      if kfs.isdir(dir) and kfs.isfile(mfile) then
        local mtext = kfs.read(mfile)
        local decl = sandbox.eval_module(mfile, mtext)
        local rec = { name = decl.name, dir = dir, file = mfile, fields = decl.fields }
        local override = master.overrides[decl.name]
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
  return { master = master, modules = modules, by_name = by_name, root_dir = root_dir }
end

return M
