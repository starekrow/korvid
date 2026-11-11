-- kormake: what makes a package an Apple application bundle, for a toolchain with package =
-- "app" (macOS) or "iosapp" (iOS and its Simulator). For "app" the package command has put
-- the executable in Contents/MacOS/ and what it reads (the pack, shaders/, the licenses) in
-- Contents/Resources/, where the platform's base path finds them; for "iosapp" both at the
-- bundle's top, as iOS lays a bundle out. This adds the rest. For "app":
--   - every dynamic library the executable links that is not the system's (otool -L), copied
--     into Contents/Frameworks/, its install name @rpath/<name>, the executable's reference to
--     it rewritten and an rpath of @executable_path/../Frameworks added, so the bundle runs
--     on a machine without the library where the build found it (Homebrew's SDL3);
--   - Contents/Info.plist: the bundle's identifier, name, executable, version, the minimum
--     system (the toolchain's os_min), high resolution;
--   - a signature: ad-hoc unless KORMAKE_CODESIGN_IDENTITY names one (a Developer ID, for a
--     notarized release; notarizing is not kormake's). arm64 code does not run unsigned, and
--     rewriting install names breaks the linker's signature, so signing comes last.
local util = kormake.load("util")
local M = {}

local function run(build, what, command)
  local log = util.join(build, "package.log")
  local rc = kfs.exec(command .. " >> " .. util.shell_quote(log) .. " 2>&1")
  if rc ~= 0 then util.fail("package: %s failed (%s; see %s)", what, command, log) end
end

-- The non-system libraries a Mach-O file links, as otool -L names them (absolute paths;
-- the file's own install name excluded).
local function linked(build, file)
  local out = util.join(build, "package.otool")
  if kfs.exec("otool -L " .. util.shell_quote(file) .. " > " .. util.shell_quote(out) .. " 2>&1") ~= 0 then
    util.fail("package: otool -L %s failed", file)
  end
  local list, first = {}, true
  for line in (kfs.read(out) or ""):gmatch("[^\n]+") do
    if first then first = false -- "<file>:"
    else
      local path = line:match("^%s+(%S+)")
      if path and not path:match("^/System/") and not path:match("^/usr/lib/") and not path:match("^@") then list[#list + 1] = path end
    end
  end
  return list
end

local function plist_string(s) return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;")) end

-- An iOS bundle: flat, its Info.plist the iOS keys, one scene (platform_hid's delegate makes
-- it), a launch screen (without one iOS shows the app letterboxed, in a compatibility mode),
-- full screen. Nothing to bundle: the engine links no library that iOS does not have. Signed
-- ad-hoc, which the Simulator runs; a device wants a provisioning profile (slice 8).
local function ios_app(build, app, exe_name, tc)
  local q = util.shell_quote
  local id = "org.korvid." .. exe_name:gsub("[^%w%-%.]", "-")
  local sim = tc.fields.sdk == "iphonesimulator"
  local plist = {
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
    '<plist version="1.0">',
    '<dict>',
    '  <key>CFBundleDevelopmentRegion</key><string>en</string>',
    '  <key>CFBundleExecutable</key><string>' .. plist_string(exe_name) .. '</string>',
    '  <key>CFBundleIdentifier</key><string>' .. plist_string(id) .. '</string>',
    '  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>',
    '  <key>CFBundleName</key><string>' .. plist_string(exe_name) .. '</string>',
    '  <key>CFBundlePackageType</key><string>APPL</string>',
    '  <key>CFBundleShortVersionString</key><string>1.0</string>',
    '  <key>CFBundleVersion</key><string>1</string>',
    '  <key>CFBundleSupportedPlatforms</key><array><string>' .. (sim and "iPhoneSimulator" or "iPhoneOS") .. '</string></array>',
    '  <key>LSRequiresIPhoneOS</key><true/>',
    '  <key>MinimumOSVersion</key><string>' .. plist_string(tc.fields.os_min or "") .. '</string>',
    '  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>',
    '  <key>UIRequiredDeviceCapabilities</key><array><string>arm64</string><string>metal</string></array>',
    '  <key>UILaunchScreen</key><dict/>',
    '  <key>UIStatusBarHidden</key><true/>',
    '  <key>UIRequiresFullScreen</key><true/>',
    '  <key>UIApplicationSceneManifest</key><dict>',
    '    <key>UIApplicationSupportsMultipleScenes</key><false/>',
    '  </dict>',
    '  <key>UISupportedInterfaceOrientations</key><array>',
    '    <string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string>',
    '    <string>UIInterfaceOrientationPortrait</string>',
    '  </array>',
    '</dict>',
    '</plist>',
  }
  kfs.write(util.join(app, "Info.plist"), table.concat(plist, "\n") .. "\n")
  local identity = kfs.getenv("KORMAKE_CODESIGN_IDENTITY")
  if not identity or identity == "" then identity = "-" end
  run(build, "codesign", "codesign --force --sign " .. q(identity) .. " " .. q(app))
  run(build, "codesign --verify", "codesign --verify --strict " .. q(app))
  return 0, identity == "-" and "ad-hoc" or identity
end

function M.run(build, app, exe_name, tc)
  local q = util.shell_quote
  if tc.fields.package == "iosapp" then
    kfs.write(util.join(build, "package.log"), "")
    return ios_app(build, app, exe_name, tc)
  end
  local contents = util.join(app, "Contents")
  local exe = util.join(contents, "MacOS/" .. exe_name)
  local frameworks = util.join(contents, "Frameworks")
  kfs.write(util.join(build, "package.log"), "")

  -- The libraries, and theirs, breadth first; each once.
  local queue, seen, bundled = { exe }, {}, 0
  while #queue > 0 do
    local file = table.remove(queue, 1)
    for _, lib in ipairs(linked(build, file)) do
      local name = util.basename(lib)
      local inside = util.join(frameworks, name)
      if not seen[name] then
        seen[name] = true
        kfs.mkdirp(frameworks)
        kfs.copy(lib, inside)
        run(build, "chmod", "chmod u+w " .. q(inside)) -- Homebrew installs them read-only
        run(build, "install_name_tool -id", "install_name_tool -id " .. q("@rpath/" .. name) .. " " .. q(inside))
        queue[#queue + 1] = inside
        bundled = bundled + 1
      end
      if file ~= inside then
        run(build, "install_name_tool -change", "install_name_tool -change " .. q(lib) .. " " .. q("@rpath/" .. name) .. " " .. q(file))
      end
    end
  end
  if bundled > 0 then
    run(build, "install_name_tool -add_rpath", "install_name_tool -add_rpath @executable_path/../Frameworks " .. q(exe))
  end

  -- Identifiers allow letters, digits, '-' and '.'.
  local id = "org.korvid." .. exe_name:gsub("[^%w%-%.]", "-")
  local plist = {
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
    '<plist version="1.0">',
    '<dict>',
    '  <key>CFBundleDevelopmentRegion</key><string>en</string>',
    '  <key>CFBundleExecutable</key><string>' .. plist_string(exe_name) .. '</string>',
    '  <key>CFBundleIdentifier</key><string>' .. plist_string(id) .. '</string>',
    '  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>',
    '  <key>CFBundleName</key><string>' .. plist_string(exe_name) .. '</string>',
    '  <key>CFBundlePackageType</key><string>APPL</string>',
    '  <key>CFBundleShortVersionString</key><string>1.0</string>',
    '  <key>CFBundleVersion</key><string>1</string>',
    '  <key>LSMinimumSystemVersion</key><string>' .. plist_string(tc.fields.os_min or "") .. '</string>',
    '  <key>NSHighResolutionCapable</key><true/>',
    '  <key>NSPrincipalClass</key><string>NSApplication</string>',
    '</dict>',
    '</plist>',
  }
  kfs.write(util.join(contents, "Info.plist"), table.concat(plist, "\n") .. "\n")

  -- Libraries first, then the bundle, which seals them.
  local identity = kfs.getenv("KORMAKE_CODESIGN_IDENTITY")
  if not identity or identity == "" then identity = "-" end
  for name in pairs(seen) do
    run(build, "codesign " .. name, "codesign --force --sign " .. q(identity) .. " " .. q(util.join(frameworks, name)))
  end
  run(build, "codesign", "codesign --force --sign " .. q(identity) .. " " .. q(app))
  run(build, "codesign --verify", "codesign --verify --strict " .. q(app))
  return bundled, identity == "-" and "ad-hoc" or identity
end

return M
