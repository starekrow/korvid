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

local commands = { build = true, gen = true, check = true, graph = true, test = true, package = true, clean = true, help = true, run = true }

local function usage()
  io.stderr:write([[
usage: kormake [COMMAND] [NAME...] [TARGET] [OPTIONS]

  build    build the named modules, or what the current directory means (the default)
  test     build and run their tests; --heavy: their heavy tests (tests/heavy/), -- ARGS for those
  package  dist/<exe>/: one executable, its pack, shaders and licenses (an .app on Apple)
  run      kormake run SCRIPT.lua [ARGS...]: the script with kvscript, built for the host
  gen      write build.ninja and compile_commands.json only
  check    resolve only: cycles, bindings, duplicates
  graph    the instance graph [--dot] [--why MODULE]
  clean    remove the target's build directory (every target's, without one)

NAME is a module by its name, its executable's name or its directory's name; a NAME with
a slash (or ".") is a directory, every module under it. Without one: the module you are
in, the modules under the directory you are in, or at the root the master file's roots.

TARGET is <platform>-<config>, a config alone (the host platform), host or host-<config>.
Without one: the korvid.config's target, else the host platform in the first config.

options: -C DIR (start in DIR), --no-unity, --cxx PATH, --dot, --why MODULE, --heavy,
         --sanitize address,undefined|thread (build, test, gen: into <target>+<names>/), --version
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
    elseif a == "--heavy" then opts.heavy = true
    elseif a == "--sanitize" then opts.sanitize = value(a)
    elseif a == "--" then -- the rest is a heavy test's
      opts.rest = {}
      for j = i + 1, #args do opts.rest[#opts.rest + 1] = args[j] end
      break
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
  if opts.heavy and cmd ~= "test" then util.fail("--heavy is for test: kormake test --heavy [NAME...] [-- ARGS]") end
  if opts.rest and not opts.heavy then util.fail("the words after -- are a heavy test's: kormake test --heavy NAME -- ARGS") end
  if opts.sanitize and cmd ~= "build" and cmd ~= "test" and cmd ~= "gen" then util.fail("--sanitize is for build, test and gen") end
  if opts.sanitize then
    -- The names, each once, in name order: the build directory's suffix and what the
    -- regeneration passes on. The toolchain says which it has (emit.lua).
    local list, seen = {}, {}
    for n in opts.sanitize:gmatch("[^,%s]+") do if not seen[n] then seen[n] = true; list[#list + 1] = n end end
    if #list == 0 then util.fail("--sanitize needs a list: address,undefined or thread") end
    table.sort(list)
    if seen.address and seen.thread then util.fail("--sanitize: address and thread cannot be in one build (the compilers refuse it); build each on its own") end
    opts.sanitize = list
  end
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
      -- What goldberg cooked for the programs built here, and what it remembers of it.
      -- (A ship game's tree is dist/<game>/assets/: clean leaves dist/ alone, as it always has.)
      for _, d in ipairs({ "assets", "assets-ship", "goldberg" }) do dirs[#dirs + 1] = util.join(c.build_root, d) end
    end
    -- Each target's sanitized builds (linux-dev+address+undefined) go with it.
    if kfs.isdir(c.build_root) then
      local plain = {}
      for _, d in ipairs(dirs) do plain[util.basename(d)] = true end
      for _, n in ipairs(kfs.listdir(c.build_root)) do
        local base = n:match("^([^+]+)%+")
        if base and plain[base] then dirs[#dirs + 1] = util.join(c.build_root, n) end
      end
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
          local i = inst.parent
          while i do
            print(string.format("  %s: pulled in by %s", i.key, i.why))
            i = i.parent
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
      -- What it links from outside: the toolchain's externals by name, a project's with the
      -- file that defines it.
      local exts = {}
      local ext_fields = inst.module.fields.external or {}
      for _, ext in ipairs(util.sorted_keys(ext_fields)) do
        if util.external_on(ext_fields[ext], inst.platform) then
          local e = g.externals_of(inst.platform)[ext]
          exts[#exts + 1] = ext .. (e and e.from and (" (" .. e.from .. ")") or "")
        end
      end
      print(string.format("%-40s %-8s deps: %s%s%s%s%s", inst.key, inst.module.fields.output or "lib", table.concat(deps, ", "),
        #tools > 0 and ("  tools: " .. table.concat(tools, ", ")) or "", #exts > 0 and ("  externals: " .. table.concat(exts, ", ")) or "",
        inst.tests and "  [tests]" or "",
        inst.problem and ("  [cannot build: " .. inst.problem:match("^[^\n]*") .. "]") or ""))
      ::next_inst::
    end
    return 0
  end
  local build = emit.run(c, g, opts)
  -- The host target's graph says which binary each published command is: the shim's
  -- table, written whenever that graph is (the regenerate rule's gen too).
  if platform == g.host and config == g.host_config and not opts.sanitize then kormake.load("link").write_table(c, g, build) end
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
    if not selected or #selected ~= 1 then util.fail("package makes one executable: name it (kormake package NAME [TARGET])") end
    return kormake.load("package").run(c, build, selected[1], ninja, target)
  end
  if cmd == "test" and opts.heavy then
    -- The selection's heavy tests (a directory's modules without any skip), or every one
    -- a root asks for: built by Ninja, then run here, one at a time, on this machine.
    local list = {}
    if not selected then
      for _, inst in ipairs(g.order) do if inst.heavy and inst.tests then list[#list + 1] = inst end end
    else
      for _, inst in ipairs(selected) do
        if inst.heavy then list[#list + 1] = inst
        elseif not from_dir[inst.module] then util.fail("%s has no heavy tests (tests/heavy/*.cc)", inst.module.name) end
      end
    end
    if #list == 0 then print("kormake: no heavy tests there"); return 0 end
    local targets = {}
    for _, inst in ipairs(list) do
      if not inst.heavy.host and g.toolchain.fields.test_runner then
        util.fail("%s: heavy tests run on this machine, and %s runs its tests under a runner", inst.module.name, target)
      end
      targets[#targets + 1] = util.shell_quote(inst.heavy.target)
    end
    local rc = kfs.exec(string.format("%s -C %s %s", ninja, util.shell_quote(build), table.concat(targets, " ")))
    if rc ~= 0 then return rc end
    local args = {}
    for _, a in ipairs(opts.rest or {}) do args[#args + 1] = util.shell_quote(a) end
    local worst = 0
    for _, inst in ipairs(list) do
      local exe = util.native(util.join(build, inst.heavy.exe))
      print("kormake: heavy test " .. inst.module.name .. ": " .. exe)
      rc = kfs.exec(util.shell_quote(exe) .. (#args > 0 and (" " .. table.concat(args, " ")) or ""))
      if rc ~= 0 then
        io.stderr:write(string.format("kormake: heavy test %s failed (%d)\n", inst.module.name, rc))
        worst = rc ~= -1 and rc or 1
      end
    end
    return worst
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

-- `kormake run SCRIPT [ARGS...]`: the script with the engine's script runner (tools/kvscript),
-- built for the host first (quietly: a built one costs a no-op Ninja run). Everything after
-- the script is the script's, options and targets alike, so this comes before the
-- command line is parsed. The script runs from the current directory.
local function run_script(args)
  local script = args[2]
  if not script then util.fail("run: kormake run SCRIPT.lua [ARGS...]") end
  local c = collect.run(collect.discover(kfs.cwd()))
  local config = c.master.host_config or "dev"
  local g = resolve.run(c, KORMAKE_HOST, config, {})
  if not c.by_name.kvscript then util.fail("run: this engine has no kvscript module (tools/kvscript)") end
  local build = emit.run(c, g, {})
  local ninja = g.ninja and util.shell_quote(util.native(g.ninja)) or "ninja"
  local rc = kfs.exec(string.format("%s --quiet -C %s kvscript", ninja, util.shell_quote(build)))
  if rc ~= 0 then return rc end
  local exe = util.join(build, "bin/kvscript" .. g.toolchain.fields.exe_suffix)
  local words = { util.shell_quote(util.native(exe)), "run", util.shell_quote(script) }
  for i = 3, #args do words[#words + 1] = util.shell_quote(args[i]) end
  return kfs.exec(table.concat(words, " "))
end

-- `kormake _link engine DIR`: the engine root DIR's config names, read leniently (any
-- kormake can answer for any engine's config), printed for the shim. `kormake _link write
-- DIR`: this engine's answer for the anchor DIR: gen of the host target, which writes its
-- tool table; the table's path is printed.
local function link_internal(args)
  local dir = args[3] or util.fail("_link %s DIR", tostring(args[2]))
  if args[2] == "engine" then
    local real = kfs.realpath(dir) or util.fail("no directory '%s'", dir)
    local cfg = util.join(real, "korvid.config")
    if not kfs.isfile(cfg) then
      if kfs.isfile(util.join(real, "kormake.lua")) then print(real); return 0 end
      util.fail("%s has neither a korvid.config nor a kormake.lua", real)
    end
    local e = kormake.load("sandbox").eval_config_engine(cfg, kfs.read(cfg)) or util.fail("%s names no engine", cfg)
    e = util.expand_home(e)
    local root = kfs.realpath(util.is_abs(e) and e or util.join(real, e))
    if not root or not kfs.isfile(util.join(root, "kormake.lua")) then util.fail("%s: engine '%s' is not a directory with a kormake.lua", cfg, e) end
    print(root)
    return 0
  end
  if args[2] == "write" then
    local c = collect.run(collect.discover(dir))
    local config = c.master.host_config or "dev"
    local g = resolve.run(c, KORMAKE_HOST, config, {})
    local build = emit.run(c, g, {})
    local link = kormake.load("link")
    if not link.write_table(c, g, build) then return 1 end
    print(link.table_path(link.paths().cache, c.anchor))
    return 0
  end
  util.fail("_link engine|write DIR")
end

-- What the Ninja file runs for the host's own steps, so no rule needs a shell: _copy IN OUT
-- copies a file; _cook runs a cook's commands; _test EXE STAMP runs a test executable in
-- the build directory and, if it passes, writes the stamp. None reads the master file.
local function internal(args)
  if args[1] == "_copy" and #args == 3 then
    kfs.copy(args[2], args[3])
    return 0
  end
  -- _cook LIST STAMP [--stamped]: a cook's command lines (emit.lua), one a line, in turn;
  -- the first to fail ends it. Then the stamp is touched, unless the cook writes it itself.
  if args[1] == "_cook" and (#args == 3 or (#args == 4 and args[4] == "--stamped")) then
    for line in (kfs.read(args[2]) or ""):gmatch("[^\n]+") do
      local rc = kfs.exec(line)
      if rc ~= 0 then return rc ~= -1 and rc or 1 end
    end
    if not args[4] then kfs.write(args[3], "cooked\n", true) end
    return 0
  end
  -- _ios_test EXE: a test executable run on the connected iOS device (ios.lua), the ios
  -- toolchain's test_runner under _test.
  if args[1] == "_ios_test" and #args == 2 then return kormake.load("ios").test(args[2]) end
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
  if arg[1] == "run" then return run_script(arg) end
  if arg[1] == "_link" then return link_internal(arg) end
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
