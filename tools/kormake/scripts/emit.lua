-- kormake: emit. One Ninja file for the target, compile_commands.json, the generated
-- sources (unity files, module lists). Source paths are absolute; outputs are relative
-- to the build directory, which is where Ninja runs.
local util = kormake.load("util")
local M = {}

local function abs(root, p)
  if util.is_abs(p) then return p end
  return util.join(root, p)
end

local function flag_list(t) return table.concat(t or {}, " ") end

-- The executable's file name: the module's, unless it sets exe.
local function exe_name(inst) return inst.module.fields.exe or inst.module.name end

-- What a toolchain module must say: its rules and how it names and spells things. Nothing
-- is assumed about the compiler family.
local required_fields = { "obj_suffix", "exe_suffix", "lib_file", "flag_include", "flag_define", "flag_lib" }
local required_rules = { "cxx", "ar", "link" }

local function check_toolchain(tc)
  local f = tc.fields
  for _, k in ipairs(required_fields) do
    if type(f[k]) ~= "string" then util.fail("toolchain '%s' does not set %s", tc.name, k) end
  end
  for _, r in ipairs(required_rules) do
    if not (f.rules and f.rules[r] and f.rules[r].command) then util.fail("toolchain '%s' has no %s rule", tc.name, r) end
  end
end

