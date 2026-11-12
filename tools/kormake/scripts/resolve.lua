-- kormake: resolve. Roots become instances of (module, platform, config); link edges
-- follow deps, an interface dep also links its bound backend, tool edges switch to the
-- host: the machine kormake runs on (KORMAKE_HOST) in the master file's host_config (dev
-- by default). A module with "host" in its platforms is built only for the host, whoever
-- asks. When the host is the target (linux-dev on Linux) the two share instances; when it
-- is not, the host instances are a second set in the same graph, under host/ in the build
-- directory, with the host toolchain's flags. Cycles, missing modules, unsupported
-- platforms, unknown externals and unbound interfaces are errors that name the path.
local util = kormake.load("util")
local M = {}

local function host_only(rec)
  return util.contains(rec.fields.platforms or {}, "host")
end

-- Whether a module builds for a platform. A host module builds for the host only; any
-- other names in its platforms restrict which hosts.
local function has_platform(rec, platform)
  local ps = rec.fields.platforms
  if not ps then return true end
  if host_only(rec) then
    if platform ~= KORMAKE_HOST then return false end
    return #ps == 1 or util.contains(ps, platform)
  end
  return util.contains(ps, platform)
end

function M.run(c, platform, config, opts)
  local master, by_name = c.master, c.by_name
  if not util.contains(master.platforms, platform) then util.fail("platform '%s' is not in the master file's platforms", platform) end
  if not util.contains(master.configs, config) then util.fail("config '%s' is not in the master file's configs", config) end

  local host_platform = KORMAKE_HOST
  local host_config = master.host_config or "dev"
  if not util.contains(master.configs, host_config) then util.fail("host_config '%s' is not in the master file's configs", host_config) end

  -- Interfaces: whatever some module provides.
  local interfaces = {}
  for _, rec in ipairs(c.modules) do
    local p = rec.fields.provides
    if p and p ~= "toolchain" then interfaces[p] = interfaces[p] or {}; interfaces[p][#interfaces[p] + 1] = rec.name end
  end

  -- A toolchain as one of its platforms sees it: its fields with per_platform[<plat>] laid
  -- over them. In the section a list is appended to the toolchain's list of the same name
  -- (flags, source directories), a map is merged key by key (externals, bindings) and
  -- anything else replaces. The result is a toolchain of its own, named for the platform,
  -- so a build that holds two platforms of one module (a target and the host) keeps them
  -- apart.
  local function is_list(t) return next(t) == nil or t[1] ~= nil end
  local function for_platform(rec, plat)
    local sec = rec.fields.per_platform and rec.fields.per_platform[plat]
    if not sec then return rec end
    local fields = {}
    for k, v in pairs(rec.fields) do if k ~= "per_platform" then fields[k] = v end end
    for k, v in pairs(sec) do
      local base = fields[k]
      if type(v) == "table" and type(base) == "table" then
        local out = {}
        if is_list(v) and is_list(base) then
          for _, x in ipairs(base) do out[#out + 1] = x end
          for _, x in ipairs(v) do out[#out + 1] = x end
        else
          for kk, x in pairs(base) do out[kk] = x end
          for kk, x in pairs(v) do out[kk] = x end
        end
        fields[k] = out
      else
        fields[k] = v
      end
    end
    local out = {}
    for k, v in pairs(rec) do out[k] = v end
    out.name, out.fields = rec.name .. "." .. plat, fields
    return out
  end

  -- Per platform (the target's, and the host's when it differs): the toolchain, and the
  -- bindings, the toolchain's defaults then the master file's overrides.
  local per_platform = {}
  local function platform_of(plat)
    if per_platform[plat] then return per_platform[plat] end
    local tc = nil
    for _, rec in ipairs(c.modules) do
      if rec.fields.provides == "toolchain" and has_platform(rec, plat) then
        if tc then util.fail("two toolchains for platform '%s': '%s' and '%s'", plat, tc.name, rec.name) end
        tc = rec
      end
    end
    if not tc then util.fail("no toolchain module provides platform '%s'", plat) end
    for p in pairs(tc.fields.per_platform or {}) do
      if not has_platform(tc, p) then util.fail("toolchain '%s' has a section for '%s', which is not in its platforms", tc.name, p) end
    end
    tc = for_platform(tc, plat)
    local bindings = {}
    for iface, backend in pairs(tc.fields.bindings or {}) do bindings[iface] = util.as_list(backend) end
    for iface, per in pairs(master.binds) do
      local b = per[plat]
      if b then bindings[iface] = util.as_list(b) end
    end
    for iface, backends in pairs(bindings) do
      for _, b in ipairs(backends) do
        local rec = by_name[b]
        if not rec then util.fail("binding for '%s' on %s names unknown module '%s'", iface, plat, b) end
        if rec.fields.provides ~= iface then util.fail("'%s' is bound to '%s' on %s but provides '%s'", b, iface, plat, tostring(rec.fields.provides)) end
        if not has_platform(rec, plat) then util.fail("'%s' is bound to '%s' on %s but does not support that platform", b, iface, plat) end
      end
    end
    per_platform[plat] = { toolchain = tc, bindings = bindings }
    return per_platform[plat]
  end
  local tc = platform_of(platform).toolchain
  local bindings = platform_of(platform).bindings

  local g = { platform = platform, config = config, toolchain = tc, bindings = bindings, interfaces = interfaces,
              instances = {}, by_key = {}, order = {}, host = host_platform, host_config = host_config,
              platform_of = platform_of }

  local visiting = {}
  local stack = {}

  local function instance_of(name, plat, conf, why)
    local rec = by_name[name]
    if not rec then util.fail("module '%s' is needed by %s but does not exist", name, why) end
    if host_only(rec) then plat, conf = host_platform, host_config end
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
    if not has_platform(rec, plat) then util.fail("module '%s' (needed by %s) does not support platform '%s'", name, why, plat) end
    local pp = platform_of(plat)
    -- Outputs: the target's at the build directory's top, the host's (when it is not the
    -- target) under host/.
    local prefix = (plat == platform and conf == config) and "" or "host/"
    -- Selectable: what a command line can name, with a phony of its module's name; the
    -- target's instance, or a host module's only one.
    local inst = { module = rec, platform = plat, config = conf, key = key, deps = {}, tools = {}, why = why, tests = false,
                   prefix = prefix, toolchain = pp.toolchain, host = (plat == host_platform and conf == host_config),
                   selectable = (prefix == "" or host_only(rec)) and rec.fields.provides ~= "toolchain"
                     and not rec.fields.system and not rec.fields.prebuilt }
    g.by_key[key] = inst
    g.instances[#g.instances + 1] = inst
    visiting[key] = true
    stack[#stack + 1] = key

    for _, dep in ipairs(rec.fields.deps or {}) do
      local drec = by_name[dep]
      if drec and host_only(drec) and not host_only(rec) then
        util.fail("'%s' links '%s', which is built for the host only; name it in tools instead", name, dep)
      end
      local d = instance_of(dep, plat, conf, name)
      inst.deps[#inst.deps + 1] = d
      -- An interface: whoever uses it links the backend too, unless this is the backend.
      if interfaces[dep] and rec.fields.provides ~= dep then
        local bound = pp.bindings[dep]
        if not bound then util.fail("'%s' depends on interface '%s' but no backend is bound for %s", name, dep, plat) end
        for _, b in ipairs(bound) do
          local bi = instance_of(b, plat, conf, name .. " (via interface " .. dep .. ")")
          if not util.contains(inst.deps, bi) then inst.deps[#inst.deps + 1] = bi end
        end
      end
    end
    for ext, spec in pairs(rec.fields.external or {}) do
      if util.external_on(spec, plat) and not (pp.toolchain.fields.externals or {})[ext] then util.fail("'%s' needs external '%s', which toolchain '%s' does not know", name, ext, pp.toolchain.name) end
    end
    -- Tools: an interface or a tool module, instanced for the host. On a native build in
    -- the host config that is the target's own instance.
    local tools = rec.fields.tools or {}
    for k, v in pairs(tools) do
      local tname, spec = nil, {}
      if type(k) == "number" then tname = v else tname = k; spec = type(v) == "table" and v or {} end
      local tmod = tname
      if interfaces[tname] then
        local bound = pp.bindings[tname]
        if not bound then util.fail("'%s' needs tool interface '%s' but no backend is bound for %s", name, tname, plat) end
        tmod = bound[1]
      end
      local ti = instance_of(tmod, host_platform, host_config, name .. " (tool)")
      inst.tools[#inst.tools + 1] = { instance = ti, spec = spec, name = tname }
    end

    stack[#stack] = nil
    visiting[key] = nil
    g.order[#g.order + 1] = inst -- post-order: dependencies first
    return inst
  end

  -- Toolchains and tools that are not built from source are never roots of a target
  -- build. A host module is a root of every build its host supports.
  local function buildable(rec)
    local builds = host_only(rec) and has_platform(rec, host_platform) or has_platform(rec, platform)
    return builds and rec.fields.provides ~= "toolchain" and not rec.fields.system and not rec.fields.prebuilt
  end

  -- Roots.
  -- A project of its own (a korvid.config that adds module paths) has its modules as the
  -- one root, with their tests; the master file's roots are the engine's.
  local roots = master.roots
  if #roots == 0 then roots = { { name = "all", fields = { modules = "*" } } } end
  if c.project then roots = { { name = "project", fields = { modules = "*", tests = true }, project = true } } end
  local wanted = {}
  for _, root in ipairs(roots) do
    -- paths: only the modules found in those module paths (relative to the root), every one
    -- of them unless modules says otherwise.
    local in_paths
    if root.fields.paths then
      in_paths = {}
      for _, p in ipairs(util.as_list(root.fields.paths)) do
        local real = kfs.realpath(util.is_abs(p) and p or util.join(c.root_dir, p))
        if not real then util.fail("root '%s': path '%s' is not a directory", root.name, p) end
        in_paths[real] = true
      end
    end
    local patterns = root.fields.modules and util.as_list(root.fields.modules) or { in_paths and "*" or root.name }
    -- A root may name the platforms it applies to; on any other it is left out.
    local applies = not root.fields.platforms or util.contains(util.as_list(root.fields.platforms), platform)
    for _, rec in ipairs(applies and c.modules or {}) do
      for _, pat in ipairs(patterns) do
        if util.matches(rec.name, pat) and buildable(rec) and (rec.project or not root.project)
           and (not in_paths or in_paths[rec.modulepath]) then
          local inst = instance_of(rec.name, platform, config, "root '" .. root.name .. "'")
          wanted[#wanted + 1] = inst
          if root.fields.tests and rec.tests_dir then inst.tests = true end
          if not util.contains(inst.roots or {}, root.name) then inst.roots = inst.roots or {}; inst.roots[#inst.roots + 1] = root.name end
        end
      end
    end
  end
  if #wanted == 0 then util.fail("no module matched any root") end

  -- Every other module the platform supports, so a command line can name it without the
  -- build file changing: built only when asked for. One that does not resolve here (an
  -- external the toolchain does not know, a dependency that does not build) is left out,
  -- everything it added taken back, and the reason kept for whoever names it.
  -- What the roots reach: the default build (`all`), as it was before anything below.
  for _, inst in ipairs(g.instances) do inst.in_roots = true end
  g.unavailable = {}
  for _, rec in ipairs(c.modules) do
    local key = rec.name .. "@" .. (host_only(rec) and (host_platform .. "-" .. host_config) or (platform .. "-" .. config))
    if buildable(rec) and not g.by_key[key] then
      local n_inst, n_order = #g.instances, #g.order
      local ok, err = pcall(instance_of, rec.name, platform, config, "no root (built when named)")
      if not ok then
        for i = #g.instances, n_inst + 1, -1 do g.by_key[g.instances[i].key] = nil; g.instances[i] = nil end
        for i = #g.order, n_order + 1, -1 do g.order[i] = nil end
        for k in pairs(visiting) do visiting[k] = nil end
        for i = #stack, 1, -1 do stack[i] = nil end
        g.unavailable[rec.name] = tostring(err)
      end
    end
  end

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
