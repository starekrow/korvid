-- kormake: the entry point. Commands: build, gen, check, graph, test, package, clean, help,
-- --version; the rest of the command line is module names and one target.
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

local commands = { build = true, gen = true, check = true, graph = true, test = true, package = true, clean = true, help = true }

local function usage()
  io.stderr:write([[
usage: kormake [COMMAND] [NAME...] [TARGET] [OPTIONS]

  build    build the named modules, or what the current directory means (the default)
  test     build and run their tests
  package  dist/<exe>/: one executable, its pack, shaders and licenses (an .app on Apple)
  gen      write build.ninja and compile_commands.json only
  check    resolve only: cycles, bindings, duplicates
  graph    the instance graph [--dot] [--why MODULE]
  clean    remove the target's build directory (every target's, without one)

NAME is a module by its name, its executable's name or its directory's name; a NAME with
a slash (or ".") is a directory, every module under it. Without one: the module you are
in, the modules under the directory you are in, or at the root the master file's roots.

TARGET is <platform>-<config>, a config alone (the host platform), host or host-<config>.
Without one: the korvid.config's target, else the host platform in the first config.

options: -C DIR (start in DIR), --no-unity, --cxx PATH, --dot, --why MODULE, --version
See tools/kormake/README.md.
]])
end

-- The raw command line: the command, every other word, the options.
local function parse(args)
  local cmd
  local words, opts = {}, {}
  local i = 1
  local function value(a)
    i = i + 1
    return args[i] or util.fail("%s needs a value", a)
  end
  while i <= #args do
    local a = args[i]
    if a == "--no-unity" then opts.no_unity = true
    elseif a == "--unity" then opts.no_unity = false
    elseif a == "--dot" then opts.dot = true
    elseif a == "--why" then opts.why = value(a)
    elseif a == "--cxx" then opts.cxx = value(a)
    elseif a == "-C" then opts.dir = value(a)
    elseif a == "--help" or a == "-h" then cmd = "help"
    elseif a == "--version" or a == "-v" then cmd = "--version"
    elseif a:match("^%-") then util.fail("unknown option '%s'", a)
    elseif not cmd and #words == 0 and commands[a] then cmd = a -- the first word, if it is a command
    else words[#words + 1] = a end
    i = i + 1
  end
  return cmd or "build", words, opts
end

-- Whether a word is a target: <platform>-<config> of the master file's, a config alone,
-- host, or host-<config>.
local function is_target(word, master)
  if word == "host" or util.contains(master.configs, word) then return true end
  local p, cf = word:match("^([%w_]+)%-([%w_]+)$")
  if not p or not util.contains(master.configs, cf) then return false end
  return p == "host" or util.contains(master.platforms, p)
end

local function split_target(target, c)
  if not target then return KORMAKE_HOST, c.master.configs[1] or "debug" end
  if target == "host" then return KORMAKE_HOST, c.master.host_config or "dev" end
  if util.contains(c.master.configs, target) then return KORMAKE_HOST, target end
  local p, cf = target:match("^([%w_]+)%-([%w_]+)$")
  if p == "host" then p = KORMAKE_HOST end
  return p, cf
end

-- What the names mean, as module records, or nil for the roots. A module by name, then by
-- executable name, then by directory name; a path, every module under it; no names, the
-- current directory (the module it is in, the modules under it, or the roots at the root
-- or at a project's own directory). Also the set of records that came with a directory
-- rather than by name: those may skip quietly where a named one is an error.
local function select_modules(c, names)
  local real = {}
  for _, rec in ipairs(c.modules) do real[rec] = kfs.realpath(rec.dir) or rec.dir end
  local function under_dir(dir)
    local list = {}
    for _, rec in ipairs(c.modules) do
      if collect.under(dir, real[rec]) then return { rec }, false end -- inside one module
      if collect.under(real[rec], dir) then list[#list + 1] = rec end
    end
    -- In a project, a directory that holds both (the engine checked out under it) means
    -- the project's.
    if c.project then
      local own = {}
      for _, rec in ipairs(list) do if rec.project then own[#own + 1] = rec end end
      if #own > 0 then list = own end
    end
    return list, true
  end
  local from_dir = {}
  if #names == 0 then
    if c.start == c.root_dir or (c.project and c.start == c.anchor) then return nil, from_dir end
    local list = under_dir(c.start)
    if #list == 0 then return nil, from_dir end
    for _, rec in ipairs(list) do from_dir[rec] = true end
    return list, from_dir
  end
  local out = {}
  local function add(rec) if not util.contains(out, rec) then out[#out + 1] = rec end end
  for _, n in ipairs(names) do
    if n:find("[/\\]") or n == "." or n == ".." then
      local dir = kfs.realpath(util.is_abs(n) and n or util.join(c.start, n))
      if not dir or not kfs.isdir(dir) then util.fail("'%s' is not a directory", n) end
      local list, many = under_dir(dir)
      if #list == 0 then util.fail("no module in or under %s", dir) end
      for _, rec in ipairs(list) do
        if many and not util.contains(out, rec) then from_dir[rec] = true end
        add(rec)
      end
    else
      local found
      for _, how in ipairs({ "name", "exe", "dir" }) do
        local hits = {}
        for _, rec in ipairs(c.modules) do
          local v = how == "name" and rec.name or how == "exe" and rec.fields.exe or util.basename(rec.dir)
          if v == n then hits[#hits + 1] = rec end
        end
        if #hits > 1 then
          local which = {}
          for _, h in ipairs(hits) do which[#which + 1] = h.name .. " (" .. h.dir .. ")" end
          util.fail("'%s' could be %s; name the module", n, table.concat(which, " or "))
        end
        if #hits == 1 then found = hits[1]; break end
      end
      if not found then util.fail("no module, executable or module directory is called '%s'", n) end
      from_dir[found] = nil -- named: an error if it cannot build, whatever else brought it
      add(found)
    end
  end
  return out, from_dir
end

-- The selected modules' instances in this graph. A module the build cannot hold is an
-- error when it was named, a note when it came with a directory.
local function instances_of(g, recs, from_dir)
  local out = {}
  for _, rec in ipairs(recs) do
    local inst
    for _, i in ipairs(g.instances) do if i.module == rec and i.selectable then inst = i end end
    local why
    if not inst then
      local f = rec.fields
      if f.provides == "toolchain" then why = "it is a toolchain"
      elseif f.system or f.prebuilt then why = "it is a tool kormake finds, not one it builds"
      elseif g.unavailable[rec.name] then why = g.unavailable[rec.name]
      else why = string.format("it does not build for %s", g.platform) end
    end
    if inst then out[#out + 1] = inst
    elseif not from_dir[rec] then util.fail("%s: nothing to build for %s-%s: %s", rec.name, g.platform, g.config, why)
    else print(string.format("kormake: skipping %s: %s", rec.name, why)) end
  end
  return out
end

-- A warning when this binary carries scripts other than the engine's: run the bootstrap.
local function stale_check(root)
  local dir = util.join(root, "tools/kormake/scripts")
  if not kfs.isdir(dir) then return end
  local differs = {}
  for _, n in ipairs(util.sorted_files(dir, "lua")) do
    if kormake_script(util.stem(n)) ~= kfs.read(util.join(dir, n)) then differs[#differs + 1] = n end
  end
  if #differs > 0 then
    io.stderr:write(string.format("kormake: warning: this binary's %s differ%s from %s; run %s\n", table.concat(differs, ", "),
      #differs == 1 and "s" or "", dir, util.join(root, KORMAKE_HOST == "windows" and "tools/kormake/bootstrap.bat" or "tools/kormake/bootstrap.sh")))
  end
end

local function run(cmd, words, opts)
  if cmd == "--version" or cmd == "-v" then print("kormake " .. KORMAKE_VERSION .. " (" .. _VERSION .. ")"); return 0 end
  if cmd == "help" or cmd == "--help" then usage(); return 0 end
  local c = collect.run(collect.discover(opts.dir or kfs.cwd()))
  local root = c.root_dir
  stale_check(root)
  -- The words: one target at most, the rest names.
  local target, names = nil, {}
  for _, w in ipairs(words) do
    if is_target(w, c.master) then
      if target then util.fail("two targets, '%s' and '%s'", target, w) end
      target = w
    else names[#names + 1] = w end
  end
  if not target and c.default_target then
    if not is_target(c.default_target, c.master) then util.fail("%s: target '%s' is not a target of the master file", c.config, c.default_target) end
    target = c.default_target
  end
  local platform, config = split_target(target, c)
  target = platform .. "-" .. config
  if cmd == "clean" then
    -- What kormake writes there and nothing else: one target's directory, or every
    -- target's and compile_commands.json (and the directory, if that leaves it empty).
    if #names > 0 then util.fail("clean takes a target, not modules") end
    local dirs = {}
    if #words > 0 then dirs[1] = util.join(c.build_root, target) else
      for _, p in ipairs(c.master.platforms) do
        for _, cf in ipairs(c.master.configs) do dirs[#dirs + 1] = util.join(c.build_root, p .. "-" .. cf) end
      end
      dirs[#dirs + 1] = util.join(c.build_root, "compile_commands.json")
    end
    for _, d in ipairs(dirs) do
      if kfs.isdir(d) or kfs.isfile(d) then kfs.rmtree(d); print("kormake: removed " .. d) end
    end
    if #words == 0 and kfs.isdir(c.build_root) and #kfs.listdir(c.build_root) == 0 then kfs.rmtree(c.build_root) end
    return 0
  end
  local g = resolve.run(c, platform, config, opts)
  -- What the names (or the current directory) select, for the commands that act on
  -- modules; gen and check take the whole graph wherever they run.
  local selected, from_dir
  local function choose()
    local recs
    recs, from_dir = select_modules(c, names)
    selected = recs and instances_of(g, recs, from_dir) or nil
    if selected and #selected == 0 then util.fail("nothing to do for %s", target) end
  end
  if cmd == "check" then
    if #names > 0 then choose() end
    -- Modules outside the roots that cannot build here, and why: not a problem, but worth
    -- knowing before someone names one.
    for _, n in ipairs(util.sorted_keys(g.unavailable)) do print(string.format("kormake: left out of %s: %s: %s", target, n, g.unavailable[n])) end
    -- Roots that cannot build on this machine (a package not fetched): what gen would stop
    -- on. The modules that share a reason, on one line.
    local reasons, by_reason = {}, {}
    for _, inst in ipairs(g.problems) do
      if not by_reason[inst.problem] then by_reason[inst.problem] = {}; reasons[#reasons + 1] = inst.problem end
      table.insert(by_reason[inst.problem], inst.module.name)
    end
    for _, why in ipairs(reasons) do print(string.format("kormake: cannot build %s for %s: %s", table.concat(by_reason[why], ", "), target, why)) end
    print(string.format("kormake: %d modules, %d instances for %s-%s, %s", #c.modules, #g.instances, platform, config,
      #g.problems == 0 and "no problems" or (#g.problems == 1 and "1 problem" or (#g.problems .. " problems"))))
    return #g.problems == 0 and 0 or 1
  end
  if cmd == "graph" then
    choose()
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
    -- Named modules: those and what they reach.
    local show
    if selected then
      show = {}
      local function walk(i)
        if show[i] then return end
        show[i] = true
        for _, d in ipairs(i.deps) do walk(d) end
        for _, t in ipairs(i.tools) do walk(t.instance) end
      end
      for _, i in ipairs(selected) do walk(i) end
    end
    if opts.dot then
      print("digraph kormake {\n  rankdir=LR;")
      for _, inst in ipairs(g.instances) do
        if (show and not show[inst]) or (not show and not inst.in_roots) then goto next_dot end
        for _, d in ipairs(inst.deps) do print(string.format('  "%s" -> "%s";', inst.key, d.key)) end
        for _, t in ipairs(inst.tools) do print(string.format('  "%s" -> "%s" [style=dashed];', inst.key, t.instance.key)) end
        ::next_dot::
      end
      print("}")
      return 0
    end
    -- Without a selection, what the roots reach (the default build), not the modules
    -- outside them.
    for _, inst in ipairs(g.order) do
      if (show and not show[inst]) or (not show and not inst.in_roots) then goto next_inst end
      local deps = {}
      for _, d in ipairs(inst.deps) do deps[#deps + 1] = d.module.name end
      local tools = {}
      for _, t in ipairs(inst.tools) do tools[#tools + 1] = t.instance.module.name end
      print(string.format("%-40s %-8s deps: %s%s%s%s", inst.key, inst.module.fields.output or "lib", table.concat(deps, ", "),
        #tools > 0 and ("  tools: " .. table.concat(tools, ", ")) or "", inst.tests and "  [tests]" or "",
        inst.problem and ("  [cannot build: " .. inst.problem:match("^[^\n]*") .. "]") or ""))
      ::next_inst::
    end
    return 0
  end
  local build = emit.run(c, g, opts)
  if cmd ~= "gen" or #names > 0 then choose() end
  -- Ninja: the toolchain probe's when it found one (Visual Studio ships one), else PATH's.
  local ninja = g.ninja and util.shell_quote(util.native(g.ninja)) or "ninja"
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
    -- The selection's phonies, or Ninja's default (the roots, with their tests).
    local targets = {}
    for _, inst in ipairs(selected or {}) do targets[#targets + 1] = util.shell_quote(inst.module.name) end
    if selected then
      local which = {}
      for _, inst in ipairs(selected) do which[#which + 1] = inst.module.name end
      print(string.format("kormake: building %s for %s", table.concat(which, ", "), target))
    end
    return kfs.exec(string.format("%s -C %s %s", ninja, util.shell_quote(build), table.concat(targets, " ")))
  end
  if cmd == "package" then
    -- A self-contained directory: the executable, its pack, its shaders, the licenses of
    -- everything it links. dist/<exe>/ under the root, replaced each time.
    if not selected or #selected ~= 1 then util.fail("package makes one executable: name it (kormake package NAME [TARGET])") end
    local inst = selected[1]
    local exe = inst.module.fields.exe or inst.module.name
    if inst.module.fields.output ~= "exe" or inst.prefix ~= "" then util.fail("'%s' is not an executable of %s", inst.module.name, target) end
    local exe_file = inst.module.fields.exe or inst.module.name
    local exe_bin = exe_file .. inst.toolchain.fields.exe_suffix
    -- Its packs: what its per-exe rules write, as emit named them.
    local packs = inst.per_exe_outputs or {}
    local q = util.shell_quote
    local want = { q("bin/" .. exe_bin) }
    for _, p in ipairs(packs) do want[#want + 1] = q(p) end
    local rc = kfs.exec(string.format("%s -C %s %s", ninja, q(build), table.concat(want, " ")))
    if rc ~= 0 then return rc end
    local dist = util.join(c.base_dir, "dist/" .. exe)
    if not kfs.rmtree(dist) and kfs.isdir(dist) then util.fail("package: cannot remove %s", dist) end
    -- Where the executable goes and where what it reads goes: one directory, or for a
    -- toolchain that packages applications, <exe>.app: a macOS one's (package = "app")
    -- Contents/MacOS/ and Contents/Resources/, an iOS one's (package = "iosapp") top, where
    -- both go (package_app.lua does the rest).
    local kind = inst.toolchain.fields.package
    local app = (kind == "app" or kind == "iosapp") and util.join(dist, exe .. ".app") or nil
    local exe_dir = kind == "app" and util.join(app, "Contents/MacOS") or app or dist
    local res = kind == "app" and util.join(app, "Contents/Resources") or app or dist
    kfs.mkdirp(exe_dir)
    kfs.mkdirp(util.join(res, "licenses"))
    local function copy_in(from, name) kfs.copy(from, util.join(res, name or util.basename(from))) end
    kfs.copy(util.join(build, "bin/" .. exe_bin), util.join(exe_dir, exe_bin))
    for _, p in ipairs(packs) do copy_in(util.join(build, p)) end
    if kfs.isdir(util.join(build, "bin/shaders")) then util.copy_tree(util.join(build, "bin/shaders"), util.join(res, "shaders")) end
    local copied = 0
    for _, f in ipairs({ "LICENSE", "NOTICE" }) do
      if kfs.isfile(util.join(root, f)) then copy_in(util.join(root, f)); copied = copied + 1 end
    end
    local mods = {}
    for _, d in ipairs(inst.closure) do mods[#mods + 1] = d end
    mods[#mods + 1] = inst
    -- Without a pack (an executable that has none), what it reads is the loose assets: its
    -- modules' folders of bin/assets/, as the build staged them, into assets/.
    local loose = 0
    if #packs == 0 then
      for _, d in ipairs(mods) do
        local from = util.join(build, "bin/assets/" .. d.module.name)
        if kfs.isdir(from) then util.copy_tree(from, util.join(res, "assets/" .. d.module.name)); loose = loose + 1 end
      end
    end
    for _, d in ipairs(mods) do
      for _, f in ipairs(util.as_list(d.module.fields.licenses or {})) do
        copy_in(util.join(d.module.dir, f), "licenses/" .. d.module.name .. "-" .. util.basename(f))
        copied = copied + 1
      end
    end
    local pack_names = {}
    for _, p in ipairs(packs) do pack_names[#pack_names + 1] = util.basename(p) end
    local bundle = ""
    if app then
      local libs, signed = kormake.load("package_app").run(build, app, exe_bin, inst.toolchain, root)
      bundle = kind == "app" and string.format(", %d librar%s bundled, signed %s", libs, libs == 1 and "y" or "ies", signed) or (", signed " .. signed)
    end
    print(string.format("kormake: %s: %s%s, %d license files%s%s", app or dist, exe,
      #packs > 0 and (", " .. table.concat(pack_names, ", ")) or (loose > 0 and string.format(", loose assets of %d module%s", loose, loose == 1 and "" or "s") or ", no pack"), copied,
      bundle, kfs.isfile(util.join(root, "LICENSE")) and "" or " (the repository has no LICENSE at its root)"))
    return 0
  end
  if cmd == "test" then
    -- The selection's test stamps (a directory's modules without tests skip), or `check`:
    -- every test a root asks for.
    local targets = {}
    if not selected then targets[1] = "check" else
      for _, inst in ipairs(selected) do
        if inst.test_stamp then targets[#targets + 1] = util.shell_quote(inst.test_stamp)
        elseif not from_dir[inst.module] then util.fail("%s has no tests", inst.module.name) end
      end
      if #targets == 0 then print("kormake: no tests there"); return 0 end
    end
    return kfs.exec(string.format("%s -C %s %s", ninja, util.shell_quote(build), table.concat(targets, " ")))
  end
  usage()
  return 2
end

-- What the Ninja file runs for the host's own steps, so no rule needs a shell: _copy IN OUT
-- copies a file; _test EXE STAMP runs a test executable in the build directory and, if it
-- passes, writes the stamp. Neither reads the master file.
local function internal(args)
  if args[1] == "_copy" and #args == 3 then
    kfs.copy(args[2], args[3])
    return 0
  end
  if args[1] == "_test" and #args >= 3 then
    local exe = util.native(args[2])
    if KORMAKE_HOST ~= "windows" and not util.is_abs(exe) and not exe:find("/") then exe = "./" .. exe end
    -- Under a runner (args 4 on: the toolchain's test_runner), the executable by its
    -- absolute path: the runner's process does not start in the build directory.
    local runner = {}
    for i = 4, #args do runner[#runner + 1] = util.shell_quote(args[i]) end
    if #runner > 0 then exe = util.join(kfs.cwd(), exe) end
    local rc = kfs.exec((#runner > 0 and (table.concat(runner, " ") .. " ") or "") .. util.shell_quote(exe))
    if rc ~= 0 then return rc ~= -1 and rc or 1 end
    kfs.write(args[3], "", true)
    return 0
  end
end

local ok, result = pcall(function()
  if arg[1] and arg[1]:sub(1, 1) == "_" then
    local rc = internal(arg)
    if rc then return rc end
  end
  local cmd, words, opts = parse(arg)
  return run(cmd, words, opts)
end)
if not ok then
  io.stderr:write("kormake: " .. util.message(result) .. "\n")
  return 1
end
return result