function M.run(c, g, opts)
  local root = c.root_dir
  local target = g.platform .. "-" .. g.config
  -- A sanitized build is a directory of its own, linux-dev+address+undefined, so the
  -- plain target's objects and stamps are never touched.
  local san = opts.sanitize
  local build = util.join(c.build_root, target .. (san and ("+" .. table.concat(san, "+")) or ""))
  -- What the names mean in the target's toolchain, with KV_SANITIZE_<NAME>=1 defined for
  -- each; checked before anything is written.
  local san_flags
  if san then
    local tcf = g.toolchain.fields
    local have = tcf.sanitizers
    if not have then util.fail("--sanitize: toolchain '%s' has no sanitizers", g.toolchain.name) end
    local cx, ld, seen = {}, {}, {}
    local function add(list, f) if not seen[list] then seen[list] = {} end; if not seen[list][f] then seen[list][f] = true; list[#list + 1] = f end end
    for _, n in ipairs(san) do
      local sz = have[n]
      if not sz then util.fail("--sanitize: toolchain '%s' has no sanitizer '%s' (it has %s)", g.toolchain.name, n, table.concat(util.sorted_keys(have), ", ")) end
      for _, f in ipairs(sz.cxxflags or {}) do add(cx, f) end
      for _, f in ipairs(sz.ldflags or {}) do add(ld, f) end
      add(cx, (tcf.flag_define or "-D%s"):format("KV_SANITIZE_" .. n:upper() .. "=1"))
    end
    san_flags = { cxxflags = table.concat(cx, " "), ldflags = table.concat(ld, " ") }
  end
  local q = util.shell_quote
  kfs.mkdirp(build)

  -- Per toolchain: its rule names (suffixed with its tag) and what its probe found.
  local toolchains = {}
  local function toolchain(tc)
    if toolchains[tc.name] then return toolchains[tc.name] end
    check_toolchain(tc)
    local probed = {}
    if tc.fields.probe then probed = kormake.load("probe_" .. tc.fields.probe).run(build, tc) end
    local t = { rec = tc, fields = tc.fields, tag = util.symbol(tc.fields.tag or tc.name), probed = probed }
    toolchains[tc.name] = t
    toolchains[#toolchains + 1] = t
    return t
  end
  -- What an instance compiles and links with: its platform's toolchain in its config, plus
  -- what the toolchain's probe found. The target's are the file's variables; a host
  -- instance overrides them on each build statement.
  local function flags_of(tc, conf, cross, plat)
    local t = toolchain(tc)
    local tcf, pr = t.fields, t.probed
    local cf = (tcf.configs or {})[conf]
    return {
      tc = t,
      cxx = (not cross and opts.cxx) or pr.cxx or tcf.cxx or util.fail("toolchain '%s' names no compiler", tc.name),
      ar = pr.ar or tcf.ar or "",
      link = pr.link or tcf.link or "",
      cxxflags = util.trim(flag_list(tcf.cxxflags) .. " " .. flag_list(cf and cf.cxxflags) .. " " .. flag_list(pr.cxxflags)),
      ldflags = util.trim(flag_list(cf and cf.ldflags) .. " " .. flag_list(tcf.ldflags) .. " " .. flag_list(pr.ldflags)),
      srcdirs = tcf.srcdirs or {},
      externals = g.externals_of(plat), -- the toolchain's and the project's (resolve.lua)
    }
  end
  local target_flags = flags_of(g.toolchain, g.config, false, g.platform)
  -- The sanitizers' flags, on every target instance: the module, its closure, the
  -- engine's modules and the tests alike. A tool the build runs is one of them when the
  -- target is the host in its tools' config (linux-dev), and runs sanitized; a host
  -- instance of its own (linux-debug's tools, built dev) is built plain.
  if san then
    target_flags.cxxflags = util.trim(target_flags.cxxflags .. " " .. san_flags.cxxflags)
    target_flags.ldflags = util.trim(target_flags.ldflags .. " " .. san_flags.ldflags)
  end
  g.ninja = target_flags.tc.probed.ninja
  local function flags(inst)
    if inst.prefix == "" then return target_flags end
    inst.flags = inst.flags or flags_of(inst.toolchain, inst.config, inst.platform ~= g.platform, inst.platform)
    return inst.flags
  end
  local function rule(inst, name) return name .. "_" .. flags(inst).tc.tag end
  -- A program as a command's first word: quoted for the host when it is a path.
  local function program(p) return p:find("[/\\ ]") and q(p) or p end
  -- Build-statement overrides of the file's variables, for a host instance.
  local function overrides(inst)
    if inst.prefix == "" then return "" end
    local fl = flags(inst)
    return "\n  cxx = " .. program(fl.cxx) .. "\n  ar = " .. program(fl.ar) .. "\n  link = " .. program(fl.link) ..
      "\n  cxxflags = " .. fl.cxxflags .. "\n  ldflags = " .. fl.ldflags
  end
  local out = {}
  local function w(s) out[#out + 1] = s end

  -- Where an external's package is (resolve found it, or it is not kept in a directory).
  local function external_dir(fl, ext) return g.external_dir(fl.tc.rec, ext, fl.externals[ext]) end
  -- One of an external's include or library directories: under its package's directory,
  -- or absolute as written (a system package's, or what pkg-config reported). nil for a
  -- relative one with no directory to be under.
  local function external_path(d, p)
    if util.is_abs(p) then return p end
    return d and util.join(d, p) or nil
  end
  -- The externals of these modules that apply on the instance's platform, in name order.
  local function externals_used(inst, mods)
    local seen, list = {}, {}
    for _, mod in ipairs(mods) do
      local ext_fields = mod.fields.external or {}
      for _, ext in ipairs(util.sorted_keys(ext_fields)) do
        if not seen[ext] and util.external_on(ext_fields[ext], inst.platform) then seen[ext] = true; list[#list + 1] = ext end
      end
    end
    return list
  end
  -- Link inputs from externals: the libraries (flag_lib) and, for a package, its library
  -- directories (flag_libdir), as two lists.
  local function extlibs_of(inst, mods)
    local fl = flags(inst)
    local libs, dirs = {}, {}
    for _, ext in ipairs(externals_used(inst, mods)) do
      local e = fl.externals[ext]
      for _, l in ipairs(e.libs or {}) do
        local flag = fl.tc.fields.flag_lib:format(l)
        if not util.contains(libs, flag) then libs[#libs + 1] = flag end
      end
      -- Apple frameworks (Metal, QuartzCore): linked by name, spelled by the toolchain.
      for _, fw in ipairs(e.frameworks or {}) do
        local spell = fl.tc.fields.flag_framework or util.fail("toolchain '%s' does not set flag_framework, which external '%s' needs", fl.tc.rec.name, ext)
        local flag = spell:format(fw)
        if not util.contains(libs, flag) then libs[#libs + 1] = flag end
      end
      local d = external_dir(fl, ext)
      for _, ld in ipairs(e.libdirs or {}) do
        local p = external_path(d, ld)
        if p then
          local spell = fl.tc.fields.flag_libdir or util.fail("toolchain '%s' does not set flag_libdir, which external '%s' needs", fl.tc.rec.name, ext)
          dirs[#dirs + 1] = spell:format(q(p))
        end
      end
    end
    return libs, dirs
  end
  -- Files a package's executables need beside them (a DLL): each copied to bin/ once, and
  -- an order-only input of every executable that links the package.
  local runtime_copied = {}
  local function runtime_of(inst, mods)
    local fl = flags(inst)
    local outs = {}
    for _, ext in ipairs(externals_used(inst, mods)) do
      local d = external_dir(fl, ext)
      for _, f in ipairs(d and fl.externals[ext].runtime or {}) do
        local outpath = inst.prefix .. "bin/" .. util.basename(f)
        if not runtime_copied[outpath] then
          runtime_copied[outpath] = true
          w(string.format("build %s: copy %s", util.ninja_escape(outpath), util.ninja_escape(util.join(d, f))))
        end
        outs[#outs + 1] = outpath
      end
    end
    return outs
  end

  -- A root that cannot build here (resolve.lua's problems) stops the generation: the first,
  -- in dependency order.
  if #g.problems > 0 then util.fail("%s: %s", g.problems[1].module.name, g.problems[1].problem) end

  local cc = {} -- compile_commands.json entries
  local all, tests, checks = {}, {}, {}

  w("# generated by kormake " .. KORMAKE_VERSION .. " for " .. target .. "; edit kormake.lua and the module.lua files, not this")
  w("ninja_required_version = 1.10")
  w("cxx = " .. program(target_flags.cxx))
  w("ar = " .. program(target_flags.ar))
  w("link = " .. program(target_flags.link))
  w("cxxflags = " .. target_flags.cxxflags)
  w("ldflags = " .. target_flags.ldflags)
  w("")
  -- Compile, archive and link: each toolchain's own rules, named with its tag, for the
  -- target's toolchain and the host's when it differs.
  for _, inst in ipairs(g.instances) do if not inst.dropped then flags(inst) end end
  for _, t in ipairs(toolchains) do
    for _, name in ipairs(required_rules) do
      local r = t.fields.rules[name]
      local extra = ""
      for _, k in ipairs({ "depfile", "deps", "msvc_deps_prefix" }) do
        if r[k] then extra = extra .. "\n  " .. k .. " = " .. r[k] end
      end
      w("rule " .. name .. "_" .. t.tag .. "\n  command = " .. r.command .. extra .. "\n  description = " .. name:upper() .. " $out")
    end
  end
  -- Copying, running a test and regenerating are the host's, whatever compiles: kormake
  -- does them itself, so no rule needs a shell.
  local self = util.ninja_escape(program(kfs.self() or "kormake"))
  w("rule copy\n  command = " .. self .. ' _copy $in $out\n  description = COPY $out')
  -- A test the host cannot run itself (the iOS Simulator's, a device's) runs under the
  -- toolchain's test_runner, its words after the stamp (%{root}: the project's root,
  -- %{kormake}: this kormake). With
  -- test_jobs, no more than that many run at once (a device installs each as the same app).
  local runner = g.toolchain.fields.test_runner
  if runner then
    local words = {}
    for _, word in ipairs(util.as_list(runner)) do
      word = word:gsub("%%{root}", (root:gsub("%%", "%%%%"))):gsub("%%{kormake}", ((kfs.self() or "kormake"):gsub("%%", "%%%%")))
      words[#words + 1] = util.ninja_escape(q(word))
    end
    runner = " " .. table.concat(words, " ")
  else
    runner = ""
  end
  local jobs = g.toolchain.fields.test_jobs
  if jobs then w("pool test_pool\n  depth = " .. math.floor(jobs)) end
  w("rule run_test\n  command = " .. self .. ' _test $in $out' .. runner .. (jobs and "\n  pool = test_pool" or "") .. '\n  description = TEST $in')
  -- A host instance's test runs here, never under the target's runner.
  if runner ~= "" then w("rule run_test_host\n  command = " .. self .. ' _test $in $out\n  description = TEST $in') end
  -- From where the regeneration sees the same module paths (collect.lua's regen_dir).
  w("rule regen\n  command = " .. self .. " -C " .. util.ninja_escape(q(c.regen_dir)) .. " gen " .. target .. (opts.no_unity and " --no-unity" or "") ..
    (san and (" --sanitize " .. table.concat(san, ",")) or "") ..
    (opts.cxx and (" --cxx " .. util.ninja_escape(q(opts.cxx))) or "") .. "\n  generator = 1\n  description = KORMAKE gen " .. target)
  w("")

  -- Tool rules, one Ninja rule per (tool, rule). %{shader_includes} is an include flag for
  -- every module of the build with a shaders/ directory, so a game's shader can include the
  -- renderer's interface by name.
  local shader_includes = {}
  do
    local seen = {}
    for _, inst in ipairs(g.instances) do
      local d = not inst.dropped and inst.module.shaders_dir
      if d and not seen[d] then
        seen[d] = true
        shader_includes[#shader_includes + 1] = "-I " .. util.ninja_escape(q(d))
      end
    end
    table.sort(shader_includes)
  end
  local tool_rules = {}
  -- A rule as a consuming instance's platform sees it: the rule's fields with those of its
  -- variant for a module bound to an interface on that platform (the shader rule's DXIL
  -- form, `platforms = { ["renderer-d3d12"] = ... }`: the shader format follows the
  -- backend, not the OS), or else of its `platforms[<platform>]` variant, or else of the
  -- variant for the platform's toolchain family (the shader rule's Metal form,
  -- `platforms = { apple = ... }`). The Ninja rule's name carries the key only when a
  -- variant applies, so a rule without one is named as it always was.
  local function rule_variant(r, platform, family)
    if not r.platforms then return r, "" end
    local v, key
    local bound = g.platform_of(platform).bindings
    local ifaces = {}
    for iface in pairs(bound) do ifaces[#ifaces + 1] = iface end
    table.sort(ifaces) -- the same choice every run when two bound modules both have variants
    for _, iface in ipairs(ifaces) do
      for _, b in ipairs(bound[iface]) do
        if not v and r.platforms[b] then v, key = r.platforms[b], platform .. "_" .. b end
      end
    end
    if not v then v, key = r.platforms[platform], platform end
    if not v and family then v, key = r.platforms[family], platform end
    if not v then return r, "" end
    local out = {}
    for k, x in pairs(r) do if k ~= "platforms" then out[k] = x end end
    for k, x in pairs(v) do out[k] = x end
    return out, "_" .. util.symbol(key)
  end
  -- The rules an instance's tools export, as its platform sees them, each with the tool it
  -- came from and its Ninja rule's name: tool_<tool>_<rule>[_<platform>].
  -- A rule with `bound = "<module>"` applies only where that module is bound to an
  -- interface on the instance's platform (slang's shader_cpu, where renderer-cpu is).
  local function is_bound(platform, name)
    for _, list in pairs(g.platform_of(platform).bindings) do
      for _, b in ipairs(list) do if b == name then return true end end
    end
    return false
  end
  local function tool_rules_of(inst)
    local list = {}
    for _, t in ipairs(inst.tools) do
      for _, r0 in ipairs(t.instance.module.fields.exports or {}) do
        local r, variant = rule_variant(r0, inst.platform, flags(inst).tc.fields.family)
        -- A module may decline a tool's rule by name (its tool spec's skip_rules).
        if (not r.bound or is_bound(inst.platform, r.bound)) and not util.contains(t.spec.skip_rules or {}, r.name) then
          list[#list + 1] = { tool = t, rule = r, name = "tool_" .. util.symbol(t.instance.module.name) .. "_" .. r.name .. variant }
        end
      end
    end
    return list
  end
  for _, inst in ipairs(g.instances) do
    for _, tr in ipairs(inst.dropped and {} or tool_rules_of(inst)) do
      local t, r, rname = tr.tool, tr.rule, tr.name
      if not tool_rules[rname] then
        local tool, implicit = g.tool_path(t.instance)
        local cmd = r.command or util.fail("rule '%s' of '%s' has no command", r.name, t.instance.module.name)
        -- Ninja quotes $in and $out itself when a path needs it.
        -- The program in the host's spelling: Windows' CreateProcess takes "host/bin/x.exe"
        -- as a bare name to search for, and finds "host\bin\x.exe" where it is.
        cmd = cmd:gsub("%%{tool}", (util.ninja_escape(program(util.native(tool))):gsub("%%", "%%%%"))):gsub("%%{in}", "$in")
          :gsub("%%{out}", "$out"):gsub("%%{depfile}", "$out.d"):gsub("%%{rsp}", "$out.rsp"):gsub("%%{assets}", "$assets")
          :gsub("%%{shader_includes}", (table.concat(shader_includes, " "):gsub("%%", "%%%%")))
        -- The consuming platform's target ("apple-ios26.0-simulator"), for a compiler that
        -- writes code for it (Metal's, through slangc).
        if cmd:find("%{triple}", 1, true) then
          local triple = flags(inst).tc.fields.triple or util.fail("rule '%s' of '%s' wants %%{triple}; toolchain '%s' has none",
            r.name, t.instance.module.name, flags(inst).tc.rec.name)
          cmd = cmd:gsub("%%{triple}", (triple:gsub("%%", "%%%%")))
        end
        local rsp = r.per == "exe" and "\n  rspfile = $out.rsp\n  rspfile_content = $in" or ""
        w("rule " .. rname .. "\n  command = " .. cmd .. (r.deps == "depfile" and "\n  depfile = $out.d\n  deps = gcc" or "") .. rsp .. "\n  description = " .. r.name:upper() .. " $out")
        tool_rules[rname] = { rule = r, tool = tool, implicit = implicit }
      end
    end
  end
  w("")

  local function include_flags(inst, extra)
    local spell = flags(inst).tc.fields.flag_include
    local function flag(dir) return spell:format(q(dir)) end
    local dirs = {}
    for _, d in ipairs(inst.closure) do if d.module.include then dirs[#dirs + 1] = flag(d.module.include) end end
    if inst.module.include then dirs[#dirs + 1] = flag(inst.module.include) end
    -- The module's private include directories (a vendored library's own headers, which
    -- it includes by <name>): its own sources and tests only, never its dependents'.
    for _, p in ipairs(inst.module.fields.private_includes or {}) do
      dirs[#dirs + 1] = flag(util.join(inst.module.dir, p))
    end
    for _, e in ipairs(extra or {}) do dirs[#dirs + 1] = flag(e) end
    -- A package's headers, for the module that names it (not its dependents: the package
    -- stays behind the module's API).
    -- They are system headers (flag_include_system) where the toolchain spells one: their
    -- warnings are not ours.
    local fl = flags(inst)
    local sys = fl.tc.fields.flag_include_system or spell
    for _, ext in ipairs(externals_used(inst, { inst.module })) do
      local d = external_dir(fl, ext)
      for _, i in ipairs(fl.externals[ext].include or {}) do
        local p = external_path(d, i)
        if p then dirs[#dirs + 1] = sys:format(q(p)) end
      end
    end
    return table.concat(dirs, " ")
  end

  local function define_flags(inst)
    local spell = flags(inst).tc.fields.flag_define
    local ds = {}
    for _, d in ipairs(inst.module.fields.defines or {}) do ds[#ds + 1] = spell:format(d) end
    -- The modules bound to each interface on the instance's platform, by their place in the
    -- binding's list (its order of preference): KV_BIND_<INTERFACE>_<MODULE>=<1-based place>,
    -- and KV_BIND_<INTERFACE>_COUNT. Where two backends share a build, render's dispatcher
    -- compiles a case for each, and a backend knows it is not alone.
    local function up(s) return (s:upper():gsub("[^%w]", "_")) end
    local bound = g.platform_of(inst.platform).bindings
    for _, iface in ipairs(util.sorted_keys(bound)) do
      for i, b in ipairs(bound[iface]) do ds[#ds + 1] = spell:format(("KV_BIND_%s_%s=%d"):format(up(iface), up(b), i)) end
      ds[#ds + 1] = spell:format(("KV_BIND_%s_COUNT=%d"):format(up(iface), #bound[iface]))
    end
    return table.concat(ds, " ")
  end

  -- A module's sources: C++ (.cc) from src/ and then each of the platform's source
  -- directories, then Objective-C++ (.mm) the same way. .mm files live only where an Apple
  -- toolchain picks them up; they get a unity file of their own (a .cc and a .mm cannot
  -- share one), and the compiler takes the language from the extension.
  local source_exts = { "cc", "mm" }
  local function sources_of(inst)
    local f = inst.module.fields
    local list = {}
    if f.sources then
      for _, s in ipairs(f.sources) do list[#list + 1] = abs(inst.module.dir, s) end
      return list
    end
    if not inst.module.src_dir then return list end
    for _, ext in ipairs(source_exts) do
      for _, n in ipairs(util.sorted_files(inst.module.src_dir, ext)) do list[#list + 1] = util.join(inst.module.src_dir, n) end
      for _, sd in ipairs(flags(inst).srcdirs) do
        local d = util.join(inst.module.src_dir, sd)
        if kfs.isdir(d) then for _, n in ipairs(util.sorted_files(d, ext)) do list[#list + 1] = util.join(d, n) end end
      end
    end
    return list
  end

  -- A code-generation profile's flags (a module's `codegen`, a rule's): the toolchain's
  -- codegen_profiles[name]. "" for none.
  local function profile_flags(inst, name, who)
    if not name then return "" end
    local p = (flags(inst).tc.fields.codegen_profiles or {})[name]
    if not p then util.fail("%s asks for codegen profile '%s', which toolchain '%s' does not have", who, name, flags(inst).tc.rec.name) end
    return table.concat(p, " ")
  end
  -- extra: `implicit`, a file the compile waits for (a generated source's include);
  -- `flags`, added to the toolchain's (code another program wrote: flags_generated; a
  -- codegen profile; an instruction set's).
  local function compile(inst, src, obj, includes, defines, extra)
    local fl = flags(inst)
    local cxxflags = fl.cxxflags
    local more = ""
    local add = extra and extra.flags and util.trim(extra.flags) or ""
    if add ~= "" then
      cxxflags = cxxflags .. " " .. add
      if inst.prefix == "" then more = "\n  cxxflags = " .. cxxflags end
    end
    local ov = overrides(inst)
    if add ~= "" and inst.prefix ~= "" then ov = ov:gsub("\n  cxxflags = [^\n]*", function() return "\n  cxxflags = " .. cxxflags end) end
    w(string.format("build %s: %s %s%s\n  includes = %s\n  defines = %s%s%s", util.ninja_escape(obj), rule(inst, "cxx"), util.ninja_escape(src),
      extra and extra.implicit and (" | " .. util.ninja_escape(extra.implicit)) or "", includes, defines, ov, more))
    -- The command the toolchain's cxx rule runs, with this file's values in it. clangd takes
    -- the first entry for a file: the target's, since the host's come later.
    local vars = { cxx = program(fl.cxx), cxxflags = cxxflags, defines = defines, includes = includes, ["in"] = program(src), out = program(obj) }
    local command = fl.tc.fields.rules.cxx.command:gsub("%$(%w+)", function(v) return vars[v] or "" end)
    cc[#cc + 1] = string.format("{\"directory\": %s, \"file\": %s, \"output\": %s, \"command\": %s}",
      util.json_string(build), util.json_string(src), util.json_string(obj), util.json_string(command))
  end

  -- A rule that writes code compiled into the module that has its inputs (slang's
  -- shader_cpu: a game shader as C++ for the CPU renderer). Each input gets, in the
  -- module's gen directory:
  --   - with `wrap`, the file the rule's tool reads instead of the input: the first of the
  --     rule's wrappers whose `match` the input contains (or that has none), %{in} the
  --     input's path;
  --   - the rule's output, %{stem} from the input;
  --   - with `glue`, a C++ file compiled into the module, quietly (flags_generated): %{out}
  --     the output's path, %{stem}, and %{symbol}, the module's and the stem's symbols;
  --   - with `engine_module`, an engine module that glue defines (kv::<it>, %{symbol}
  --     filled in), listed in each executable's module list after the module that owns it.
  local function generated_sources(inst, gen)
    local list = {}
    inst.generated_modules = {}
    for _, tr0 in ipairs(tool_rules_of(inst)) do
      local t, r, rname = tr0.tool, tr0.rule, tr0.name
      local dir = util.join(inst.module.dir, util.dirname(r.inputs))
      if r.glue and r.per ~= "exe" and kfs.isdir(dir) then
        local pat_glob = util.basename(r.inputs)
        for _, n in ipairs(kfs.listdir(dir)) do
          if util.matches(n, pat_glob) then
            local stem = util.stem(n)
            local input = util.join(dir, n):gsub("\\", "/")
            local symbol = util.symbol(inst.module.name) .. "_" .. util.symbol(stem)
            local function fill(text, out)
              return (text:gsub("%%{(%w+)}", function(v)
                if v == "in" then return input elseif v == "stem" then return stem elseif v == "symbol" then return symbol
                elseif v == "out" then return out else util.fail("rule '%s': %%{%s} is not %%{in}, %%{out}, %%{stem} or %%{symbol}", r.name, v) end
              end))
            end
            local src = util.join(dir, n)
            if r.wrap then
              local body = kfs.read(src) or ""
              local chosen
              for _, wr in ipairs(r.wrap) do
                if not chosen and (not wr.match or body:find(wr.match, 1, true)) then chosen = wr end
              end
              if not chosen then util.fail("rule '%s': no wrapper for %s", r.name, src) end
              src = util.join(build, gen .. "/" .. stem .. "." .. r.name .. "." .. util.ext(n))
              kfs.write(src, fill(chosen.text, ""))
            end
            local outname = r.outputs:gsub("%%{stem}", stem)
            local outpath = gen .. "/" .. outname
            local tr = tool_rules[rname]
            w(string.format("build %s: %s %s%s", util.ninja_escape(outpath), rname, util.ninja_escape(src),
              tr.implicit and (" | " .. util.ninja_escape(tr.tool)) or ""))
            local glue = util.join(build, gen .. "/" .. stem .. "." .. r.name .. ".cc")
            kfs.write(glue, fill(r.glue, util.join(build, outpath):gsub("\\", "/")))
            list[#list + 1] = { src = glue, implicit = outpath, name = stem .. "." .. r.name, codegen = r.codegen }
            if r.engine_module then inst.generated_modules[#inst.generated_modules + 1] = fill(r.engine_module, "") end
          end
        end
      end
    end
    return list
  end

  -- The generated module list for an executable: every engine module in its closure,
  -- dependencies first, then itself.
  local function module_list_source(inst)
    local lines = { "// generated by kormake: the module list of " .. inst.module.name .. ", in dependency order.", '#include "korvid/core/module.h"' }
    local names = {}
    local function consider(i)
      if i.module.fields.engine_module then
        lines[#lines + 1] = string.format('#include "korvid/%s/module.h"', i.module.name)
        names[#names + 1] = "&kv::" .. util.symbol(i.module.name) .. "_module"
      end
      for _, gm in ipairs(i.generated_modules or {}) do
        lines[#lines + 1] = string.format("namespace kv { extern const ModuleDesc %s; }", gm)
        names[#names + 1] = "&kv::" .. gm
      end
    end
    for _, d in ipairs(inst.closure) do consider(d) end
    consider(inst)
    lines[#lines + 1] = "\nnamespace {\nconstinit const kv::ModuleDesc* const k_modules[] = {"
    for _, n in ipairs(names) do lines[#lines + 1] = "    " .. n .. "," end
    lines[#lines + 1] = "};\n}\n\nkv::Span<const kv::ModuleDesc* const> kv_module_list() {\n    return kv::Span<const kv::ModuleDesc* const>(k_modules, KV_COUNTOF(k_modules));\n}"
    return table.concat(lines, "\n") .. "\n"
  end

  local libs_by_key = {}
  local outputs_by_key = {} -- what a selectable instance's phony stands for
  local runtime_by_key = {} -- files an executable needs next to it

  -- What a module stages beside its executables: its tool outputs (shaders and the like) and
  -- its assets, as Ninja edges; the paths, for every executable that links it. A header-only
  -- module stages its own too (the render interface's shaders) though it compiles nothing.
  local function stage_runtime(inst, gen)
    local m = inst.module
    local pre = inst.prefix
    -- Tool outputs: shaders and the like.
    local runtime = {}
    for _, tr0 in ipairs(tool_rules_of(inst)) do
      local t, r, rname = tr0.tool, tr0.rule, tr0.name
      local dir = util.join(m.dir, util.dirname(r.inputs))
      -- A per-exe rule runs once per executable, below; a rule with glue is compiled into
      -- the module (generated_sources), not staged.
      if r.per ~= "exe" and not r.glue and kfs.isdir(dir) then
        local pat_glob = util.basename(r.inputs)
        for _, n in ipairs(kfs.listdir(dir)) do
          if util.matches(n, pat_glob) then
            local outname = r.outputs:gsub("%%{stem}", util.stem(n))
            local outpath = t.spec.install and (pre .. "bin/" .. t.spec.install .. "/" .. outname) or (gen .. "/" .. outname)
            local tr = tool_rules[rname]
            w(string.format("build %s: %s %s%s", util.ninja_escape(outpath), rname, util.ninja_escape(util.join(dir, n)),
              tr.implicit and (" | " .. util.ninja_escape(tr.tool)) or ""))
            runtime[#runtime + 1] = outpath
          end
        end
      end
    end
    -- Assets are not staged here: goldberg cooks them, once per program (cook, below).
    return runtime
  end

  -- Assets. Nothing copies them: goldberg (tools/goldberg, built for the host) cooks the
  -- assets/ of a program's modules into that program's output tree, and the program
  -- finds the tree through <program>.assets beside it. One edge per program, which runs
  -- at every build: Ninja does not know goldberg's inputs (a rule file that does not exist
  -- yet is one), so goldberg's own state decides, and a cook with nothing to do costs a
  -- scan. Its stamp changes only when the tree did, so nothing downstream reruns for it.
  --
  -- The tree: build/assets/<program>/ in every config but ship (dev and debug share it);
  -- in ship, dist/<program>/assets/ for a game that packages as a plain directory, where
  -- the tree is the shipped one, and build/assets-ship/<program>/ for everything else (a
  -- test, a program that packages as a bundle or an .apk, which copies it in).
  --
  -- What runs is the cook (collect.lua): kormake.lua's or korvid.config's `cook "..."`,
  -- by default goldberg. Its first word is a program: an executable module, built for the
  -- host (goldberg), or a script beside the file that names it (`copy_assets` is
  -- copy_assets.sh, run with sh, or copy_assets.bat on Windows), or anything on PATH.
  -- The rest are its words, with {roots} (the roots file), {stamp} (a file it rewrites only
  -- when the tree changed), {tree} (the program's output tree) and {program} filled in.
  -- A cook that uses {in} (an asset root), {name} (its module) or {out} ({tree}/{name})
  -- runs once per root. One that does not write {stamp}, or runs per root, is run by
  -- kormake (_cook), which writes the stamp after it, every time.
  local spec, cook_rule = nil, false
  local function json_string(v) return '"' .. v:gsub("\\", "\\\\"):gsub('"', '\\"') .. '"' end
  local function cook_spec(for_module)
    if spec then return spec end
    local ck = c.cook
    local words = {}
    for word in ck.text:gmatch("%S+") do words[#words + 1] = word end
    local known = { roots = true, stamp = true, tree = true, program = true, ["in"] = true, name = true, out = true }
    for _, word in ipairs(words) do
      for v in word:gmatch("{([^}]*)}") do
        if not known[v] then util.fail("%s: cook: {%s} is not {roots}, {stamp}, {tree}, {program}, {in}, {name} or {out}", ck.file, v) end
      end
    end
    spec = { args = {}, implicit = {}, tool = words[1] }
    for i = 2, #words do spec.args[#spec.args + 1] = words[i] end
    spec.per_root = ck.text:find("{in}", 1, true) or ck.text:find("{name}", 1, true) or ck.text:find("{out}", 1, true)
    spec.stamps = ck.text:find("{stamp}", 1, true) ~= nil
    local first = words[1]
    for _, i in ipairs(g.instances) do
      if i.module.name == first and i.module.fields.output == "exe" and not i.dropped then
        local tool = g.tool_path(i)
        spec.program = { util.native(tool) }
        spec.implicit[#spec.implicit + 1] = tool
      end
    end
    if not spec.program then
      if c.by_name[first] then
        util.fail("'%s' has assets, and its cook (%s) runs %s, which cannot build for this host", for_module, ck.file, first)
      end
      local function script(path)
        if path:match("%.bat$") then
          if KORMAKE_HOST ~= "windows" then util.fail("%s: cook: %s is a Windows script; name it without the extension to have %s.sh run elsewhere", ck.file, first, first:gsub("%.bat$", "")) end
          return { "cmd", "/c", util.native(path) }
        elseif path:match("%.sh$") then return { KORMAKE_HOST == "windows" and "sh" or "/bin/sh", util.native(path) } end
        return { util.native(path) }
      end
      local base = util.is_abs(first) and first or util.join(ck.dir, first)
      if not first:match("%.%w+$") and KORMAKE_HOST == "windows" and kfs.isfile(base .. ".bat") then
        spec.program = script(base .. ".bat"); spec.implicit[#spec.implicit + 1] = base .. ".bat"
      elseif not first:match("%.%w+$") and kfs.isfile(base .. ".sh") then
        spec.program = script(base .. ".sh"); spec.implicit[#spec.implicit + 1] = base .. ".sh"
      elseif kfs.isfile(base) then
        spec.program = script(base); spec.implicit[#spec.implicit + 1] = base
      elseif first == "goldberg" then
        util.fail("'%s' has assets, and nothing here cooks them: the goldberg module (the engine's tools/goldberg) is not on a module path, or cannot build for this host. A cook \"...\" in kormake.lua or korvid.config names another",
          for_module)
      else
        spec.program = { first } -- on PATH
      end
    end
    return spec
  end
  -- One command line of the cook, its words filled in from vars.
  local function cook_line(sp, vars)
    local out = {}
    -- A .bat runs as cmd /c, which does not read its line by the C runtime's rules: with
    -- more than two quotes it strips the first and the last of the whole rest, and the last
    -- argument loses its closing quote. `cmd /d /s /c "..."` is the form it reads as
    -- written (/s: exactly the outer pair taken off; /d: no AutoRun), as platform's
    -- process_win32.cc runs it.
    local via_cmd = sp.program[1] == "cmd" and sp.program[2] == "/c"
    for i, word in ipairs(sp.program) do if not (via_cmd and i <= 2) then out[#out + 1] = program(word) end end
    for _, word in ipairs(sp.args) do
      local filled = word:gsub("{([^}]*)}", function(v) return vars[v] end)
      out[#out + 1] = filled:find("[%s'\"$]") and q(filled) or filled
    end
    local line = table.concat(out, " ")
    if via_cmd then line = 'cmd /d /s /c "' .. line .. '"' end
    return line
  end
  local function cook(inst, program_name, is_game)
    local roots, seen = {}, {}
    local function add(m) if m.assets_dir and not seen[m.name] then seen[m.name] = true; roots[#roots + 1] = m end end
    for _, d in ipairs(inst.closure) do add(d.module) end
    add(inst.module)
    if #roots == 0 then return nil end
    local sp = cook_spec(inst.module.name)
    local pre = inst.prefix
    local ship = g.config == "ship"
    local tree
    if not ship then tree = util.join(c.build_root, "assets/" .. program_name)
    elseif is_game and not flags(inst).tc.fields.package then tree = util.join(c.base_dir, "dist/" .. program_name .. "/assets")
    else tree = util.join(c.build_root, "assets-ship/" .. program_name) end
    local state = util.join(c.build_root, "goldberg/" .. program_name .. "/" .. (ship and "ship" or "dev") .. "/state.json")
    local lines = {}
    for _, m in ipairs(roots) do lines[#lines + 1] = string.format('    { "name": %s, "dir": %s }', json_string(m.name), json_string(m.assets_dir)) end
    local roots_rel = pre .. "goldberg/" .. program_name .. ".roots"
    local stamp = pre .. "goldberg/" .. program_name .. ".cooked"
    kfs.mkdirp(util.join(build, pre .. "goldberg"))
    kfs.mkdirp(util.join(build, pre .. "bin"))
    kfs.write(util.join(build, roots_rel), string.format('{\n  "game": %s,\n  "out": %s,\n  "state": %s,\n  "project": %s,\n  "roots": [\n%s\n  ]\n}\n',
      json_string(program_name), json_string(tree), json_string(state), json_string(c.base_dir), table.concat(lines, ",\n")), true)
    kfs.write(util.join(build, pre .. "bin/" .. program_name .. ".assets"), tree .. "\n", true)
    if not cook_rule then
      cook_rule = true
      -- The command is the edge's own ($cmd); kormake's _cook runs a list of them.
      w("rule cook\n  command = $cmd\n  restat = 1\n  description = COOK $in")
      -- Never a file, so always out of date: what makes the cook edges run every time.
      w("build always-cook: phony")
    end
    local vars = { roots = util.native(roots_rel), stamp = util.native(stamp), tree = util.native(tree), program = program_name }
    local cmd
    if not sp.per_root and sp.stamps then
      cmd = cook_line(sp, vars)
    else
      local runs = {}
      for _, m in ipairs(sp.per_root and roots or { false }) do
        if m then vars["in"], vars.name, vars.out = util.native(m.assets_dir), m.name, util.native(util.join(tree, m.name)) end
        runs[#runs + 1] = cook_line(sp, vars)
      end
      local list = pre .. "goldberg/" .. program_name .. ".cook"
      kfs.write(util.join(build, list), table.concat(runs, "\n") .. "\n", true)
      cmd = program(util.native(kfs.self() or "kormake")) .. " _cook " .. q(list) .. " " .. q(stamp) .. (sp.stamps and " --stamped" or "")
    end
    local implicit = { "always-cook" }
    for _, p in ipairs(sp.implicit) do implicit[#implicit + 1] = util.ninja_escape(p) end
    w(string.format("build %s: cook %s | %s\n  cmd = %s", util.ninja_escape(stamp), util.ninja_escape(roots_rel), table.concat(implicit, " "),
      (cmd:gsub("%$", "$$"))))
    return stamp, tree
  end

  -- What a program (an executable, or a module's tests) links and has staged beside it:
  -- the instance's own library (a test's module) and its closure's, last dependency first;
  -- every file the instance and its closure stage (assets, shaders) and a package's runtime
  -- files; and the externals' link flags.
  local function link_inputs(inst)
    local libs, mods, staged = {}, {}, {}
    if libs_by_key[inst.key] then libs[#libs + 1] = libs_by_key[inst.key] end
    for i = #inst.closure, 1, -1 do
      local d = inst.closure[i]
      if libs_by_key[d.key] then libs[#libs + 1] = libs_by_key[d.key] end
      mods[#mods + 1] = d.module
      for _, r in ipairs(runtime_by_key[d.key] or {}) do staged[#staged + 1] = r end
    end
    mods[#mods + 1] = inst.module
    for _, r in ipairs(runtime_by_key[inst.key] or {}) do staged[#staged + 1] = r end
    local extlibs, extdirs = extlibs_of(inst, mods)
    for _, r in ipairs(runtime_of(inst, mods)) do staged[#staged + 1] = r end
    return { libs = libs, staged = staged, extlibs = extlibs,
             flags = util.trim(table.concat(extlibs, " ") .. " " .. table.concat(extdirs, " ")) }
  end
  -- A program's link. What is staged for it is order-only: building the program also
  -- builds what it reads, and a changed asset does not relink it.
  local function link(inst, exe, objs, li, more_ldflags)
    w(string.format("build %s: %s %s %s%s\n  libs = %s%s%s", util.ninja_escape(exe), rule(inst, "link"), util.ninja_paths(objs),
      util.ninja_paths(li.libs), #li.staged > 0 and (" || " .. util.ninja_paths(li.staged)) or "", li.flags, overrides(inst),
      more_ldflags and ("\n  ldflags = " .. flags(inst).ldflags .. " " .. more_ldflags) or ""))
  end
  -- An executable's file under bin/, and the link flags it adds. On a toolchain whose
  -- applications are libraries the OS loads (app_module, app_file: Android), an executable
  -- that links the window is one: lib<name>.so, linked shared. inst.exe_file is what
  -- `package` reads.
  local function exe_file(inst)
    local tcf = flags(inst).tc.fields
    local app = false
    if tcf.app_module and tcf.app_file and inst.prefix == "" then
      for _, d in ipairs(inst.closure) do if d.module.name == tcf.app_module then app = true end end
    end
    inst.is_app = app
    inst.exe_file = app and tcf.app_file:format(exe_name(inst)) or (exe_name(inst) .. tcf.exe_suffix)
    return inst.exe_file, app and flag_list(tcf.app_ldflags) or nil
  end

  -- Libraries and executables.
  for _, inst in ipairs(g.order) do
    local m = inst.module
    local f = m.fields
    local output = f.output or "lib"
    local sym = util.symbol(m.name)
    if inst.dropped then goto continue end
    if f.provides == "toolchain" or f.system or f.prebuilt then goto continue end
    if output == "headers" then
      runtime_by_key[inst.key] = stage_runtime(inst, inst.prefix .. "gen/" .. sym)
      outputs_by_key[inst.key] = runtime_by_key[inst.key]
      goto continue
    end
    do
      local srcs = sources_of(inst)
      local pre = inst.prefix
      local naming = flags(inst).tc.fields
      local gen = pre .. "gen/" .. sym
      kfs.mkdirp(util.join(build, gen))
      local includes = include_flags(inst, { util.join(build, gen) })
      local defines = define_flags(inst)
      -- The module's codegen profile, for its own sources; the instruction sets its
      -- isa_sources are built again for (where the toolchain names them), each said to the
      -- module's sources as KV_HAS_ISA_<SET>=1 and to its copies as KV_ISA_<SET>=1.
      local own = { flags = profile_flags(inst, f.codegen, m.name) }
      local isa_sets = {}
      for _, set in ipairs(util.sorted_keys(f.isa_sources or {})) do
        local isa_flags = (flags(inst).tc.fields.isa or {})[set]
        if isa_flags then
          isa_sets[#isa_sets + 1] = { name = set, flags = table.concat(isa_flags, " "), sources = util.as_list(f.isa_sources[set]) }
          defines = defines .. " " .. flags(inst).tc.fields.flag_define:format("KV_HAS_ISA_" .. set:upper() .. "=1")
        end
      end
      local objs = {}
      local unity = f.unity
      if unity == nil then unity = not opts.no_unity end
      -- Code a tool writes (generated_sources): each file alone, after its tool has run.
      inst.generated_objs = {}
      local quiet = table.concat(flags(inst).tc.fields.flags_generated or {}, " ")
      for _, gs in ipairs(generated_sources(inst, gen)) do
        local obj = pre .. "obj/" .. sym .. "/gen_" .. gs.name:gsub("[^%w_]", "_") .. naming.obj_suffix
        compile(inst, gs.src, obj, includes, defines, { implicit = gs.implicit, flags = quiet .. " " .. profile_flags(inst, gs.codegen, "rule of " .. m.name) })
        objs[#objs + 1] = obj
        inst.generated_objs[#inst.generated_objs + 1] = obj
      end
      if output == "exe" and f.module_list ~= false then
        local path = util.join(build, gen .. "/modules.gen.cc")
        kfs.write(path, module_list_source(inst))
        srcs[#srcs + 1] = path
      end
      -- One unity file per language with more than one source (unity.cc, unity.mm); a
      -- language with one source, or a build without unity, compiles each file alone.
      for _, ext in ipairs(source_exts) do
        local group = {}
        for _, s in ipairs(srcs) do if util.ext(s) == ext then group[#group + 1] = s end end
        local tail = ext == "cc" and "" or ("_" .. ext) -- a .mm's object never meets a .cc's of the same name
        if unity and #group > 1 then
          local lines = { "// generated by kormake: the unity build of " .. m.name .. (ext == "cc" and "" or (" (" .. ext .. ")")) }
          for _, s in ipairs(group) do lines[#lines + 1] = '#include "' .. s .. '"' end
          local path = util.join(build, gen .. "/unity." .. ext)
          kfs.write(path, table.concat(lines, "\n") .. "\n")
          local obj = pre .. "obj/" .. sym .. "/unity" .. tail .. naming.obj_suffix
          compile(inst, path, obj, includes, defines, own)
          objs[#objs + 1] = obj
        else
          for _, s in ipairs(group) do
            local rel = s:sub(#m.dir + 2):gsub("^src/", "")
            if s:sub(1, #build) == build then rel = "gen_" .. util.basename(s) end
            local obj = pre .. "obj/" .. sym .. "/" .. rel:gsub("%." .. ext .. "$", "") .. tail .. naming.obj_suffix
            compile(inst, s, obj, includes, defines, own)
            objs[#objs + 1] = obj
          end
        end
      end
      -- Anything else a module's `sources` names compiles alone, as it always has.
      for _, s in ipairs(srcs) do
        if not util.contains(source_exts, util.ext(s)) then
          local rel = s:sub(#m.dir + 2):gsub("^src/", "")
          local obj = pre .. "obj/" .. sym .. "/" .. rel:gsub("%.[^%./]*$", "") .. naming.obj_suffix
          compile(inst, s, obj, includes, defines, own)
          objs[#objs + 1] = obj
        end
      end
      -- The instruction sets' copies: each listed source again, alone.
      for _, set in ipairs(isa_sets) do
        for _, rel0 in ipairs(set.sources) do
          local s = abs(m.dir, rel0)
          local rel = s:sub(#m.dir + 2):gsub("^src/", "")
          local obj = pre .. "obj/" .. sym .. "/" .. rel:gsub("%.[^%./]*$", "") .. "_" .. set.name .. naming.obj_suffix
          compile(inst, s, obj, includes, defines .. " " .. flags(inst).tc.fields.flag_define:format("KV_ISA_" .. set.name:upper() .. "=1"),
            { flags = own.flags .. " " .. set.flags })
          objs[#objs + 1] = obj
        end
      end
      local runtime = stage_runtime(inst, gen)
      runtime_by_key[inst.key] = runtime

      if output == "lib" then
        if #objs > 0 then
          local lib = pre .. "lib/" .. naming.lib_file:format("kv_" .. sym)
          w(string.format("build %s: %s %s%s", util.ninja_escape(lib), rule(inst, "ar"), util.ninja_paths(objs), overrides(inst)))
          libs_by_key[inst.key] = lib
          if inst.in_roots then all[#all + 1] = lib end
        end
        local outs = { libs_by_key[inst.key] }
        for _, r in ipairs(runtime) do outs[#outs + 1] = r end
        outputs_by_key[inst.key] = outs
      elseif output == "exe" then
        local li = link_inputs(inst)
        inst.extlibs = li.extlibs
        -- Its assets, cooked: building the program builds what it reads. The cook tool's own
        -- (goldberg's window) cook after it is built, with it: its cook edge runs it, so the
        -- program cannot wait for that cook as every other does.
        local cook_stamp, tree = cook(inst, exe_name(inst), true)
        inst.assets_tree = tree
        local cooks_itself = cook_stamp and spec and spec.tool == inst.module.name
        if cooks_itself then
          if inst.in_roots then all[#all + 1] = cook_stamp end
        elseif cook_stamp then li.staged[#li.staged + 1] = cook_stamp end
        local file, app_ldflags = exe_file(inst)
        local exe = pre .. "bin/" .. file
        link(inst, exe, objs, li, app_ldflags)
        if inst.in_roots then all[#all + 1] = exe end
        local outs = { exe }
        outputs_by_key[inst.key] = outs
        -- Rules that run once per executable over its cooked assets (a pack): part of the
        -- default build in the configs the rule names, buildable by name in the others.
        -- Their outputs are what `package` ships beside the executable. The rule's command
        -- names the tree as %{assets}; its input is the cook's stamp.
        inst.per_exe_outputs = {}
        for _, tr0 in ipairs(tool_rules_of(inst)) do
          local r = tr0.rule
          if r.per == "exe" and cook_stamp then
            local tr = tool_rules[tr0.name]
            local outp = pre .. "bin/" .. r.outputs:gsub("%%{exe}", exe_name(inst))
            local implicit = tr.implicit and (" | " .. util.ninja_escape(tr.tool)) or ""
            w(string.format("build %s: %s %s%s\n  assets = %s", util.ninja_escape(outp), tr0.name, util.ninja_escape(cook_stamp), implicit,
              util.ninja_escape(q(tree))))
            inst.per_exe_outputs[#inst.per_exe_outputs + 1] = outp
            local configs = util.as_list(r.configs or {})
            for _, cf in ipairs(configs) do
              if not util.contains(c.master.configs, cf) then
                util.fail("rule '%s' of '%s' names config '%s', which is not in the master file's configs", r.name, tr0.tool.instance.module.name, cf)
              end
            end
            if util.contains(configs, g.config) then
              if inst.in_roots then all[#all + 1] = outp end
              outs[#outs + 1] = outp
            end
          end
        end
      end
    end
    ::continue::
  end

  -- Tests: one executable per selectable module with a tests/ directory, never a unity
  -- build. `tests` and `check` hold the ones a root asks for; the rest are built and run
  -- by name (kormake test MODULE).
  for _, inst in ipairs(g.order) do
    if not (inst.selectable and inst.module.tests_dir) then goto skip end
    do
      local m = inst.module
      local sym = util.symbol(m.name)
      local pre = inst.prefix
      local srcs = {}
      for _, n in ipairs(util.sorted_files(m.tests_dir, "cc")) do srcs[#srcs + 1] = util.join(m.tests_dir, n) end
      if #srcs == 0 then goto skip end
      -- test_deps: modules the tests use and the module does not (a picture decoder to read
      -- reference pictures), each with its closure: their headers and their libraries.
      local extra_dirs, extra = { m.tests_dir }, {}
      for _, name in ipairs(util.as_list(m.fields.test_deps or {})) do
        local found
        for _, i in ipairs(g.instances) do
          if i.module.name == name and i.platform == inst.platform and i.prefix == inst.prefix and not i.dropped then found = i end
        end
        if not found then util.fail("%s: test_deps names '%s', which this build does not have for %s", m.name, name, inst.platform) end
        for _, d in ipairs(found.closure) do extra[#extra + 1] = d end
        extra[#extra + 1] = found
      end
      for _, d in ipairs(extra) do if d.module.include then extra_dirs[#extra_dirs + 1] = d.module.include end end
      local includes = include_flags(inst, extra_dirs)
      local objs = {}
      for _, s in ipairs(srcs) do
        local obj = pre .. "obj/" .. sym .. "/tests/" .. util.basename(s):gsub("%.cc$", "") .. flags(inst).tc.fields.obj_suffix
        compile(inst, s, obj, includes, define_flags(inst))
        objs[#objs + 1] = obj
      end
      -- An executable's generated code (generated_sources) is not in a library: its tests
      -- link the objects, so they can list the modules (a library's tests have them in it).
      if m.fields.output == "exe" then
        for _, o in ipairs(inst.generated_objs or {}) do objs[#objs + 1] = o end
      end
      -- Linked as an executable is. The stamp takes what is staged as implicit inputs, so
      -- a changed asset reruns the test without relinking it.
      local li = link_inputs(inst)
      local have = {}
      for _, l in ipairs(li.libs) do have[l] = true end
      for i = #extra, 1, -1 do
        local l = libs_by_key[extra[i].key]
        if l and not have[l] then li.libs[#li.libs + 1] = l; have[l] = true end
      end
      local exe = pre .. "bin/" .. sym .. "_tests" .. flags(inst).tc.fields.exe_suffix
      link(inst, exe, objs, li)
      -- An executable's tests do not link it (it has a main of its own): they run it, so
      -- it is built before they are, and they run again when it changes.
      local implicit = {}
      for _, p in ipairs(li.staged) do implicit[#implicit + 1] = p end
      -- A test's assets are cooked as a game's are, into a tree of its own.
      local cook_stamp = cook(inst, sym .. "_tests", false)
      if cook_stamp then implicit[#implicit + 1] = cook_stamp end
      if m.fields.output == "exe" and outputs_by_key[inst.key] then implicit[#implicit + 1] = outputs_by_key[inst.key][1] end
      w(string.format("build %s: %s %s%s", util.ninja_escape(pre .. "test/" .. sym .. ".ok"),
        (pre ~= "" and runner ~= "") and "run_test_host" or "run_test", util.ninja_escape(exe),
        #implicit > 0 and (" | " .. util.ninja_paths(implicit)) or ""))
      if inst.tests then
        tests[#tests + 1] = exe
        checks[#checks + 1] = pre .. "test/" .. sym .. ".ok"
      end
      inst.test_stamp = pre .. "test/" .. sym .. ".ok"
    end
    ::skip::
  end

  -- Heavy tests: one more executable per module with a tests/heavy/ directory, linked as
  -- its tests are. Built with the tests, so they keep compiling, and never run by Ninja:
  -- `kormake test --heavy` builds the phony heavy/<module> (the executable and what it
  -- runs) and runs it itself, every time, with no stamp. They are for what takes minutes
  -- and gigabytes, and for measurements, which have no pass to remember.
  for _, inst in ipairs(g.order) do
    local hdir = inst.selectable and inst.module.tests_dir and util.join(inst.module.tests_dir, "heavy")
    if not (hdir and kfs.isdir(hdir)) then goto next_heavy end
    do
      local m = inst.module
      local sym = util.symbol(m.name)
      local pre = inst.prefix
      local srcs = util.sorted_files(hdir, "cc")
      if #srcs == 0 then goto next_heavy end
      local includes = include_flags(inst, { m.tests_dir, hdir })
      local objs = {}
      for _, n in ipairs(srcs) do
        local obj = pre .. "obj/" .. sym .. "/heavy/" .. n:gsub("%.cc$", "") .. flags(inst).tc.fields.obj_suffix
        compile(inst, util.join(hdir, n), obj, includes, define_flags(inst))
        objs[#objs + 1] = obj
      end
      local li = link_inputs(inst)
      local exe = pre .. "bin/" .. sym .. "_heavy" .. flags(inst).tc.fields.exe_suffix
      link(inst, exe, objs, li)
      local needs = { exe }
      for _, p in ipairs(li.staged) do needs[#needs + 1] = p end
      if m.fields.output == "exe" and outputs_by_key[inst.key] then needs[#needs + 1] = outputs_by_key[inst.key][1] end
      w(string.format("build %s: phony %s", util.ninja_escape(pre .. "heavy/" .. sym), util.ninja_paths(needs)))
      if inst.tests then tests[#tests + 1] = exe end
      inst.heavy = { target = pre .. "heavy/" .. sym, exe = exe, host = pre ~= "" }
    end
    ::next_heavy::
  end

  w("")
  w("build tests: phony " .. util.ninja_paths(tests))
  w("build check: phony " .. util.ninja_paths(checks))
  w("build all: phony " .. util.ninja_paths(all) .. " tests")
  w("default all")
  -- A phony per selectable module, named for it: what `kormake NAME` asks Ninja for.
  w("")
  for _, inst in ipairs(g.order) do
    if inst.selectable then
      local outs = outputs_by_key[inst.key] or {}
      w(string.format("build %s: phony%s", util.ninja_escape(inst.module.name), #outs > 0 and (" " .. util.ninja_paths(outs)) or ""))
    end
  end
  -- Regenerate when any input changes.
  local inputs = { util.join(root, "kormake.lua") }
  if c.regen_config then inputs[#inputs + 1] = c.regen_config end
  -- Every module file collected, built here or not: an edit may let a left-out one in.
  for _, rec in ipairs(c.modules) do inputs[#inputs + 1] = rec.file end
  -- The .pc files pkg-config answered from: a package upgraded is a build regenerated.
  for _, pc in ipairs(g.regen_inputs or {}) do inputs[#inputs + 1] = pc end
  w("build build.ninja: regen | " .. util.ninja_paths(inputs))

  kfs.mkdirp(util.join(build, "test"))
  kfs.mkdirp(util.join(build, "host/test"))
  kfs.write(util.join(build, "build.ninja"), table.concat(out, "\n") .. "\n", true)
  kfs.write(util.join(build, "compile_commands.json"), "[\n" .. table.concat(cc, ",\n") .. "\n]\n")
  -- Where clangd looks: a plain target's flags, never a sanitized build's.
  if not san then kfs.write(util.join(c.build_root, "compile_commands.json"), "[\n" .. table.concat(cc, ",\n") .. "\n]\n") end
  return build
end

return M
