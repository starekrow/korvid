-- kormake: the Xcode probe, for a toolchain with probe = "xcode". Asks xcrun, which is
-- the one thing that knows which developer directory is active (Xcode.app, or the Command
-- Line Tools alone), for the C++ compiler and the SDK of the toolchain's `sdk` ("macosx",
-- "iphoneos", "iphonesimulator"), and returns them as the Ninja file then holds them
-- outright: an absolute clang++, -isysroot with the SDK's path, and -target from the
-- toolchain's `triple`. The build then works from any shell, regenerations
-- included, whatever PATH holds. KORMAKE_SDKROOT overrides the SDK's path.
local util = kormake.load("util")
local M = {}

-- The first line a command prints, or nil if it fails.
local function capture(build, name, command)
  local out = util.join(build, "xcode-" .. name .. ".txt")
  if kfs.exec(command .. " > " .. util.shell_quote(out) .. " 2>/dev/null") ~= 0 then return nil end
  local line = util.trim((kfs.read(out) or ""):match("[^\r\n]+") or "")
  return line ~= "" and line or nil
end

function M.run(build, tc)
  local q = util.shell_quote
  local sdk = tc.fields.sdk or "macosx"
  local cxx = capture(build, "cxx", "xcrun --sdk " .. sdk .. " -f clang++")
  if not cxx then
    util.fail("xcode: xcrun cannot find clang++ for the %s SDK. Install Xcode, or run `xcode-select --install` for the Command Line Tools.", sdk)
  end
  local root = kfs.getenv("KORMAKE_SDKROOT")
  if root == "" then root = nil end
  root = root or capture(build, "sdk", "xcrun --sdk " .. sdk .. " --show-sdk-path")
  if not root or not kfs.isdir(root) then
    util.fail("xcode: no %s SDK (xcrun --sdk %s --show-sdk-path). If Xcode is installed, point the system at it with\n" ..
              "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer", sdk, sdk)
  end
  local ar = capture(build, "ar", "xcrun --sdk " .. sdk .. " -f ar") or "ar"
  -- The target: Apple silicon, the toolchain's triple (its OS, release floor and, for a
  -- simulator, environment), so one clang serves the whole family.
  local target = tc.fields.triple and { "-target", "arm64-" .. tc.fields.triple } or {}
  local cxxflags, ldflags = { "-isysroot", q(root) }, { "-isysroot", q(root) }
  for _, f in ipairs(target) do cxxflags[#cxxflags + 1] = f; ldflags[#ldflags + 1] = f end
  return { cxx = cxx, ar = ar, cxxflags = cxxflags, ldflags = ldflags }
end

return M
