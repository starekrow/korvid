-- kormake: package. A self-contained directory for one executable: the executable, its
-- cooked assets, its shaders, the licenses of everything it links. dist/<exe>/ under the build
-- base, replaced each time; on a toolchain that packages applications, dist/<exe>/<exe>.app
-- (package_app.lua). Runs after emit, which names the executable's packs.
local util = kormake.load("util")
local M = {}

-- inst: the executable's instance; ninja: the command that runs Ninja. Returns the exit code.
function M.run(c, build, inst, ninja, target)
  local root = c.root_dir
  local exe = inst.module.fields.exe or inst.module.name
  if inst.module.fields.output ~= "exe" or inst.prefix ~= "" then util.fail("'%s' is not an executable of %s", inst.module.name, target) end
  -- The file emit linked: the program, or on Android an app's lib<exe>.so.
  local exe_bin = inst.exe_file or (exe .. inst.toolchain.fields.exe_suffix)
  -- Its packs: what its per-exe rules write, as emit named them.
  local packs = inst.per_exe_outputs or {}
  local q = util.shell_quote
  -- Packs are not part of a package for now: goldberg cooks loose, and the loose tree is
  -- what ships. A pack is still built by name (kormake bin/<exe>.kpak).
  packs = {}
  local want = { q("bin/" .. exe_bin) }
  local rc = kfs.exec(string.format("%s -C %s %s", ninja, q(build), table.concat(want, " ")))
  if rc ~= 0 then return rc end
  local dist = util.join(c.base_dir, "dist/" .. exe)
  -- The tree goldberg cooked for this executable (emit named it). A ship game that
  -- packages as a plain directory was cooked straight into dist/<exe>/assets/: that stays,
  -- and everything else in dist/<exe>/ is replaced. Anywhere else, the tree is copied in.
  local tree = inst.assets_tree
  local in_place = tree ~= nil and tree == util.join(dist, "assets")
  if in_place then
    for _, n in ipairs(kfs.isdir(dist) and kfs.listdir(dist) or {}) do
      if n ~= "assets" then kfs.rmtree(util.join(dist, n)) end
    end
    if kfs.isdir(util.join(tree, ".goldberg.attic")) then
      print("kormake: package: " .. util.join(tree, ".goldberg.attic") .. " would ship with the game: `goldberg clean attic --out " .. tree .. "` empties it")
    end
  elseif not kfs.rmtree(dist) and kfs.isdir(dist) then util.fail("package: cannot remove %s", dist) end
  -- Where the executable goes and where what it reads goes: one directory, or for a
  -- toolchain that packages applications, <exe>.app: a macOS one's (package = "app")
  -- Contents/MacOS/ and Contents/Resources/, an iOS one's (package = "iosapp") top, where
  -- both go (package_app.lua does the rest).
  local kind = inst.toolchain.fields.package
  local app = (kind == "app" or kind == "iosapp") and util.join(dist, exe .. ".app") or nil
  local exe_dir = kind == "app" and util.join(app, "Contents/MacOS") or app or dist
  local res = kind == "app" and util.join(app, "Contents/Resources") or app or dist
  -- An Android package (package = "apk") is staged as it is laid out inside: the library
  -- under lib/<abi>/, what it reads under assets/ (package_apk.lua makes the .apk of it).
  local stage = kind == "apk" and util.join(dist, "apk") or nil
  if stage then
    if not inst.is_app then util.fail("package: '%s' does not link the window, so on Android it is a program for adb's shell, not an app", exe) end
    exe_dir = util.join(stage, "lib/" .. (inst.toolchain.fields.abi or util.fail("toolchain '%s' names no abi", inst.toolchain.name)))
    res = util.join(stage, "assets")
  end
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
  -- What it reads is the loose assets: its modules' folders of the cooked tree, into
  -- assets/ (or already there, for a tree cooked in place). Module by module, so nothing
  -- of goldberg's own in the tree (its marker, its attic) is packaged.
  local loose = 0
  if tree then
    for _, d in ipairs(mods) do
      local from = util.join(tree, d.module.name)
      if kfs.isdir(from) then
        if not in_place then util.copy_tree(from, util.join(res, "assets/" .. d.module.name)) end
        loose = loose + 1
      end
    end
    -- A plain directory's assets/ is where a ship cook of this game writes. Copied in from
    -- another config's tree, it carries goldberg's marker with it, so that cook will take
    -- the directory as its own (and cook over it) rather than refuse it as a stranger's.
    if not in_place and not kind and loose > 0 and kfs.isfile(util.join(tree, ".goldberg")) then
      kfs.copy(util.join(tree, ".goldberg"), util.join(res, "assets/.goldberg"))
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
  local apk = nil
  if stage then
    -- A game that draws links the module that provides "render".
    local draws, java_dir = false, nil
    for _, d in ipairs(inst.closure) do
      if d.module.fields.provides == "render" then draws = true end
      -- The window module's Java (its activity), if it has any.
      if d.module.name == inst.toolchain.fields.app_module then java_dir = util.join(d.module.dir, "src/android/java") end
    end
    local path, signed, id, activity = kormake.load("package_apk").run(build, dist, stage, exe, exe_bin, inst.toolchain, target:match("%-([^%-]+)$"), draws, java_dir)
    apk, bundle = path, string.format(", %s/%s, signed %s", id, activity, signed)
  end
  print(string.format("kormake: %s: %s%s, %d license files%s%s", apk or app or dist, exe,
    #packs > 0 and (", " .. table.concat(pack_names, ", ")) or (loose > 0 and string.format(", loose assets of %d module%s", loose, loose == 1 and "" or "s") or ", no pack"), copied,
    bundle, kfs.isfile(util.join(root, "LICENSE")) and "" or " (the repository has no LICENSE at its root)"))
  return 0
end

return M
