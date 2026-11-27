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
    -- What each external means here: the toolchain's, then the ones the project defines
    -- for the platform (collect.lua), each a copy that remembers its file. The names are
    -- one namespace, so a project cannot redefine the engine's.
    local externals = {}
    for k, v in pairs(tc.fields.externals or {}) do externals[k] = v end
    for name, p in pairs((c.externals or {})[plat] or {}) do
      if externals[name] then
        util.fail("external '%s' is defined for %s in both %s (toolchain '%s') and %s", name, plat, tc.file, tc.name, p.file)
      end
      local e = { from = p.file, base = p.base }
      for k, v in pairs(p.def) do e[k] = v end
      externals[name] = e
    end
    per_platform[plat] = { toolchain = tc, bindings = bindings, externals = externals }
    return per_platform[plat]
  end
  local tc = platform_of(platform).toolchain
  local bindings = platform_of(platform).bindings

  local g = { platform = platform, config = config, toolchain = tc, bindings = bindings, interfaces = interfaces,
              instances = {}, by_key = {}, order = {}, host = host_platform, host_config = host_config,
              platform_of = platform_of }

  local visiting = {}
  local stack = {}

  -- why: who asked, for messages; parent: that instance (none for a root), which graph
  -- --why walks.
  local function instance_of(name, plat, conf, why, parent)
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
    if not has_platform(rec, plat) then util.soft_fail("module '%s' (needed by %s) does not support platform '%s'", name, why, plat) end
    local pp = platform_of(plat)
    -- Outputs: the target's at the build directory's top, the host's (when it is not the
    -- target) under host/.
    local prefix = (plat == platform and conf == config) and "" or "host/"
    -- Selectable: what a command line can name, with a phony of its module's name; the
    -- target's instance, or a host module's only one.
    local inst = { module = rec, platform = plat, config = conf, key = key, deps = {}, tools = {}, why = why, parent = parent, tests = false,
                   prefix = prefix, toolchain = pp.toolchain, host = (plat == host_platform and conf == host_config),
                   selectable = (prefix == "" or host_only(rec)) and rec.fields.provides ~= "toolchain"
                     and not rec.fields.system and not rec.fields.prebuilt }
    g.by_key[key] = inst
    g.instances[#g.instances + 1] = inst
    visiting[key] = true
    stack[#stack + 1] = key

    for _, dep in ipairs(rec.fields.deps or {}) do
      local drec = by_name[dep]
      if drec and drec.private and rec.project then
        util.fail("'%s' depends on '%s', which is the engine's own (%s), not a module a project can use", name, dep, drec.dir)
      end
      if drec and host_only(drec) and not host_only(rec) then
        util.fail("'%s' links '%s', which is built for the host only; name it in tools instead", name, dep)
      end
      local d = instance_of(dep, plat, conf, name, inst)
      inst.deps[#inst.deps + 1] = d
      -- An interface: whoever uses it links the backend too, unless this is the backend.
      if interfaces[dep] and rec.fields.provides ~= dep then
        local bound = pp.bindings[dep]
        if not bound then util.soft_fail("'%s' depends on interface '%s' but no backend is bound for %s", name, dep, plat) end
        for _, b in ipairs(bound) do
          local bi = instance_of(b, plat, conf, name .. " (via interface " .. dep .. ")", inst)
          if not util.contains(inst.deps, bi) then inst.deps[#inst.deps + 1] = bi end
        end
      end
    end
    for ext, spec in pairs(rec.fields.external or {}) do
      if util.external_on(spec, plat) and not pp.externals[ext] then
        util.soft_fail("'%s' needs external '%s', which toolchain '%s' does not know and no module or korvid.config defines for %s", name, ext, pp.toolchain.name, plat)
      end
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
        if not bound then util.soft_fail("'%s' needs tool interface '%s' but no backend is bound for %s", name, tname, plat) end
        tmod = bound[1]
      end
      local ti = instance_of(tmod, host_platform, host_config, name .. " (tool)", inst)
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
  -- build file changing: built only when asked for. One that cannot build here (soft_fail:
  -- an external the toolchain does not know, a dependency that does not build) is left out,
  -- everything it added taken back, and the reason kept for whoever names it. Any other
  -- failure (a cycle, a missing module, a bug) stops the run as it would for a root.
  -- What the roots reach: the default build (`all`), as it was before anything below.
  for _, inst in ipairs(g.instances) do inst.in_roots = true end
  g.unavailable = {}
  for _, rec in ipairs(c.modules) do
    local key = rec.name .. "@" .. (host_only(rec) and (host_platform .. "-" .. host_config) or (platform .. "-" .. config))
    if buildable(rec) and not g.by_key[key] then
      local n_inst, n_order = #g.instances, #g.order
      local ok, err = pcall(instance_of, rec.name, platform, config, "no root (built when named)")
      if not ok then
        if not (type(err) == "table" and err.soft) then error(err, 0) end
        for i = #g.instances, n_inst + 1, -1 do g.by_key[g.instances[i].key] = nil; g.instances[i] = nil end
        for i = #g.order, n_order + 1, -1 do g.order[i] = nil end
        for k in pairs(visiting) do visiting[k] = nil end
        for i = #stack, 1, -1 do stack[i] = nil end
        g.unavailable[rec.name] = util.message(err)
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

  -- What each external means on a platform: the toolchain's and the project's.
  local function externals_of(plat) return platform_of(plat).externals end
  g.externals_of = externals_of
  -- Who defined an external, for messages: its toolchain, or the project's file.
  local function definer(tc, e) return e.from or ("toolchain " .. tc.name) end

  -- An external with pkg_config = "<package>" is asked of pkg-config (KORMAKE_PKG_CONFIG
  -- names another program) when the platform is the machine kormake runs on: the include
  -- directories, library directories and libraries it reports replace the external's own,
  -- which are the fallback where there is no pkg-config or the target is another platform.
  -- Only -I, -L and -l are taken. A package pkg-config does not find is a soft failure,
  -- with the external's `fetch`. The package's .pc file becomes a regeneration input.
  g.regen_inputs = {}
  local pkg_program
  local function pkg_config(args)
    if pkg_program == nil then
      local p = kfs.getenv("KORMAKE_PKG_CONFIG")
      pkg_program = (p and p ~= "") and util.shell_quote(util.native(p)) or "pkg-config"
    end
    kfs.mkdirp(c.build_root)
    local out = util.join(c.build_root, "pkg-config.txt")
    local null = KORMAKE_HOST == "windows" and "NUL" or "/dev/null"
    local rc = kfs.exec(pkg_program .. " " .. args .. " > " .. util.shell_quote(util.native(out)) .. " 2> " .. null)
    return rc == 0, util.trim(kfs.read(out) or "")
  end
  local pkg_available
  local function external_pkg(tc, ext, e, plat)
    if not e.pkg_config or plat ~= host_platform or e.pkg_asked then return end
    e.pkg_asked = true
    if pkg_available == nil then pkg_available = pkg_config("--version") end
    local hint = e.fetch or ("Install the development package that provides " .. e.pkg_config .. ".pc.")
    if not pkg_available then
      if e.libs or e.include or e.dir or e.env or e.frameworks then return end -- the explicit fields stand
      e.pkg_failed = string.format("external '%s' (%s) is found with pkg-config, which is not on PATH (KORMAKE_PKG_CONFIG names another).\n%s",
        ext, definer(tc, e), hint)
      return
    end
    local name = util.shell_quote(e.pkg_config)
    if not pkg_config("--exists " .. name) then
      e.pkg_failed = string.format("external '%s' (%s): pkg-config does not find '%s'.\n%s", ext, definer(tc, e), e.pkg_config, hint)
      return
    end
    local function words(flag, prefix)
      local _, text = pkg_config(flag .. " " .. name)
      local list = {}
      for word in text:gmatch("%S+") do
        if word:sub(1, #prefix) == prefix and #word > #prefix then list[#list + 1] = word:sub(#prefix + 1) end
      end
      return list
    end
    e.include, e.libdirs, e.libs = words("--cflags-only-I", "-I"), words("--libs-only-L", "-L"), words("--libs-only-l", "-l")
    e.pkg_found = true
    local _, pcdir = pkg_config("--variable=pcfiledir " .. name)
    local pc = pcdir ~= "" and util.join((pcdir:gsub("\\", "/")), e.pkg_config .. ".pc") or nil
    if pc and kfs.isfile(pc) then g.regen_inputs[#g.regen_inputs + 1] = pc end
  end
  -- The version a module asks of an external (pipewire = ">=0.3.60"), checked where
  -- pkg-config found the package; anywhere else it is documentation.
  local function external_version(inst, ext, e, spec)
    local want = type(spec) == "table" and spec[1] or spec
    if not e.pkg_found or type(want) ~= "string" then return end
    local op, v = want:match("^%s*([<>=]*)%s*([%w%.%-_+~]+)%s*$")
    local flag = ({ [">="] = "--atleast-version", ["=="] = "--exact-version", ["="] = "--exact-version", [""] = "--atleast-version",
                    ["<="] = "--max-version" })[op or "?"]
    if not flag then return end
    local name = util.shell_quote(e.pkg_config)
    if not pkg_config(flag .. "=" .. util.shell_quote(v) .. " " .. name) then
      local _, have = pkg_config("--modversion " .. name)
      util.soft_fail("'%s' needs external '%s' %s, and pkg-config finds %s %s.\n%s", inst.module.name, ext, want, e.pkg_config, have,
        e.fetch or ("Install a newer " .. e.pkg_config .. "."))
    end
  end

  -- An external kept in a directory (a prebuilt package or an installed SDK) is found
  -- wherever KORMAKE_<NAME>_DIR says, else where the variable its `env` names says (an
  -- SDK's installer sets one), else at its `dir`: under the engine root for a toolchain's,
  -- beside the file that defined it for a project's. tc is the toolchain as the platform
  -- sees it, e the external (the toolchain's own of that name by default). One pkg-config
  -- found has no directory: its paths are absolute. A package that is not there is a soft
  -- failure, with the definer's instructions.
  local external_dirs = {}
  local function external_dir(tc, ext, e)
    e = e or tc.fields.externals[ext]
    if e.pkg_failed then util.soft_fail("%s", e.pkg_failed) end
    if e.pkg_found or (not e.dir and not e.env) then return nil end
    if external_dirs[e] then return external_dirs[e] end
    local var = "KORMAKE_" .. ext:upper():gsub("[^%w]", "_") .. "_DIR"
    local env = kfs.getenv(var)
    if env == "" then env = nil end
    if not env and e.env then
      env = kfs.getenv(e.env)
      if env == "" then env = nil end
      if env then var = e.env end
    end
    if not env and not e.dir then
      util.soft_fail("external '%s' (%s): neither %s nor %s is set.\n%s", ext, definer(tc, e), var, e.env,
        e.fetch or ("Set " .. var .. " to the directory that holds it."))
    end
    local dir = env and (env:gsub("\\", "/"):gsub("/$", "")) or (util.is_abs(e.dir) and e.dir or util.join(e.base or c.root_dir, e.dir))
    local check = util.join(dir, e.check or (e.include and e.include[1]) or ".")
    if not (kfs.isdir(check) or kfs.isfile(check)) then
      util.soft_fail("external '%s' (%s) is not at %s%s.\n%s", ext, definer(tc, e), dir,
        env and (", where " .. var .. " points") or "", e.fetch or ("Set " .. var .. " to the directory that holds it."))
    end
    external_dirs[e] = dir
    return dir
  end
  g.external_dir = external_dir

  -- A tool instance's program, and whether it is a file the build depends on (built here,
  -- or vendored) rather than a name looked up on PATH.
  local function tool_path(inst)
    local f = inst.module.fields
    if f.system then return f.system, false end
    if f.prebuilt then
      local p = f.prebuilt[inst.platform]
      if not p then util.soft_fail("tool '%s' has no prebuilt binary for %s", inst.module.name, inst.platform) end
      return util.join(inst.module.dir, p), true
    end
    -- The host instance, built in this graph.
    if f.output == "exe" then return inst.prefix .. "bin/" .. (f.exe or inst.module.name) .. inst.toolchain.fields.exe_suffix, true end
    util.fail("tool '%s' is neither built from source, prebuilt nor a system lookup", inst.module.name)
  end
  g.tool_path = tool_path

  -- What would stop an instance building on this machine, past what the graph says: a
  -- package that is not where the toolchain looks, a private include that is not a
  -- directory, a tool with no binary for the host. Outside the roots the instance is left
  -- out, with everything built on it, and the reason kept for whoever names it. In the
  -- roots it is a problem: check reports it, graph marks it, and emit stops on the first.
  g.problems = {}
  for _, inst in ipairs(g.order) do
    local why
    for _, d in ipairs(inst.closure) do if d.dropped then why = d.module.name .. " cannot build: " .. d.dropped end end
    for _, t in ipairs(inst.tools) do if t.instance.dropped then why = "its tool " .. t.instance.module.name .. " cannot build: " .. t.instance.dropped end end
    if not why then
      local ok, err = pcall(function()
        local f = inst.module.fields
        for _, ext in ipairs(util.sorted_keys(f.external or {})) do
          if util.external_on(f.external[ext], inst.platform) then
            local e = externals_of(inst.platform)[ext]
            external_pkg(inst.toolchain, ext, e, inst.platform)
            external_dir(inst.toolchain, ext, e)
            external_version(inst, ext, e, f.external[ext])
          end
        end
        for _, pi in ipairs(f.private_includes or {}) do
          if not kfs.isdir(util.join(inst.module.dir, pi)) then util.soft_fail("private include '%s' is not a directory", pi) end
        end
        for _, t in ipairs(inst.tools) do tool_path(t.instance) end
      end)
      if not ok then
        if not (type(err) == "table" and err.soft) then error(err, 0) end
        why = util.message(err)
      end
    end
    if why and inst.in_roots then
      inst.problem = why
      g.problems[#g.problems + 1] = inst
    elseif why then
      inst.dropped = why
      if inst.selectable then g.unavailable[inst.module.name] = why end
      inst.selectable = false
    end
  end
  return g
end

return M
