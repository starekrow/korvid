-- kormake: evaluation of module.lua and kormake.lua files. Each runs in an environment
-- that holds the declaration functions and the string, table and math libraries, and
-- nothing that reaches the file system, the OS or other modules. The result is plain data.
local util = kormake.load("util")
local M = {}

local allowed = {
  "pairs", "ipairs", "next", "type", "tostring", "tonumber", "select", "error", "assert",
  "string", "table", "math", "utf8", "rawget", "rawset", "rawequal", "setmetatable", "getmetatable",
}

local function base_env()
  local env = {}
  for _, name in ipairs(allowed) do env[name] = _G[name] end
  return env
end

local function copy_data(v, depth)
  depth = depth or 0
  if depth > 16 then util.fail("declaration nests too deep") end
  if type(v) == "table" then
    local out = {}
    for k, x in pairs(v) do
      if type(k) ~= "string" and type(k) ~= "number" then util.fail("a declaration key must be a string or a number") end
      out[k] = copy_data(x, depth + 1)
    end
    return out
  elseif type(v) == "string" or type(v) == "number" or type(v) == "boolean" or v == nil then
    return v
  else
    util.fail("a declaration may hold only strings, numbers, booleans and tables, not %s", type(v))
  end
end

local module_fields = {
  output = true, deps = true, provides = true, platforms = true, external = true, tools = true,
  exports = true, sources = true, defines = true, prebuilt = true, system = true, bindings = true,
  platform = true, unity = true, engine_module = true, module_list = true, licenses = true, exe = true, private_includes = true,
  tag = true, srcdirs = true, cxx = true, cxxflags = true, ldflags = true, configs = true, ar = true, externals = true,
  -- A toolchain's own: the linker, its compile/archive/link rules, how it names outputs,
  -- how it spells flags, and a probe that finds the tools on the host.
  link = true, rules = true, obj_suffix = true, exe_suffix = true, lib_file = true,
  flag_include = true, flag_define = true, flag_lib = true, flag_libdir = true, flag_include_system = true, probe = true,
  sdk = true, -- the Apple SDK the xcode probe asks xcrun for ("macosx")
  flag_framework = true, -- how a toolchain links an external's frameworks ("-framework %s")
  package = true, -- "app": kormake package writes an application bundle (package_app.lua)
  os_min = true,  -- the oldest OS release the toolchain builds for ("26.0"), for what packaging writes
  -- A toolchain that serves several platforms: per_platform[<platform>] is laid over its
  -- fields for that platform (resolve.lua says how); triple is the target's
  -- "<vendor>-<os>[-<env>]" without the architecture ("apple-ios26.0-simulator"), family
  -- names the platforms' common rule variants ("apple"), and test_runner is a command a
  -- test executable runs under when the host cannot run it directly ("%{root}" in it is
  -- the project's root).
  per_platform = true, triple = true, family = true, test_runner = true,
  test_jobs = true, -- at most this many tests at once, for a runner that cannot run more
}

-- module "name" { fields } or module "name" (defaults). One per file.
function M.eval_module(path, text)
  local env = base_env()
  local result = nil
  local rules = {}
  env.module = function(name)
    if type(name) ~= "string" or name == "" then util.fail("%s: module needs a name", path) end
    if result then util.fail("%s: a second module declaration ('%s' after '%s')", path, name, result.name) end
    result = { name = name, fields = {} }
    return function(fields)
      if type(fields) ~= "table" then util.fail("%s: module '%s' takes a table", path, name) end
      for k, v in pairs(fields) do
        if type(k) ~= "string" or not module_fields[k] then util.fail("%s: module '%s' has an unknown field '%s'", path, name, tostring(k)) end
        result.fields[k] = copy_data(v)
      end
    end
  end
  -- rule "name" { inputs, outputs, command, deps } inside exports.
  env.rule = function(name)
    return function(fields)
      local r = copy_data(fields)
      r.name = name
      return r
    end
  end
  local chunk, err = load(text, "=" .. path, "t", env)
  if not chunk then util.fail("%s", err) end
  local ok, perr = pcall(chunk)
  if not ok then util.fail("%s", tostring(perr)) end
  if not result then util.fail("%s declares no module", path) end
  return result
end

-- The master file: project, requires_kormake, modulepath, platforms, configs, host_config, bind, root, override.
function M.eval_master(path, text)
  local env = base_env()
  local m = { modulepaths = {}, platforms = {}, configs = {}, binds = {}, roots = {}, overrides = {} }
  env.project = function(name) m.project = name end
  env.requires_kormake = function(v) m.requires = v end
  env.modulepath = function(t) for _, p in ipairs(util.as_list(t)) do m.modulepaths[#m.modulepaths + 1] = p end end
  env.platforms = function(t) m.platforms = copy_data(util.as_list(t)) end
  env.configs = function(t) m.configs = copy_data(util.as_list(t)) end
  env.host_config = function(c) if type(c) ~= "string" then util.fail("%s: host_config takes a config name", path) end m.host_config = c end
  env.bind = function(iface) return function(t) m.binds[iface] = copy_data(t) end end
  env.root = function(name) return function(t) m.roots[#m.roots + 1] = { name = name, fields = copy_data(t or {}) } end end
  env.override = function(name) return function(t) m.overrides[name] = copy_data(t) end end
  local chunk, err = load(text, "=" .. path, "t", env)
  if not chunk then util.fail("%s", err) end
  local ok, perr = pcall(chunk)
  if not ok then util.fail("%s", tostring(perr)) end
  return m
end

-- A korvid.config: engine, modulepath, build, target. Paths are relative to the file.
function M.eval_config(path, text)
  local env = base_env()
  local m = {}
  local function str(what)
    return function(v)
      if type(v) ~= "string" or v == "" then util.fail("%s: %s takes a string", path, what) end
      m[what] = v
    end
  end
  env.engine, env.build, env.target = str("engine"), str("build"), str("target")
  env.modulepath = function(t)
    m.modulepaths = m.modulepaths or {}
    for _, p in ipairs(util.as_list(t)) do m.modulepaths[#m.modulepaths + 1] = p end
  end
  local chunk, err = load(text, "=" .. path, "t", env)
  if not chunk then util.fail("%s", err) end
  local ok, perr = pcall(chunk)
  if not ok then util.fail("%s", tostring(perr)) end
  return m
end

return M
