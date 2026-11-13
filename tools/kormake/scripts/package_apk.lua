-- kormake: what makes a package an Android application, for a toolchain with package =
-- "apk". The package command has laid out dist/<exe>/apk/: the library in lib/<abi>/ and
-- what it reads (the pack, shaders/, loose assets, the licenses) in assets/. This writes
-- dist/<exe>/<exe>.apk from it with the SDK's build tools and nothing else (no Gradle):
--   - AndroidManifest.xml: the package org.korvid.<exe>, the oldest and the targeted API
--     (the toolchain's os_min and target_api), one NativeActivity that loads lib<exe>.so
--     and has no Java (hasCode false), handling its own turns and resizes (configChanges),
--     full screen; debuggable unless the config is ship;
--   - aapt package: the manifest compiled against the platform's android.jar, and the staged
--     tree with it, every entry stored (-0 ""): the engine reads an asset in place, and the
--     loader maps the library from the package (extractNativeLibs false). aapt and not
--     aapt2, which links resources and has no way to take a lib/ directory;
--   - zipalign, with 16 KiB pages for the library;
--   - a signature: the debug key (~/.android/debug.keystore, made with keytool the first
--     time, the one Android's own tools use), or the keystore KORMAKE_ANDROID_KEYSTORE
--     names with KORMAKE_ANDROID_KEY_ALIAS and KORMAKE_ANDROID_KEYSTORE_PASS.
-- One ABI a package: the toolchain's abi.
local util = kormake.load("util")
local M = {}

local function env(name)
  local v = kfs.getenv(name)
  return v ~= nil and v ~= "" and v or nil
end

local function xml(s) return (s:gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub('"', "&quot;")) end

-- A Java package segment from an executable's name.
local function segment(name)
  local s = name:gsub("[^%w_]", "_")
  if s:match("^%d") then s = "_" .. s end
  return s:lower()
end

function M.package_id(exe) return "org.korvid." .. segment(exe) end

-- build: the build directory (for the log); dist: dist/<exe>/; stage: dist/<exe>/apk/;
-- lib: the library's file name. Returns the .apk's path and who signed it.
function M.run(build, dist, stage, exe, lib, tc, config)
  local q = util.shell_quote
  local f = tc.fields
  local log = util.join(build, "package.log")
  local function run(what, command, dir)
    local rc = kfs.exec((dir and ("cd " .. q(dir) .. " && ") or "") .. command .. " >> " .. q(log) .. " 2>&1")
    if rc ~= 0 then util.fail("package: %s failed (%s; see %s)", what, command, log) end
  end

  local sdk = kormake.load("probe_android").sdk() or util.fail("package: no Android SDK (ANDROID_HOME, or ~/Android/Sdk)")
  local want = f.build_tools or util.fail("toolchain '%s' pins no build tools (build_tools = \"36.0.0\")", tc.name)
  local tools = util.join(sdk, "build-tools/" .. want)
  if not kfs.isdir(tools) then
    util.fail("package: no build tools %s under %s. Install them with\n  sdkmanager \"build-tools;%s\"", want, sdk, want)
  end
  local api = f.target_api or util.fail("toolchain '%s' names no target_api", tc.name)
  local jar = util.join(sdk, "platforms/android-" .. api .. "/android.jar")
  if not kfs.isfile(jar) then
    util.fail("package: no platform %s under %s. Install it with\n  sdkmanager \"platforms;android-%s\"", api, sdk, api)
  end
  local function tool(name) return q(util.join(tools, name)) end

  local id = M.package_id(exe)
  local libname = lib:match("^lib(.*)%.so$") or util.fail("package: '%s' is not a lib<name>.so", lib)
  local manifest = {
    '<?xml version="1.0" encoding="utf-8"?>',
    '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="' .. xml(id) .. '"',
    '          android:versionCode="1" android:versionName="1.0">',
    '  <uses-sdk android:minSdkVersion="' .. xml(f.os_min or "33") .. '" android:targetSdkVersion="' .. xml(api) .. '"/>',
    '  <application android:label="' .. xml(exe) .. '" android:hasCode="false" android:extractNativeLibs="false"',
    '               android:appCategory="game" android:isGame="true">',
    '    <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"',
    '              android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|density|keyboard|keyboardHidden|navigation|uiMode|touchscreen"',
    '              android:theme="@android:style/Theme.NoTitleBar.Fullscreen">',
    '      <meta-data android:name="android.app.lib_name" android:value="' .. xml(libname) .. '"/>',
    '      <intent-filter>',
    '        <action android:name="android.intent.action.MAIN"/>',
    '        <category android:name="android.intent.category.LAUNCHER"/>',
    '      </intent-filter>',
    '    </activity>',
    '  </application>',
    '</manifest>',
    '',
  }
  local work = util.join(build, "package-apk")
  kfs.mkdirp(work)
  local manifest_path = util.join(work, "AndroidManifest.xml")
  kfs.write(manifest_path, table.concat(manifest, "\n"), true)

  local unaligned, apk = util.join(work, exe .. ".unaligned.apk"), util.join(dist, exe .. ".apk")
  if not kfs.isfile(util.join(stage, "lib/" .. f.abi .. "/" .. lib)) then util.fail("package: %s is not staged under lib/%s/", lib, f.abi) end
  run("aapt package", tool("aapt") .. " package -f -M " .. q(manifest_path) .. " -I " .. q(jar) .. ' -0 ""' ..
      (config ~= "ship" and " --debug-mode" or "") .. " -F " .. q(unaligned) .. " " .. q(stage))
  run("zipalign", tool("zipalign") .. " -f -P 16 4 " .. q(unaligned) .. " " .. q(apk))

  local ks, alias, pass = env("KORMAKE_ANDROID_KEYSTORE"), env("KORMAKE_ANDROID_KEY_ALIAS"), env("KORMAKE_ANDROID_KEYSTORE_PASS")
  local signed = "with " .. (ks or "the debug key")
  if not ks then
    local home = env("HOME") or env("USERPROFILE") or util.fail("package: no home directory for the debug keystore")
    ks, alias, pass = util.join(home, ".android/debug.keystore"), "androiddebugkey", "android"
    if not kfs.isfile(ks) then
      kfs.mkdirp(util.join(home, ".android"))
      run("making the debug keystore", "keytool -genkeypair -keystore " .. q(ks) .. " -storepass android -keypass android -alias androiddebugkey" ..
          " -keyalg RSA -keysize 2048 -validity 10000 -dname " .. q("CN=Android Debug,O=Android,C=US"))
    end
  end
  if not pass then util.fail("package: KORMAKE_ANDROID_KEYSTORE is set; KORMAKE_ANDROID_KEYSTORE_PASS must be too") end
  run("apksigner", tool("apksigner") .. " sign --ks " .. q(ks) .. " --ks-pass " .. q("pass:" .. pass) .. " --v4-signing-enabled false" ..
      (alias and (" --ks-key-alias " .. q(alias)) or "") .. " " .. q(apk))
  kfs.rmtree(stage)
  return apk, signed, id
end

return M
