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

-- What any module may set.
local module_fields = {
  output = true, deps = true, provides = true, platforms = true, external = true, tools = true,
  exports = true, sources = true, defines = true, prebuilt = true, system = true,
  unity = true, engine_module = true, module_list = true, licenses = true, exe = true, private_includes = true,
  -- codegen: a code-generation profile by name (the toolchain's codegen_profiles spell it),
  -- for the module's own sources, not its tests. isa_sources: sources built again for each
  -- instruction set the toolchain names in `isa` ({ avx2 = { "src/raster.cc" } }).
  codegen = true, isa_sources = true,
  test_deps = true, -- modules the module's tests use and it does not, linked into its test only
}
-- What only a toolchain (provides = "toolchain") may set, at its top or in a per_platform
-- section: its tag and source directories, the compiler, archiver and linker with their
-- flags (configs: per config), what each external means (externals) and the default
-- backend of each interface (bindings), its compile/archive/link rules, how it names
-- outputs and spells flags, and a probe that finds the tools on the host.
local toolchain_fields = {
  tag = true, srcdirs = true, cxx = true, ar = true, link = true, cxxflags = true, ldflags = true, configs = true,
  externals = true, bindings = true, rules = true, obj_suffix = true, exe_suffix = true, lib_file = true,
  flag_include = true, flag_define = true, flag_lib = true, flag_libdir = true, flag_include_system = true, flags_generated = true, codegen_profiles = true, isa = true, probe = true,
  sdk = true, -- the Apple SDK the xcode probe asks xcrun for ("macosx")
  flag_framework = true, -- how a toolchain links an external's frameworks ("-framework %s")
  package = true, -- "app" or "iosapp": kormake package writes an application bundle (package_app.lua); "apk": an Android package (package_apk.lua)
  os_min = true,  -- the oldest OS release the toolchain builds for ("26.0"), for what packaging writes
  -- A toolchain that serves several platforms: per_platform[<platform>] is laid over its
  -- fields for that platform (resolve.lua says how); triple is the target's
  -- "<vendor>-<os>[-<env>]" without the architecture ("apple-ios26.0-simulator"), family
  -- names the platforms' common rule variants ("apple"), and test_runner is a command a
  -- test executable runs under when the host cannot run it directly ("%{root}" in it is
  -- the project's root).
  per_platform = true, triple = true, family = true, test_runner = true,
  test_jobs = true, -- at most this many tests at once, for a runner that cannot run more
  -- arch is the target's architecture as its compiler spells it ("aarch64", "x86_64"),
  -- for a toolchain whose platforms differ in it; ndk is the Android NDK version the
  -- android probe looks for ("30.0.16248370").
  arch = true, ndk = true,
  -- An application the OS loads as a library (Android): an executable whose closure holds
  -- the module app_module names (the window's) is linked as app_file ("lib%s.so") with
  -- app_ldflags after the toolchain's own; every other executable, the tests among them,
  -- stays a program. abi is the directory such a library has in a package ("arm64-v8a"),
  -- build_tools the SDK build tools the apk packager runs ("36.0.0"), target_api the API
  -- level it is checked against ("36").
  app_module = true, app_file = true, app_ldflags = true, abi = true, build_tools = true, target_api = true,
}

-- What a rule "name" { ... } in exports may set. configs: the configs whose default build
-- holds a per-exe rule's outputs (kpack's pack: ship); they are built by name in the rest.
-- bound, wrap, glue and engine_module: a rule whose output is compiled into the module
-- (emit.lua's generated_sources).
local rule_fields = { inputs = true, outputs = true, command = true, deps = true, per = true, configs = true, platforms = true,
                      bound = true, wrap = true, glue = true, engine_module = true, codegen = true }

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
      local toolchain = fields.provides == "toolchain"
      for k, v in pairs(fields) do
        if type(k) ~= "string" or not (module_fields[k] or toolchain_fields[k]) then
          util.fail("%s: module '%s' has an unknown field '%s'", path, name, tostring(k))
        end
        if toolchain_fields[k] and not toolchain then
          util.fail("%s: module '%s' sets '%s', which only a toolchain (provides = \"toolchain\") may set", path, name, k)
        end
        result.fields[k] = copy_data(v)
      end
      for plat, sec in pairs(result.fields.per_platform or {}) do
        if type(sec) ~= "table" then util.fail("%s: per_platform.%s takes a table", path, tostring(plat)) end
        for k in pairs(sec) do
          if not toolchain_fields[k] or k == "per_platform" then
            util.fail("%s: module '%s': per_platform.%s sets '%s', which is not a toolchain field", path, name, tostring(plat), tostring(k))
          end
        end
      end
    end
  end
  -- rule "name" { inputs, outputs, command, deps, per, configs, platforms, bound, wrap, glue,
  -- engine_module } inside exports.
  -- A per-exe rule (per = "exe") runs over an executable's staged assets, so its inputs
  -- are "assets", in each platform variant too.
  env.rule = function(name)
    return function(fields)
      if type(name) ~= "string" or name == "" or type(fields) ~= "table" then util.fail("%s: rule needs a name and a table", path) end
      local r = copy_data(fields)
      local function check(t, where)
        for k in pairs(t) do
          if not rule_fields[k] or (k == "platforms" and where ~= "") then util.fail("%s: rule '%s'%s has an unknown field '%s'", path, name, where, tostring(k)) end
        end
      end
      check(r, "")
      for p, v in pairs(r.platforms or {}) do check(v, " (platforms." .. tostring(p) .. ")") end
      local variants = { r }
      for _, v in pairs(r.platforms or {}) do variants[#variants + 1] = v end
      for _, v in ipairs(variants) do
        local per, inputs = v.per or r.per, v.inputs or r.inputs
        if per ~= nil and per ~= "exe" then util.fail("%s: rule '%s': per takes \"exe\"", path, name) end
        if per == "exe" and inputs ~= "assets" then util.fail("%s: rule '%s': per-exe rules take inputs = \"assets\"", path, name) end
      end
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
  env.cook = function(v) if type(v) ~= "string" or v == "" then util.fail("%s: cook takes a command line", path) end m.cook = v end
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
  env.engine, env.build, env.target, env.cook = str("engine"), str("build"), str("target"), str("cook")
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
