-- kormake: the entry point. Commands: build, gen, check, graph, test, clean, --version.
kormake = {}
local loaded = {}
function kormake.load(name)
  if loaded[name] then return loaded[name] end
  local src = kormake_script(name)
  if not src then error("no embedded script '" .. name .. "'", 0) end
  local chunk, err = load(src, "=kormake/" .. name .. ".lua")
  if not chunk then error(err, 0) end
  loaded[name] = chunk()
  return loaded[name]
end

local util = kormake.load("util")
local collect = kormake.load("collect")
local resolve = kormake.load("resolve")
local emit = kormake.load("emit")

local function usage()
  io.stderr:write([[
A target is <platform>-<config>; "host" is the machine kormake runs on, alone (its host
config, from the master file's host_config) or with a config (host-debug).

usage: kormake build <platform>-<config> [--no-unity] [--cxx PATH]
       kormake gen   <platform>-<config> [--no-unity] [--cxx PATH]
       kormake test  <platform>-<config> [MODULE]
       kormake package <platform>-<config> <exe>   (dist/<exe>/: the exe, its pack, shaders, licenses)
       kormake check [<platform>-<config>]
       kormake graph [<platform>-<config>] [--dot] [--why MODULE]
       kormake clean [<platform>-<config>]
       kormake --version
]])
end

local function find_root()
  local dir = kfs.cwd()
  while true do
    if kfs.isfile(util.join(dir, "kormake.lua")) then return dir end
    local parent = util.dirname(dir)
    if parent == dir or parent == "" or parent == "." or dir == "/" or dir:match("^%a:/?$") then
      util.fail("no kormake.lua in %s or any parent", kfs.cwd())
    end
    dir = parent
  end
end

local function parse(args)
  local cmd = args[1]
  local target, opts, rest = nil, {}, {}
  local i = 2
  while i <= #args do
    local a = args[i]
    if a == "--no-unity" then opts.no_unity = true
    elseif a == "--unity" then opts.no_unity = false
    elseif a == "--dot" then opts.dot = true
    elseif a == "--why" then opts.why = args[i + 1]; i = i + 1
    elseif a == "--cxx" then opts.cxx = args[i + 1]; i = i + 1
    elseif a:match("^%-%-") then util.fail("unknown option '%s'", a)
    elseif not target and (a:match("^[%w_]+%-[%w_]+$") or a == "host") then target = a
    else rest[#rest + 1] = a end
    i = i + 1
  end
  return cmd, target, opts, rest
end

local function split_target(target, c)
  if not target then
    local p = c.master.platforms[1] or "linux"
    local cf = c.master.configs[1] or "debug"
    return p, cf
  end
  if target == "host" then return KORMAKE_HOST, c.master.host_config or "dev" end
  local p, cf = target:match("^([%w_]+)%-([%w_]+)$")
  if p == "host" then p = KORMAKE_HOST end
  return p, cf
end

local function run(cmd, target, opts, rest)
  if cmd == "--version" or cmd == "-v" then print("kormake " .. KORMAKE_VERSION .. " (" .. _VERSION .. ")"); return 0 end
  if not cmd or cmd == "help" or cmd == "--help" then usage(); return 2 end
  local root = find_root()
  local c = collect.run(root)
  local platform, config = split_target(target, c)
  if target then target = platform .. "-" .. config end
  if cmd == "clean" then
    local dir = target and util.join(root, "build/" .. target) or util.join(root, "build")
    kfs.rmtree(dir)
    print("kormake: removed " .. dir)
    return 0
  end
  local g = resolve.run(c, platform, config, opts)
  if cmd == "check" then
    print(string.format("kormake: %d modules, %d instances for %s-%s, no problems", #c.modules, #g.instances, platform, config))
    return 0
  end
  if cmd == "graph" then
    if opts.why then
      local found = false
      for _, inst in ipairs(g.instances) do
        if inst.module.name == opts.why then
          found = true
          print(string.format("%s: pulled in by %s", inst.key, inst.why))
          local i = inst
          while i and i.why and not i.why:match("^root ") do
            local parent = i.why:gsub(" %(.*$", "")
            i = g.by_key[parent .. "@" .. i.platform .. "-" .. i.config]
            if i then print(string.format("  %s: pulled in by %s", i.key, i.why)) end
          end
        end
      end
      if not found then print("kormake: '" .. opts.why .. "' is not in the graph") end
      return 0
    end
    if opts.dot then
      print("digraph kormake {\n  rankdir=LR;")
      for _, inst in ipairs(g.instances) do
        for _, d in ipairs(inst.deps) do print(string.format('  "%s" -> "%s";', inst.key, d.key)) end
        for _, t in ipairs(inst.tools) do print(string.format('  "%s" -> "%s" [style=dashed];', inst.key, t.instance.key)) end
      end
      print("}")
      return 0
    end
    for _, inst in ipairs(g.order) do
      local deps = {}
      for _, d in ipairs(inst.deps) do deps[#deps + 1] = d.module.name end
      local tools = {}
      for _, t in ipairs(inst.tools) do tools[#tools + 1] = t.instance.module.name end
      print(string.format("%-40s %-8s deps: %s%s%s", inst.key, inst.module.fields.output or "lib", table.concat(deps, ", "),
        #tools > 0 and ("  tools: " .. table.concat(tools, ", ")) or "", inst.tests and "  [tests]" or ""))
    end
    return 0
  end
  local build = emit.run(c, g, opts)
  print(string.format("kormake: %s/build.ninja (%d instances)", build, #g.instances))
  -- The tools this build runs, as built for the host, and what they link from the system:
  -- a tool that pulls in a window or a sound card says so here.
  local host_tools = {}
  for _, inst in ipairs(g.instances) do
    if inst.host and inst.module.fields.output == "exe" and inst.extlibs then
      local used = false
      for _, i in ipairs(g.instances) do for _, t in ipairs(i.tools) do if t.instance == inst then used = true end end end
      if used or util.contains(inst.module.fields.platforms or {}, "host") then
        host_tools[#host_tools + 1] = string.format("%s (%s)", inst.module.fields.exe or inst.module.name,
          #inst.extlibs > 0 and table.concat(inst.extlibs, " ") or "no system libraries")
      end
    end
  end
  if #host_tools > 0 then
    print(string.format("kormake: host tools for %s-%s: %s", g.host, g.host_config, table.concat(host_tools, ", ")))
  end
  if cmd == "gen" then return 0 end
  if cmd == "build" then
    local rc = kfs.exec(string.format("ninja -C %s %s", util.shell_quote(build), table.concat(rest, " ")))
    return rc
  end
  if cmd == "package" then
    -- A self-contained directory: the executable, its pack, its shaders, the licenses of
    -- everything it links. dist/<exe>/ under the root, replaced each time.
    local exe = rest[1] or util.fail("usage: kormake package <platform>-<config> <exe>")
    local inst
    for _, i in ipairs(g.instances) do
      if (i.module.name == exe or i.module.fields.exe == exe) and i.module.fields.output == "exe" and i.prefix == "" then inst = i end
    end
    if not inst then util.fail("'%s' is not an executable in %s", exe, target) end
    local exe_file = inst.module.fields.exe or inst.module.name
    local pack
    for _, t in ipairs(inst.tools) do
      for _, r in ipairs(t.instance.module.fields.exports or {}) do
        if r.per == "exe" then pack = "bin/" .. r.outputs:gsub("%%{exe}", exe_file) end
      end
    end
    local q = util.shell_quote
    local rc = kfs.exec(string.format("ninja -C %s %s%s", q(build), q("bin/" .. exe_file), pack and (" " .. q(pack)) or ""))
    if rc ~= 0 then return rc end
    local dist = util.join(root, "dist/" .. exe)
    if not kfs.rmtree(dist) and kfs.isdir(dist) then util.fail("package: cannot remove %s", dist) end
    kfs.mkdirp(util.join(dist, "licenses"))
    local function copy_in(from, name) kfs.copy(from, util.join(dist, name or util.basename(from))) end
    copy_in(util.join(build, "bin/" .. exe_file))
    if pack then copy_in(util.join(build, pack)) end
    if kfs.isdir(util.join(build, "bin/shaders")) then util.copy_tree(util.join(build, "bin/shaders"), util.join(dist, "shaders")) end
    local copied = 0
    for _, f in ipairs({ "LICENSE", "NOTICE" }) do
      if kfs.isfile(util.join(root, f)) then copy_in(util.join(root, f)); copied = copied + 1 end
    end
    local mods = {}
    for _, d in ipairs(inst.closure) do mods[#mods + 1] = d end
    mods[#mods + 1] = inst
    for _, d in ipairs(mods) do
      for _, f in ipairs(util.as_list(d.module.fields.licenses or {})) do
        copy_in(util.join(d.module.dir, f), "licenses/" .. d.module.name .. "-" .. util.basename(f))
        copied = copied + 1
      end
    end
    print(string.format("kormake: %s: %s%s, %d license files%s", dist, exe, pack and (", " .. util.basename(pack)) or ", no pack", copied,
      kfs.isfile(util.join(root, "LICENSE")) and "" or " (the repository has no LICENSE at its root)"))
    return 0
  end
  if cmd == "test" then
    local targets = {}
    if #rest == 0 then targets[1] = "check" else
      for _, m in ipairs(rest) do
        local pre = ""
        for _, i in ipairs(g.instances) do if i.module.name == m and i.tests then pre = i.prefix end end
        targets[#targets + 1] = pre .. "test/" .. util.symbol(m) .. ".ok"
      end
    end
    return kfs.exec(string.format("ninja -C %s %s", util.shell_quote(build), table.concat(targets, " ")))
  end
  usage()
  return 2
end

local ok, result = pcall(function()
  local cmd, target, opts, rest = parse(arg)
  return run(cmd, target, opts, rest)
end)
if not ok then
  io.stderr:write("kormake: " .. tostring(result) .. "\n")
  return 1
end
return result
