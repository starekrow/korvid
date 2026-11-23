-- kormake: the Android probe, for a toolchain with probe = "android". Finds the NDK and
-- returns its clang++ and llvm-ar by absolute path with --target=<arch>-<triple> (the
-- toolchain's `arch` and `triple`: "aarch64" and "linux-android33"), so the build works
-- from any shell, regenerations included, whatever PATH holds. The NDK is, in order:
-- ANDROID_NDK_HOME; the version the toolchain's `ndk` pins under <sdk>/ndk/, where <sdk> is
-- ANDROID_HOME, ANDROID_SDK_ROOT or ~/Android/Sdk (Windows: %LOCALAPPDATA%\Android\Sdk).
-- The NDK carries its own sysroot for every architecture; nothing else is asked.
local util = kormake.load("util")
local M = {}

local function env(name)
  local v = kfs.getenv(name)
  return v ~= nil and v ~= "" and v or nil
end

-- The SDK's directory, or nil: where the NDK, the build tools and adb are installed.
function M.sdk()
  local home = env("HOME") or env("USERPROFILE")
  local candidates = { env("ANDROID_HOME"), env("ANDROID_SDK_ROOT"),
                       home and util.join(home, "Android/Sdk"),
                       home and util.join(home, "Library/Android/sdk"),
                       env("LOCALAPPDATA") and util.join(env("LOCALAPPDATA"), "Android/Sdk") }
  for i = 1, 5 do
    local d = candidates[i]
    if d and kfs.isdir(d) then return d end
  end
  return nil
end

local host_tags = { linux = "linux-x86_64", macos = "darwin-x86_64", windows = "windows-x86_64" }

function M.run(_, tc)
  local f = tc.fields
  local want = f.ndk or util.fail("toolchain '%s' pins no NDK version (ndk = \"30.0.16248370\")", tc.name)
  local ndk = env("ANDROID_NDK_HOME")
  if not ndk then
    local sdk = M.sdk()
    ndk = sdk and util.join(sdk, "ndk/" .. want)
  end
  if not ndk or not kfs.isdir(ndk) then
    util.fail("android: no NDK %s. Install it with\n  sdkmanager \"ndk;%s\"\n" ..
              "under ANDROID_HOME (or ~/Android/Sdk), or point ANDROID_NDK_HOME at an unpacked one. README.md, \"Android\", has the steps.",
              want, want)
  end
  local exe = KORMAKE_HOST == "windows" and ".exe" or ""
  local bin = util.join(ndk, "toolchains/llvm/prebuilt/" .. (host_tags[KORMAKE_HOST] or "linux-x86_64") .. "/bin")
  local cxx, ar = util.join(bin, "clang++" .. exe), util.join(bin, "llvm-ar" .. exe)
  if not kfs.isfile(cxx) then util.fail("android: %s has no compiler at %s", ndk, cxx) end
  if not f.arch or not f.triple then util.fail("toolchain '%s' needs arch and triple (\"aarch64\", \"linux-android33\")", tc.name) end
  local target = "--target=" .. f.arch .. "-" .. f.triple
  return { cxx = cxx, ar = ar, cxxflags = { target }, ldflags = { target },
           cc = util.join(bin, "clang" .. exe), cflags = { target } }
end

return M
