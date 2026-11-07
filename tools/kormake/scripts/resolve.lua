-- kormake: resolve. Roots become instances of (module, platform, config); link edges
-- follow deps, an interface dep also links its bound backend, tool edges switch to the
-- host platform. Cycles, missing modules, unsupported platforms, unknown externals and
-- unbound interfaces are errors that name the path.
local util = kormake.load("util")
local M = {}

local function has_platform(rec, platform)
  local ps = rec.fields.platforms
  if not ps then return true end
  return util.contains(ps, platform)
end

function M.run(c, platform, config, opts)
  local master, by_name = c.master, c.by_name
  if not util.contains(master.platforms, platform) then util.fail("platform '%s' is not in the master file's platforms", platform) end
  if not util.contains(master.configs, config) then util.fail("config '%s' is not in the master file's configs", config) end

  -- The toolchain for this platform.
  local tc = nil
  for _, rec in ipairs(c.modules) do
    if rec.fields.provides == "toolchain" and has_platform(rec, platform) then
      if tc then util.fail("two toolchains for platform '%s': '%s' and '%s'", platform, tc.name, rec.name) end
      tc = rec
    end
  end
  if not tc then util.fail("no toolchain module provides platform '%s'", platform) end

  -- Interfaces: whatever some module provides. Bindings: the toolchain's defaults, then
  -- the master file's overrides for this platform.
  local interfaces = {}
  for _, rec in ipairs(c.modules) do
    local p = rec.fields.provides
    if p and p ~= "toolchain" then interfaces[p] = interfaces[p] or {}; interfaces[p][#interfaces[p] + 1] = rec.name end
  end
  local bindings = {}
  for iface, backend in pairs(tc.fields.bindings or {}) do bindings[iface] = util.as_list(backend) end
  for iface, per_platform in pairs(master.binds) do
    local b = per_platform[platform]
    if b then bindings[iface] = util.as_list(b) end
  end
  for iface, backends in pairs(bindings) do
    for _, b in ipairs(backends) do
      local rec = by_name[b]
      if not rec then util.fail("binding for '%s' on %s names unknown module '%s'", iface, platform, b) end
      if rec.fields.provides ~= iface then util.fail("'%s' is bound to '%s' on %s but provides '%s'", b, iface, platform, tostring(rec.fields.provides)) end
      if not has_platform(rec, platform) then util.fail("'%s' is bound to '%s' on %s but does not support that platform", b, iface, platform) end
    end
  end

  local g = { platform = platform, config = config, toolchain = tc, bindings = bindings, interfaces = interfaces,
              instances = {}, by_key = {}, order = {}, host = platform }

  local visiting = {}
  local stack = {}

  local function instance_of(name, plat, conf, why)
    local key = name .. "@" .. plat .. "-" .. conf
    if g.by_key[key] then
      if visiting[key] then
        local path = {}
        local from = nil
        for i, k in ipairs(stack) do if k == key then from = i end end
        for i = from or 1, #stack do path[#path + 1] = g.by_key[stack[i]].module.name end
        path[#path + 1] = name
        util.fail("dependency cycle: %s", table.concat(path, " -> "))
      end
      return g.by_key[key]
    end
    local rec = by_name[name]
    if not rec then util.fail("module '%s' is needed by %s but does not exist", name, why) end
    if not has_platform(rec, plat) then util.fail("module '%s' (needed by %s) does not support platform '%s'", name, why, plat) end
    local inst = { module = rec, platform = plat, config = conf, key = key, deps = {}, tools = {}, why = why, tests = false }
    g.by_key[key] = inst
    g.instances[#g.instances + 1] = inst
    visiting[key] = true
    stack[#stack + 1] = key

    for _, dep in ipairs(rec.fields.deps or {}) do
      local d = instance_of(dep, plat, conf, name)
      inst.deps[#inst.deps + 1] = d
      -- An interface: whoever uses it links the backend too, unless this is the backend.
      if interfaces[dep] and rec.fields.provides ~= dep then
        local bound = bindings[dep]
        if not bound then util.fail("'%s' depends on interface '%s' but no backend is bound for %s", name, dep, plat) end
        for _, b in ipairs(bound) do
          local bi = instance_of(b, plat, conf, name .. " (via interface " .. dep .. ")")
          if not util.contains(inst.deps, bi) then inst.deps[#inst.deps + 1] = bi end
        end
      end
    end
    for ext in pairs(rec.fields.external or {}) do
      if not (tc.fields.externals or {})[ext] then util.fail("'%s' needs external '%s', which toolchain '%s' does not know", name, ext, tc.name) end
    end
    -- Tools: an interface or a tool module, instanced for the host in its release config.
    local tools = rec.fields.tools or {}
    for k, v in pairs(tools) do
      local tname, spec = nil, {}
      if type(k) == "number" then tname = v else tname = k; spec = type(v) == "table" and v or {} end
      local tmod = tname
      if interfaces[tname] then
        local bound = bindings[tname]
        if not bound then util.fail("'%s' needs tool interface '%s' but no backend is bound for %s", name, tname, plat) end
        tmod = bound[1]
      end
      local ti = instance_of(tmod, g.host, "ship", name .. " (tool)")
      inst.tools[#inst.tools + 1] = { instance = ti, spec = spec, name = tname }
    end

    stack[#stack] = nil
    visiting[key] = nil
    g.order[#g.order + 1] = inst -- post-order: dependencies first
    return inst
  end

  -- Roots.
  local roots = master.roots
  if #roots == 0 then roots = { { name = "all", fields = { modules = "*" } } } end
  local wanted = {}
  for _, root in ipairs(roots) do
    local patterns = root.fields.modules and util.as_list(root.fields.modules) or { root.name }
    for _, rec in ipairs(c.modules) do
      for _, pat in ipairs(patterns) do
        -- Toolchains and tools that are not built from source are never roots of a target build.
        if util.matches(rec.name, pat) and has_platform(rec, platform) and rec.fields.provides ~= "toolchain"
           and not rec.fields.system and not rec.fields.prebuilt then
          local inst = instance_of(rec.name, platform, config, "root '" .. root.name .. "'")
          wanted[#wanted + 1] = inst
          if root.fields.tests and rec.tests_dir then inst.tests = true end
          if not util.contains(inst.roots or {}, root.name) then inst.roots = inst.roots or {}; inst.roots[#inst.roots + 1] = root.name end
        end
      end
    end
  end
  if #wanted == 0 then util.fail("no module matched any root") end

  -- The closure of each instance's link deps in dependency order, for include paths,
  -- link lines and module lists.
  for _, inst in ipairs(g.instances) do
    local seen, list = {}, {}
    local function walk(i)
      if seen[i.key] then return end
      seen[i.key] = true
      for _, d in ipairs(i.deps) do walk(d) end
      list[#list + 1] = i
    end
    for _, d in ipairs(inst.deps) do walk(d) end
    inst.closure = list -- dependencies first, the instance itself excluded
  end
  return g
end

return M
