-- kormake: small helpers shared by the scripts.
local M = {}

function M.fail(fmt, ...)
  error(string.format(fmt, ...), 0)
end

-- A failure that says a module cannot build where it was asked for (a platform it does not
-- support, an external or an interface the platform lacks, a package not fetched), not that
-- anything is wrong: a module outside the roots that fails so is left out of the build;
-- anything else (a cycle, a missing module) stops it. Reported like fail's otherwise.
function M.soft_fail(fmt, ...)
  error(setmetatable({ soft = true, msg = string.format(fmt, ...) }, { __tostring = function(e) return e.msg end }), 0)
end

-- The message of an error caught by pcall: fail's string, or soft_fail's.
function M.message(err)
  if type(err) == "table" and err.msg then return err.msg end
  return tostring(err)
end

function M.basename(path)
  return (path:gsub("^.*/", ""))
end

function M.dirname(path)
  local d = path:match("^(.*)/[^/]*$")
  return d or "."
end

function M.stem(path)
  local b = M.basename(path)
  return (b:gsub("%.[^%.]*$", ""))
end

function M.ext(path)
  return path:match("%.([^%./]*)$") or ""
end

function M.join(a, b)
  if a == "" or a == "." then return b end
  if a:sub(-1) == "/" then return a .. b end
  return a .. "/" .. b
end

function M.trim(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- A path in the host's own spelling, for a command's arguments: backslashes on Windows,
-- where a '/' would read as a switch.
function M.native(path)
  if KORMAKE_HOST == "windows" then return (path:gsub("/", "\\")) end
  return path
end

-- Absolute: "/x" on POSIX; on Windows also a drive ("C:/x", "C:\x") or a share ("//host/x").
function M.is_abs(path)
  local c = path:sub(1, 1)
  return c == "/" or c == "\\" or path:match("^%a:[/\\]") ~= nil
end

function M.expand_home(path)
  if path:sub(1, 1) == "~" then
    local home = kfs.getenv("HOME") or kfs.getenv("USERPROFILE") or ""
    return (home:gsub("\\", "/")) .. path:sub(2)
  end
  return path
end

-- Runs a Ninja command line in the build directory `build`, holding its lock
-- (<build>/.korvid.lock): Ninja takes none, and two runs in one directory can spoil its
-- logs. The linked tools' shim takes the same lock; KORVID_LINK_HELD tells a shim run
-- under this one (a cook that calls a linked tool) to leave the building to it. A run
-- inside one that holds the lock already takes it again for nothing.
function M.ninja_exec(build, cmd)
  if kfs.getenv("KORVID_LINK_HELD") == build then return kfs.exec(cmd) end
  kfs.mkdirp(build)
  local h = kfs.lock(M.join(build, ".korvid.lock"))
  local before = kfs.getenv("KORVID_LINK_HELD")
  kfs.setenv("KORVID_LINK_HELD", build)
  local ok, rc = pcall(kfs.exec, cmd)
  kfs.setenv("KORVID_LINK_HELD", before)
  kfs.unlock(h)
  if not ok then error(rc, 0) end
  return rc
end

-- One argument for the host's command shell (kfs.exec): sh on POSIX, cmd on Windows,
-- where a path cannot hold a double quote.
function M.shell_quote(s)
  if KORMAKE_HOST == "windows" then return '"' .. s .. '"' end
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Copies the directory from into to, recursively, making directories as needed; hidden
-- entries are skipped, as sorted_files_recursive skips them.
function M.copy_tree(from, to)
  kfs.mkdirp(to)
  for _, rel in ipairs(M.sorted_files_recursive(from)) do
    local dest = M.join(to, rel)
    kfs.mkdirp(M.dirname(dest))
    kfs.copy(M.join(from, rel), dest)
  end
end

-- A glob with * and ? in the last component only, matched against a directory listing.
function M.glob_to_pattern(glob)
  return "^" .. glob:gsub("[%^%$%(%)%%%.%[%]%+%-]", "%%%0"):gsub("%*", ".*"):gsub("%?", ".") .. "$"
end

function M.matches(name, glob)
  return name:match(M.glob_to_pattern(glob)) ~= nil
end

function M.sorted_files(dir, ext)
  local out = {}
  for _, n in ipairs(kfs.listdir(dir)) do
    if ext == nil or M.ext(n) == ext then
      if kfs.isfile(M.join(dir, n)) then out[#out + 1] = n end
    end
  end
  table.sort(out)
  return out
end

-- Every file under dir, recursively, as paths relative to it ("maps/town.tmj"), sorted;
-- hidden entries (a leading '.') skipped.
function M.sorted_files_recursive(dir)
  local out = {}
  local function walk(rel)
    local here = rel == "" and dir or M.join(dir, rel)
    for _, n in ipairs(kfs.listdir(here)) do
      if n:sub(1, 1) ~= "." then
        local r = rel == "" and n or (rel .. "/" .. n)
        if kfs.isdir(M.join(dir, r)) then walk(r) elseif kfs.isfile(M.join(dir, r)) then out[#out + 1] = r end
      end
    end
  end
  walk("")
  table.sort(out)
  return out
end

function M.sorted_keys(t)
  local ks = {}
  for k in pairs(t) do ks[#ks + 1] = k end
  table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)
  return ks
end

function M.contains(list, v)
  for _, x in ipairs(list) do if x == v then return true end end
  return false
end

function M.as_list(v)
  if v == nil then return {} end
  if type(v) == "table" then return v end
  return { v }
end

-- Ninja escaping for paths and values.
function M.ninja_escape(s)
  return (s:gsub("([ :$])", "$%1"))
end

-- A list of paths for a build line: each escaped, joined by spaces.
function M.ninja_paths(list)
  local esc = {}
  for _, s in ipairs(list) do esc[#esc + 1] = M.ninja_escape(s) end
  return table.concat(esc, " ")
end

function M.json_string(s)
  return '"' .. s:gsub('[\\"]', '\\%0'):gsub("\n", "\\n") .. '"'
end

-- Whether a module's external applies on a platform: a version (sdl3 = ">=3.2") applies
-- everywhere; a table may name its platforms (sdl3 = { ">=3.2", platforms = { "linux" } }).
function M.external_on(spec, platform)
  if type(spec) ~= "table" or not spec.platforms then return true end
  return M.contains(M.as_list(spec.platforms), platform)
end

-- A symbol from a module name: dashes and dots become underscores.
function M.symbol(name)
  return (name:gsub("[%-%.]", "_"))
end

return M
