-- kormake: the MSVC probe, for a toolchain with probe = "msvc". Finds the newest Visual
-- Studio (or Build Tools) with the C++ tools through vswhere, takes the environment its
-- vcvarsall.bat sets up for the host architecture, and returns what the Ninja file then
-- holds outright: absolute paths for cl, lib and link, the INCLUDE directories as
-- /external:I flags, the LIB directories as /LIBPATH: flags, and Visual Studio's Ninja.
-- The build then works from any prompt, regenerations included. Run from a Developer
-- prompt, the environment already there is used and nothing is searched for.
local util = kormake.load("util")
local M = {}

local function split(list)
  local out = {}
  for entry in (list or ""):gmatch("[^;]+") do
    local d = util.trim(entry)
    if d ~= "" then out[#out + 1] = (d:gsub("\\", "/"):gsub("/$", "")) end
  end
  return out
end

-- NAME=value lines, as cmd's set prints them, keyed by upper-case name.
local function parse_env(text)
  local env = {}
  for line in text:gmatch("[^\r\n]+") do
    local k, v = line:match("^([^=]+)=(.*)$")
    if k then env[k:upper()] = v end
  end
  return env
end

local function from_vcvars(build, arch)
  local q = util.shell_quote
  local pf86 = kfs.getenv("ProgramFiles(x86)") or "C:/Program Files (x86)"
  local installer = pf86:gsub("\\", "/") .. "/Microsoft Visual Studio/Installer"
  local vswhere = installer .. "/vswhere.exe"
  if not kfs.isfile(vswhere) then
    util.fail("msvc: no Visual Studio Installer (%s); install Visual Studio or the Build Tools with \"Desktop development with C++\"", vswhere)
  end
  local comp = arch == "arm64" and "Microsoft.VisualStudio.Component.VC.Tools.ARM64" or "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
  local found = util.join(build, "msvc-vs.txt")
  if kfs.exec(string.format("%s -latest -prerelease -products * -requires %s -property installationPath > %s",
      q(util.native(vswhere)), comp, q(util.native(found)))) ~= 0 then
    util.fail("msvc: vswhere failed")
  end
  local vs = util.trim(kfs.read(found) or "")
  if vs == "" then util.fail("msvc: no Visual Studio with the C++ tools (%s) is installed", comp) end
  -- vcvarsall's helpers look for vswhere on PATH.
  local dump = util.join(build, "msvc-env.txt")
  local rc = kfs.exec(string.format('set "PATH=%s;%%PATH%%" && call %s %s >nul && set > %s',
    util.native(installer), q(util.native(vs .. "/VC/Auxiliary/Build/vcvarsall.bat")), arch, q(util.native(dump))))
  if rc ~= 0 then util.fail("msvc: vcvarsall.bat %s failed in %s", arch, vs) end
  return parse_env(kfs.read(dump) or "")
end

function M.run(build, tc)
  local arch = (kfs.getenv("PROCESSOR_ARCHITECTURE") or "AMD64"):upper() == "ARM64" and "arm64" or "x64"
  local env
  if kfs.getenv("VCToolsInstallDir") and kfs.getenv("INCLUDE") and kfs.getenv("LIB") then
    env = { VCTOOLSINSTALLDIR = kfs.getenv("VCToolsInstallDir"), VSINSTALLDIR = kfs.getenv("VSINSTALLDIR"),
            INCLUDE = kfs.getenv("INCLUDE"), LIB = kfs.getenv("LIB") }
  else
    env = from_vcvars(build, arch)
  end
  local tools = env.VCTOOLSINSTALLDIR or util.fail("msvc: the environment has no VCToolsInstallDir")
  local bin = (tools:gsub("\\", "/"):gsub("/$", "")) .. "/bin/Host" .. arch .. "/" .. arch
  local function tool(name)
    local p = bin .. "/" .. name
    if not kfs.isfile(p) then util.fail("msvc: %s is not in %s", name, bin) end
    return p
  end
  local q = util.shell_quote
  local r = { cxx = tool((tc.fields.cxx or "cl") .. ".exe"), ar = tool((tc.fields.ar or "lib") .. ".exe"),
              link = tool((tc.fields.link or "link") .. ".exe"), cxxflags = { "/external:W0" }, ldflags = {} }
  -- Headers the system provides are external: warnings in them are not ours.
  for _, d in ipairs(split(env.INCLUDE)) do r.cxxflags[#r.cxxflags + 1] = "/external:I" .. q(d) end
  for _, d in ipairs(split(env.LIB)) do r.ldflags[#r.ldflags + 1] = "/LIBPATH:" .. q(d) end
  -- C is the same cl, told so (/TC, the toolchain's cflags), with the same system headers.
  r.cc = r.cxx
  r.cflags = {}
  for _, f in ipairs(r.cxxflags) do r.cflags[#r.cflags + 1] = f end
  if env.VSINSTALLDIR then
    local ninja = (env.VSINSTALLDIR:gsub("\\", "/"):gsub("/$", "")) .. "/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
    if kfs.isfile(ninja) then r.ninja = ninja end
  end
  return r
end

return M
