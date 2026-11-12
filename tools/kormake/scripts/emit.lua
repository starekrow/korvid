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

local function tool_path(inst)
  local f = inst.module.fields
  if f.system then return f.system, false end
  if f.prebuilt then
    local p = f.prebuilt[inst.platform]
    if not p then util.fail("tool '%s' has no prebuilt binary for %s", inst.module.name, inst.platform) end
    return util.join(inst.module.dir, p), true
  end
  -- The host instance, built in this graph.
  if f.output == "exe" then return inst.prefix .. "bin/" .. exe_name(inst) .. inst.toolchain.fields.exe_suffix, true end
  util.fail("tool '%s' is neither built from source, prebuilt nor a system lookup", inst.module.name)
end

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
  local build = util.join(c.build_root, target)
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
  local function flags_of(tc, conf, cross)
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
      externals = tcf.externals or {},
    }
  end
  local target_flags = flags_of(g.toolchain, g.config, false)
  g.ninja = target_flags.tc.probed.ninja
  local function flags(inst)
    if inst.prefix == "" then return target_flags end
    inst.flags = inst.flags or flags_of(inst.toolchain, inst.config, inst.platform ~= g.platform)
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

  -- An external the toolchain keeps in a directory (a prebuilt package or an installed SDK)
  -- is found wherever KORMAKE_<NAME>_DIR says, else where the variable its `env` names
  -- says (an SDK's installer sets one), else at its `dir` under the project root. A
  -- package that is not there stops the generation, with the toolchain's instructions.
  local external_dirs = {}
  local function external_dir(fl, ext)
    local e = fl.externals[ext]
    if not e.dir and not e.env then return nil end
    local key = fl.tc.rec.name .. ":" .. ext
    if external_dirs[key] then return external_dirs[key] end
    local var = "KORMAKE_" .. ext:upper():gsub("[^%w]", "_") .. "_DIR"
    local env = kfs.getenv(var)
    if env == "" then env = nil end
    if not env and e.env then
      env = kfs.getenv(e.env)
      if env == "" then env = nil end
      if env then var = e.env end
    end
    if not env and not e.dir then
      util.fail("external '%s' (toolchain %s): neither %s nor %s is set.\n%s", ext, fl.tc.rec.name, var, e.env,
        e.fetch or ("Set " .. var .. " to the directory that holds it."))
    end
    local dir = env and (env:gsub("\\", "/"):gsub("/$", "")) or (util.is_abs(e.dir) and e.dir or util.join(root, e.dir))
    local check = util.join(dir, e.check or (e.include and e.include[1]) or ".")
    if not (kfs.isdir(check) or kfs.isfile(check)) then
      util.fail("external '%s' (toolchain %s) is not at %s%s.\n%s", ext, fl.tc.rec.name, dir,
        env and (", where " .. var .. " points") or "", e.fetch or ("Set " .. var .. " to the directory that holds it."))
    end
    external_dirs[key] = dir
    return dir
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
      for _, ld in ipairs(d and e.libdirs or {}) do
        local spell = fl.tc.fields.flag_libdir or util.fail("toolchain '%s' does not set flag_libdir, which external '%s' needs", fl.tc.rec.name, ext)
        dirs[#dirs + 1] = spell:format(q(util.join(d, ld)))
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

  -- A module outside the roots must not stop the generation: what would (a package not
  -- fetched, a private include that is not there, a tool with no binary for the host)
  -- leaves it out, and everything built on it, with the reason kept for whoever names it.
  -- The roots' modules fail as they always have, below.
  for _, inst in ipairs(g.order) do
    if inst.in_roots then goto next_inst end
    do
      local why
      for _, d in ipairs(inst.closure) do if d.dropped then why = d.module.name .. " cannot build: " .. d.dropped end end
      for _, t in ipairs(inst.tools) do if t.instance.dropped then why = "its tool " .. t.instance.module.name .. " cannot build: " .. t.instance.dropped end end
      if not why then
        local ok, err = pcall(function()
          local fl = flags(inst)
          for _, ext in ipairs(externals_used(inst, { inst.module })) do external_dir(fl, ext) end
          for _, pi in ipairs(inst.module.fields.private_includes or {}) do
            if not kfs.isdir(util.join(inst.module.dir, pi)) then util.fail("private include '%s' is not a directory", pi) end
          end
          for _, t in ipairs(inst.tools) do
            tool_path(t.instance)
            for _, r in ipairs(t.instance.module.fields.exports or {}) do
              if r.per == "exe" and r.inputs ~= "assets" then util.fail("rule '%s' of '%s': per-exe rules take inputs = \"assets\"", r.name, t.instance.module.name) end
            end
          end
        end)
        if not ok then why = util.message(err) end
      end
      if why then
        inst.dropped = why
        if inst.selectable then g.unavailable[inst.module.name] = why end
        inst.selectable = false
      end
    end
    ::next_inst::
  end

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
  -- toolchain's test_runner, its words after the stamp (%{root}: the project's root). With
  -- test_jobs, no more than that many run at once (a device installs each as the same app).
  local runner = g.toolchain.fields.test_runner
  if runner then
    local words = {}
    for _, word in ipairs(util.as_list(runner)) do words[#words + 1] = util.ninja_escape(q((word:gsub("%%{root}", (root:gsub("%%", "%%%%")))))) end
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
  -- `platforms[<platform>]` variant over them, or else of the variant for the platform's
  -- toolchain family (the shader rule's Metal form, `platforms = { apple = ... }`). The
  -- Ninja rule's name carries the platform only when a variant applies, so a rule without
  -- one is named as it always was.
  local function rule_variant(r, platform, family)
    local v = r.platforms and (r.platforms[platform] or (family and r.platforms[family]))
    if not v then return r, "" end
    local out = {}
    for k, x in pairs(r) do if k ~= "platforms" then out[k] = x end end
    for k, x in pairs(v) do out[k] = x end
    return out, "_" .. util.symbol(platform)
  end
  for _, inst in ipairs(g.instances) do
    for _, t in ipairs(inst.dropped and {} or inst.tools) do
      local tf = t.instance.module.fields
      for _, r0 in ipairs(tf.exports or {}) do
        local r, variant = rule_variant(r0, inst.platform, flags(inst).tc.fields.family)
        local rname = "tool_" .. util.symbol(t.instance.module.name) .. "_" .. r.name .. variant
        if not tool_rules[rname] then
          local tool, implicit = tool_path(t.instance)
          local cmd = r.command or util.fail("rule '%s' of '%s' has no command", r.name, t.instance.module.name)
          -- Ninja quotes $in and $out itself when a path needs it.
          -- The program in the host's spelling: Windows' CreateProcess takes "host/bin/x.exe"
          -- as a bare name to search for, and finds "host\bin\x.exe" where it is.
          cmd = cmd:gsub("%%{tool}", (util.ninja_escape(program(util.native(tool))):gsub("%%", "%%%%"))):gsub("%%{in}", "$in")
            :gsub("%%{out}", "$out"):gsub("%%{depfile}", "$out.d"):gsub("%%{rsp}", "$out.rsp")
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
      local d = util.join(inst.module.dir, p)
      if not kfs.isdir(d) then util.fail("module '%s': private include '%s' is not a directory", inst.module.name, p) end
      dirs[#dirs + 1] = flag(d)
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
      for _, i in ipairs(d and fl.externals[ext].include or {}) do dirs[#dirs + 1] = sys:format(q(util.join(d, i))) end
    end
    return table.concat(dirs, " ")
  end

  local function define_flags(inst)
    local spell = flags(inst).tc.fields.flag_define
    local ds = {}
    for _, d in ipairs(inst.module.fields.defines or {}) do ds[#ds + 1] = spell:format(d) end
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

  local function compile(inst, src, obj, includes, defines)
    w(string.format("build %s: %s %s\n  includes = %s\n  defines = %s%s", util.ninja_escape(obj), rule(inst, "cxx"), util.ninja_escape(src),
      includes, defines, overrides(inst)))
    -- The command the toolchain's cxx rule runs, with this file's values in it. clangd takes
    -- the first entry for a file: the target's, since the host's come later.
    local fl = flags(inst)
    local vars = { cxx = program(fl.cxx), cxxflags = fl.cxxflags, defines = defines, includes = includes, ["in"] = program(src), out = program(obj) }
    local command = fl.tc.fields.rules.cxx.command:gsub("%$(%w+)", function(v) return vars[v] or "" end)
    cc[#cc + 1] = string.format("{\"directory\": %s, \"file\": %s, \"output\": %s, \"command\": %s}",
      util.json_string(build), util.json_string(src), util.json_string(obj), util.json_string(command))
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
    for _, t in ipairs(inst.tools) do
      for _, r0 in ipairs(t.instance.module.fields.exports or {}) do
        local r, variant = rule_variant(r0, inst.platform, flags(inst).tc.fields.family)
        if r.per == "exe" then goto next_rule end -- runs once per executable, below
        local rname = "tool_" .. util.symbol(t.instance.module.name) .. "_" .. r.name .. variant
        local pat_dir, pat_glob = util.dirname(r.inputs), util.basename(r.inputs)
        local dir = util.join(m.dir, pat_dir)
        if kfs.isdir(dir) then
          for _, n in ipairs(kfs.listdir(dir)) do
            if util.matches(n, pat_glob) then
              local stem = util.stem(n)
              local outname = r.outputs:gsub("%%{stem}", stem)
              local outpath = t.spec.install and (pre .. "bin/" .. t.spec.install .. "/" .. outname) or (gen .. "/" .. outname)
              local tr = tool_rules[rname]
              w(string.format("build %s: %s %s%s", util.ninja_escape(outpath), rname, util.ninja_escape(util.join(dir, n)),
                tr.implicit and (" | " .. util.ninja_escape(tr.tool)) or ""))
              runtime[#runtime + 1] = outpath
            end
          end
        end
        ::next_rule::
      end
    end
    -- Assets: bin/assets/<module>/<path>, subdirectories kept.
    if m.assets_dir then
      for _, n in ipairs(util.sorted_files_recursive(m.assets_dir)) do
        local outpath = pre .. "bin/assets/" .. m.name .. "/" .. n
        w(string.format("build %s: copy %s", util.ninja_escape(outpath), util.ninja_escape(util.join(m.assets_dir, n))))
        runtime[#runtime + 1] = outpath
      end
    end
    return runtime
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
      local objs = {}
      local unity = f.unity
      if unity == nil then unity = not opts.no_unity end
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
          compile(inst, path, obj, includes, defines)
          objs[#objs + 1] = obj
        else
          for _, s in ipairs(group) do
            local rel = s:sub(#m.dir + 2):gsub("^src/", "")
            if s:sub(1, #build) == build then rel = "gen_" .. util.basename(s) end
            local obj = pre .. "obj/" .. sym .. "/" .. rel:gsub("%." .. ext .. "$", "") .. tail .. naming.obj_suffix
            compile(inst, s, obj, includes, defines)
            objs[#objs + 1] = obj
          end
        end
      end
      -- Anything else a module's `sources` names compiles alone, as it always has.
      for _, s in ipairs(srcs) do
        if not util.contains(source_exts, util.ext(s)) then
          local rel = s:sub(#m.dir + 2):gsub("^src/", "")
          local obj = pre .. "obj/" .. sym .. "/" .. rel:gsub("%.[^%./]*$", "") .. naming.obj_suffix
          compile(inst, s, obj, includes, defines)
          objs[#objs + 1] = obj
        end
      end
      local runtime = stage_runtime(inst, gen)
      runtime_by_key[inst.key] = runtime

      if output == "lib" then
        if #objs > 0 then
          local lib = pre .. "lib/" .. naming.lib_file:format("kv_" .. sym)
          w(string.format("build %s: %s %s%s", lib, rule(inst, "ar"), table.concat(objs, " "), overrides(inst)))
          libs_by_key[inst.key] = lib
          if inst.in_roots then all[#all + 1] = lib end
        end
        local outs = { libs_by_key[inst.key] }
        for _, r in ipairs(runtime) do outs[#outs + 1] = r end
        outputs_by_key[inst.key] = outs
      elseif output == "exe" then
        local libs, mods, rt = {}, {}, {}
        for i = #inst.closure, 1, -1 do
          local d = inst.closure[i]
          if libs_by_key[d.key] then libs[#libs + 1] = libs_by_key[d.key] end
          mods[#mods + 1] = d.module
          for _, r in ipairs(runtime_by_key[d.key] or {}) do rt[#rt + 1] = r end
        end
        mods[#mods + 1] = m
        local extlibs, extdirs = extlibs_of(inst, mods)
        inst.extlibs = extlibs
        for _, r in ipairs(runtime) do rt[#rt + 1] = r end
        for _, r in ipairs(runtime_of(inst, mods)) do rt[#rt + 1] = r end
        local exe = pre .. "bin/" .. exe_name(inst) .. naming.exe_suffix
        w(string.format("build %s: %s %s %s%s\n  libs = %s%s", exe, rule(inst, "link"), table.concat(objs, " "), table.concat(libs, " "),
          #rt > 0 and (" || " .. table.concat(rt, " ")) or "", util.trim(table.concat(extlibs, " ") .. " " .. table.concat(extdirs, " ")), overrides(inst)))
        if inst.in_roots then all[#all + 1] = exe end
        local outs = { exe }
        outputs_by_key[inst.key] = outs
        -- Rules that run once per executable over its staged assets (a pack): part of the
        -- default build in ship, buildable by name in the other configs.
        for _, t in ipairs(inst.tools) do
          for _, r0 in ipairs(t.instance.module.fields.exports or {}) do
            local r, variant = rule_variant(r0, inst.platform, flags(inst).tc.fields.family)
            if r.per ~= "exe" then goto skip_rule end
            do
              local rname = "tool_" .. util.symbol(t.instance.module.name) .. "_" .. r.name .. variant
              local tr = tool_rules[rname]
              local ins = {}
              local staged = pre .. "bin/assets/"
              for _, p in ipairs(rt) do if p:sub(1, #staged) == staged then ins[#ins + 1] = util.ninja_escape(p) end end
              if r.inputs ~= "assets" then util.fail("rule '%s' of '%s': per-exe rules take inputs = \"assets\"", r.name, t.instance.module.name) end
              local outp = pre .. "bin/" .. r.outputs:gsub("%%{exe}", exe_name(inst))
              local implicit = tr.implicit and (" | " .. util.ninja_escape(tr.tool)) or ""
              w(string.format("build %s: %s %s%s", util.ninja_escape(outp), rname, table.concat(ins, " "), implicit))
              if g.config == "ship" then
                if inst.in_roots then all[#all + 1] = outp end
                outs[#outs + 1] = outp
              end
            end
            ::skip_rule::
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
      local includes = include_flags(inst, { m.tests_dir })
      local objs = {}
      for _, s in ipairs(srcs) do
        local obj = pre .. "obj/" .. sym .. "/tests/" .. util.basename(s):gsub("%.cc$", "") .. flags(inst).tc.fields.obj_suffix
        compile(inst, s, obj, includes, define_flags(inst))
        objs[#objs + 1] = obj
      end
      -- Order-only inputs, as an executable's: the staged assets and shaders of the module
      -- and its closure (a test reads them beside itself), and a package's runtime files.
      local libs, mods, rt = {}, {}, {}
      if libs_by_key[inst.key] then libs[#libs + 1] = libs_by_key[inst.key] end
      for i = #inst.closure, 1, -1 do
        local d = inst.closure[i]
        if libs_by_key[d.key] then libs[#libs + 1] = libs_by_key[d.key] end
        mods[#mods + 1] = d.module
        for _, r in ipairs(runtime_by_key[d.key] or {}) do rt[#rt + 1] = r end
      end
      mods[#mods + 1] = m
      for _, r in ipairs(runtime_by_key[inst.key] or {}) do rt[#rt + 1] = r end
      local extlibs, extdirs = extlibs_of(inst, mods)
      for _, r in ipairs(runtime_of(inst, mods)) do rt[#rt + 1] = r end
      local exe = pre .. "bin/" .. sym .. "_tests" .. flags(inst).tc.fields.exe_suffix
      w(string.format("build %s: %s %s %s%s\n  libs = %s%s", exe, rule(inst, "link"), table.concat(objs, " "), table.concat(libs, " "),
        #rt > 0 and (" || " .. table.concat(rt, " ")) or "", util.trim(table.concat(extlibs, " ") .. " " .. table.concat(extdirs, " ")), overrides(inst)))
      w(string.format("build %stest/%s.ok: %s %s", pre, sym, (pre ~= "" and runner ~= "") and "run_test_host" or "run_test", exe))
      if inst.tests then
        tests[#tests + 1] = exe
        checks[#checks + 1] = pre .. "test/" .. sym .. ".ok"
      end
      inst.test_stamp = pre .. "test/" .. sym .. ".ok"
    end
    ::skip::
  end

  w("")
  w("build tests: phony " .. table.concat(tests, " "))
  w("build check: phony " .. table.concat(checks, " "))
  w("build all: phony " .. table.concat(all, " ") .. " tests")
  w("default all")
  -- A phony per selectable module, named for it: what `kormake NAME` asks Ninja for.
  w("")
  for _, inst in ipairs(g.order) do
    if inst.selectable then
      local outs = {}
      for _, o in ipairs(outputs_by_key[inst.key] or {}) do outs[#outs + 1] = util.ninja_escape(o) end
      w(string.format("build %s: phony%s", util.ninja_escape(inst.module.name), #outs > 0 and (" " .. table.concat(outs, " ")) or ""))
    end
  end
  -- Regenerate when any input changes.
  local inputs = { util.join(root, "kormake.lua") }
  if c.regen_config then inputs[#inputs + 1] = c.regen_config end
  -- Every module file collected, built here or not: an edit may let a left-out one in.
  for _, rec in ipairs(c.modules) do inputs[#inputs + 1] = rec.file end
  local esc = {}
  for _, i in ipairs(inputs) do esc[#esc + 1] = util.ninja_escape(i) end
  w("build build.ninja: regen | " .. table.concat(esc, " "))

  kfs.mkdirp(util.join(build, "test"))
  kfs.mkdirp(util.join(build, "host/test"))
  kfs.write(util.join(build, "build.ninja"), table.concat(out, "\n") .. "\n", true)
  kfs.write(util.join(build, "compile_commands.json"), "[\n" .. table.concat(cc, ",\n") .. "\n]\n")
  -- Where clangd looks.
  kfs.write(util.join(c.build_root, "compile_commands.json"), "[\n" .. table.concat(cc, ",\n") .. "\n]\n")
  return build
end

return M
