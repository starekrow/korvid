-- kormake: an iOS device's two needs, signing and running a test, with what macOS ships
-- (security, plutil, codesign, xcrun devicectl) and nothing else.
--
-- Signing (M.sign; package_app.lua's for an `ios` app, and each test M.test runs): the
-- development identity (KORMAKE_CODESIGN_IDENTITY, else the first "Apple Development" one
-- `security find-identity` lists), the profile (KORMAKE_IOS_PROFILE, else the newest by
-- its CreationDate of Xcode's that covers the bundle's identifier, has not expired, and
-- names the device when one is given), embedded as embedded.mobileprovision, and
-- entitlements made from it: the application identifier, the team, get-task-allow.
-- `security cms -D` turns a profile into a property list; `plutil -extract` reads one
-- field of it (or of devicectl's JSON) at a time, so no parser is needed here.
--
-- A test on a device (M.test; `kormake _ios_test EXE`, the ios toolchain's test_runner): a
-- device runs signed applications only, so the executable becomes one, with what the tests
-- read beside them (assets/, shaders/), installed as KORMAKE_IOS_TEST_BUNDLE_ID (one the
-- profile covers; every test is the same app, so they run one at a time: test_jobs = 1) on
-- the one physical device connected, or KORMAKE_IOS_DEVICE (a name or an identifier), and
-- run to its end with its output here. DEVICECTL_CHILD_<NAME> variables reach the test as
-- <NAME> (DEVICECTL_CHILD_SDL_AUDIO_DRIVER=dummy: a silent run).
local util = kormake.load("util")
local M = {}

local q = util.shell_quote

-- Runs a command, its output (stdout and stderr) to `out`; the status.
local function exec_to(out, command)
  return kfs.exec(command .. " > " .. q(out) .. " 2>&1")
end

-- One field of a property list or JSON file, as plutil prints it raw (an array: its
-- length), or nil.
local function field(file, key)
  local out = file .. ".field"
  if exec_to(out, "plutil -extract " .. q(key) .. " raw -o - " .. q(file)) ~= 0 then return nil end
  local v = util.trim(kfs.read(out) or "")
  return v ~= "" and v or nil
end

local function first_identity(work)
  local out = util.join(work, "identities.txt")
  if exec_to(out, "security find-identity -v -p codesigning") ~= 0 then return nil end
  for line in (kfs.read(out) or ""):gmatch("[^\n]+") do
    local hash = line:match('^%s*%d+%)%s+(%x+)%s+"Apple Development:')
    if hash then return hash end
  end
  return nil
end

-- The profile to sign `bundle_id` with (for `device`, when given): its path, its team.
local function find_profile(work, bundle_id, device)
  local now = os.date("!%Y-%m-%dT%H:%M:%SZ")
  local home = kfs.getenv("HOME") or ""
  local best, best_team, best_created
  local dirs = { util.join(home, "Library/Developer/Xcode/UserData/Provisioning Profiles"),
                 util.join(home, "Library/MobileDevice/Provisioning Profiles") }
  local only = kfs.getenv("KORMAKE_IOS_PROFILE")
  local files = {}
  if only and only ~= "" then files[1] = only else
    for _, dir in ipairs(dirs) do
      if kfs.isdir(dir) then
        for _, name in ipairs(kfs.listdir(dir) or {}) do
          if name:match("%.mobileprovision$") then files[#files + 1] = util.join(dir, name) end
        end
      end
    end
  end
  local plist = util.join(work, "profile.plist")
  for _, f in ipairs(files) do
    if kfs.exec("security cms -D -i " .. q(f) .. " > " .. q(plist) .. " 2>/dev/null") == 0 then
      local app = field(plist, "Entitlements.application-identifier") or ""
      local team = field(plist, "TeamIdentifier.0") or ""
      local expires = field(plist, "ExpirationDate") or ""
      local created = field(plist, "CreationDate") or ""
      local covers = app == team .. "." .. bundle_id or app == team .. ".*"
      local on_device = true
      if device then
        local list = util.join(work, "devices.json")
        on_device = exec_to(list, "plutil -extract ProvisionedDevices json -o - " .. q(plist)) == 0 and
                    (kfs.read(list) or ""):find('"' .. device .. '"', 1, true) ~= nil
      end
      if covers and on_device and expires > now and (not best or created > best_created) then
        best, best_team, best_created = f, team, created
      end
    end
  end
  return best, best_team
end

-- Signs `app` (its Info.plist names `bundle_id`) for development; `device`, when given, is
-- one the profile must name. Scratch files go in `work`. Nil, or what went wrong.
function M.sign(work, app, bundle_id, device)
  kfs.mkdirp(work)
  local identity = kfs.getenv("KORMAKE_CODESIGN_IDENTITY")
  if not identity or identity == "" then identity = first_identity(work) end
  if not identity then return "no Apple Development signing identity (sign into Xcode with your account)" end
  local profile, team = find_profile(work, bundle_id, device)
  if not profile then
    return string.format("no development profile covers %s%s (build an app with that identifier to a device in Xcode once)",
                         bundle_id, device and (" on " .. device) or "")
  end
  kfs.copy(profile, util.join(app, "embedded.mobileprovision"))
  local ent = util.join(work, "entitlements.plist")
  kfs.write(ent, table.concat({
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
    '<plist version="1.0"><dict>',
    '  <key>application-identifier</key><string>' .. team .. "." .. bundle_id .. '</string>',
    '  <key>com.apple.developer.team-identifier</key><string>' .. team .. '</string>',
    '  <key>get-task-allow</key><true/>',
    '</dict></plist>', ''}, "\n"))
  local log = util.join(work, "codesign.log")
  if exec_to(log, "codesign --force --sign " .. q(identity) .. " --entitlements " .. q(ent) .. " " .. q(app)) ~= 0 or
     exec_to(log, "codesign --verify --strict " .. q(app)) ~= 0 then
    return "codesign failed: " .. util.trim(kfs.read(log) or "")
  end
  return nil
end

-- The physical device to run on: KORMAKE_IOS_DEVICE's, or the only one there is.
local function find_device(work)
  local json = util.join(work, "devices.json")
  if kfs.exec("xcrun devicectl list devices --json-output " .. q(json) .. " > /dev/null 2>&1") ~= 0 then
    return nil, "devicectl cannot list devices"
  end
  local want = kfs.getenv("KORMAKE_IOS_DEVICE")
  if want == "" then want = nil end
  local found = {}
  for i = 0, (tonumber(field(json, "result.devices")) or 0) - 1 do
    local d = "result.devices." .. i .. "."
    local udid = field(json, d .. "hardwareProperties.udid")
    if field(json, d .. "hardwareProperties.reality") == "physical" and udid and
       field(json, d .. "connectionProperties.tunnelState") ~= "unavailable" and
       (not want or want == udid or want == field(json, d .. "identifier") or want == field(json, d .. "deviceProperties.name")) then
      found[#found + 1] = udid
    end
  end
  if #found == 1 then return found[1] end
  return nil, #found == 0 and ("no connected device" .. (want and (" named " .. want) or "")) or "more than one device; set KORMAKE_IOS_DEVICE"
end

local function info_plist(name, bundle_id)
  return table.concat({
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">',
    '<plist version="1.0"><dict>',
    '  <key>CFBundleExecutable</key><string>' .. name .. '</string>',
    '  <key>CFBundleIdentifier</key><string>' .. bundle_id .. '</string>',
    '  <key>CFBundleName</key><string>' .. name .. '</string>',
    '  <key>CFBundlePackageType</key><string>APPL</string>',
    '  <key>CFBundleShortVersionString</key><string>1.0</string>',
    '  <key>CFBundleVersion</key><string>1</string>',
    '  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>',
    '  <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>',
    '  <key>LSRequiresIPhoneOS</key><true/>',
    '  <key>MinimumOSVersion</key><string>26.0</string>',
    '  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>',
    '</dict></plist>', ''}, "\n")
end

-- Runs the test executable `exe` (an absolute path) on the device; its exit status, with
-- its output printed here. A pass is exit code 0 and the suite's own summary line
-- together: devicectl has reported a crash as exit code 0.
function M.test(exe)
  local function fail(msg) io.stderr:write("ios test: ", msg, "\n"); return 2 end
  local bundle_id = kfs.getenv("KORMAKE_IOS_TEST_BUNDLE_ID")
  if not bundle_id or bundle_id == "" then return fail("set KORMAKE_IOS_TEST_BUNDLE_ID to an application identifier your development profile covers") end
  local name, bin = util.basename(exe), util.dirname(exe)
  local work = util.join(util.dirname(bin), "device/" .. name)
  kfs.rmtree(work)
  kfs.mkdirp(work)
  local device, why = find_device(work)
  if not device then return fail(why) end

  local app = util.join(work, name .. ".app")
  kfs.mkdirp(app)
  kfs.copy(exe, util.join(app, name))
  kfs.exec("chmod +x " .. q(util.join(app, name)))
  for _, d in ipairs({ "assets", "shaders" }) do
    if kfs.isdir(util.join(bin, d)) then util.copy_tree(util.join(bin, d), util.join(app, d)) end
  end
  kfs.write(util.join(app, "Info.plist"), info_plist(name, bundle_id))
  local err = M.sign(work, app, bundle_id, device)
  if err then return fail(err) end

  local log = util.join(work, "install.log")
  if exec_to(log, "xcrun devicectl device install app --device " .. q(device) .. " " .. q(app)) ~= 0 then
    io.stderr:write(kfs.read(log) or "")
    return fail("install failed")
  end
  -- The test's output as it comes, devicectl's own lines aside; all of it kept to read the
  -- exit code from.
  local run = util.join(work, "run.log")
  kfs.exec("xcrun devicectl device process launch --device " .. q(device) .. " --console --terminate-existing " .. q(bundle_id) ..
           " 2>&1 | tee " .. q(run) .. " | grep -v -E '^(Launched application with|Waiting for the application to terminate|The app terminated with)'")
  local text = kfs.read(run) or ""
  local code
  for c in text:gmatch("The app terminated with the exit code (%-?%d+)") do code = tonumber(c) end
  if not code and text:find("could not be, unlocked", 1, true) then
    return fail("the device is locked: unlock it (and keep it awake while the tests run), then run them again")
  end
  if not code then
    io.stderr:write(text:sub(-600))
    return fail(name .. " did not report an exit code (it crashed, or the device went away)")
  end
  if code == 0 and not text:find("test: " .. name .. ": %d+ checks passed") then
    return fail(name .. " exited 0 without its summary (it crashed, or its output was cut off)")
  end
  kfs.rmtree(work)
  return code
end

return M
