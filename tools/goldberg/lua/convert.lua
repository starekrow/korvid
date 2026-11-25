-- Converters: where a step's script or program is, the command line that runs it, and
-- the names it said it would write. Used by the cook (graph.lua) and by `goldberg learn`.
--
-- A converter is registered in a .berg file under an asset root:
--   "cut": { "from": "sheet", "to": "texture", "shell": "tools/cut",
--            "args": "{in} {work}", "outputs": ["{stem}_*.png"] }
-- `shell` names a script without its extension, relative to that .berg file. It runs as
-- `/bin/sh tools/cut.sh <args>`, in the directory it is to write into ({work}).
-- `exec` instead names a program, run directly with the args and nothing between (one
-- process a run): under the .berg file's directory (an input, as a script is), on PATH, or
-- by an absolute path.
-- In `args` and `outputs`: {in} the source's full path, {work} the directory to write
-- into, {name} the source's file name, {stem} that name up to its first dot, and any key
-- of the asset's metadata ({filter}, {atlas.pad}). The same are in the environment, as
-- GOLDBERG_IN, GOLDBERG_WORK, GOLDBERG_NAME, GOLDBERG_STEM and GOLDBERG_META_<KEY>.
local berg = require("berg")

local M = {}

local function dirname(path) return path:match("^(.*)/[^/]*$") or "" end
local function basename(path) return path:match("([^/]*)$") end

-- A step's directory in the work root: one path component, the same for the same source
-- and converter, and readable enough to find by eye.
function M.work_dir(key, conv_name)
    return kv.sha256(key .. "\0" .. conv_name):sub(1, 8) .. "-" .. basename(key:match("^[^:]*:(.*)$") or key) .. "." .. conv_name
end

-- The converter's registration as text, the same for the same registration. "exec" only
-- when it is there, so a shell converter's text (and every step's key) is what it was.
function M.signature(conv)
    local parts = {}
    for _, k in ipairs{ "from", "to", "shell", "args", "outputs", "settle" } do
        parts[#parts + 1] = k .. "=" .. (conv[k] == nil and "" or berg.show(conv[k]))
    end
    if conv.exec ~= nil then parts[#parts + 1] = "exec=" .. berg.show(conv.exec) end
    if conv.builtin ~= nil then parts[#parts + 1] = "builtin=" .. conv.builtin .. "@" .. tostring((host.builtins or {})[conv.builtin]) end
    return table.concat(parts, " ")
end

local function is_abs(p) return p:sub(1, 1) == "/" or p:match("^%a:[/\\]") ~= nil end

-- An "exec" converter's program: a path under its .berg file's directory (the key of that
-- file, "<root>:<path>", and "exe": an input, as a script is; on Windows `<path>.exe` is
-- looked for first), or a name found on PATH or an absolute path (false, "exe" and the
-- program: run as it is, and not something the session can hash). Or nil and why.
local function program(conv, has)
    local p = conv.exec
    if not p:find("/", 1, true) and not p:find("\\", 1, true) then return false, "exe", p end -- on PATH
    if is_abs(p) then return false, "exe", p end
    local root, berg_path = (conv.key or ""):match("^([^:]+):(.*)$")
    if not root then
        return nil, string.format('the converter "%s" is registered in %s: a program by a relative path is found beside the .berg file of an asset root, so register it in one, or give "exec" an absolute path', conv.name, conv.file)
    end
    if p:find("..", 1, true) then
        return nil, string.format('the converter "%s": "exec" names a program under its .berg file\'s directory, on PATH, or by an absolute path, not "%s"', conv.name, p)
    end
    local dir = dirname(berg_path)
    local base = root .. ":" .. (dir == "" and "" or dir .. "/") .. p
    if host.windows and has(base .. ".exe") then return base .. ".exe", "exe" end
    if has(base) then return base, "exe" end
    return nil, string.format('the converter "%s" runs %s%s, which is not there (registered in %s)', conv.name, p, host.windows and "(.exe)" or "", conv.file)
end

-- script(conv, has): the key ("<root>:<path>") of the script's file and its kind ("sh",
-- "bat" or, for an "exec" converter, "exe"), or nil and why. For a program that is not
-- under an asset root (on PATH, or by an absolute path), false, "exe" and the program.
-- has(key) says whether the session knows such a file.
function M.script(conv, has)
    if conv.builtin then return false, "builtin", conv.builtin end
    if conv.exec then return program(conv, has) end
    local root, berg_path = (conv.key or ""):match("^([^:]+):(.*)$")
    if not root then
        return nil, string.format('the converter "%s" is registered in %s: a shell converter\'s script is found beside the .berg file of an asset root, so register it in one', conv.name, conv.file)
    end
    if conv.shell:find("..", 1, true) or conv.shell:sub(1, 1) == "/" then
        return nil, string.format('the converter "%s": "shell" names a script under its .berg file\'s directory, not "%s"', conv.name, conv.shell)
    end
    local dir = dirname(berg_path)
    local base = root .. ":" .. (dir == "" and "" or dir .. "/") .. conv.shell
    if host.windows and has(base .. ".bat") then return base .. ".bat", "bat" end
    if not has(base .. ".sh") then
        return nil, string.format('the converter "%s" runs %s.sh, which is not there (registered in %s)', conv.name, conv.shell, conv.file)
    end
    return base .. ".sh", "sh"
end

local function lookup(params, name)
    local v = params
    for part in name:gmatch("[^.]+") do
        if type(v) ~= "table" then return nil end
        v = v[part]
    end
    return v
end

local function subst(word, vars, params)
    local bad
    local out = word:gsub("{([%w_.%-]+)}", function(name)
        local v = vars[name]
        if v == nil then v = lookup(params, name) end
        if v == nil then bad = "{" .. name .. "} is not {in}, {work}, {name}, {stem} or a key of the asset's metadata" return "" end
        if type(v) == "table" then bad = "{" .. name .. "} is not a single value" return "" end
        return tostring(v)
    end)
    if bad then return nil, bad end
    return out
end

-- vars(input, work): the names every command line may use.
function M.vars(input, work)
    local name = basename(input)
    return { ["in"] = input, work = work, name = name, stem = name:match("^([^.]*)") }
end

-- command(conv, script, kind, vars, params): argv and env for one run, or nil and why.
function M.command(conv, script, kind, vars, params)
    local argv
    if kind == "builtin" then
        -- goldberg's own: its name, the source, and the asset path the result records.
        return { script, vars["in"], vars.asset or vars.name }, {}
    end
    if kind == "exe" then argv = { script } -- the program itself: no shell between
    elseif kind == "bat" then argv = { script, shell = true } -- a batch file is the shell's to run: host.run makes its line
    else argv = { host.windows and "sh" or "/bin/sh", script } end
    for word in (conv.args or "{in} {work}"):gmatch("%S+") do
        local w, bad = subst(word, vars, params or {})
        if not w then return nil, string.format('the converter "%s": in "args", %s (%s)', conv.name, bad, conv.file) end
        argv[#argv + 1] = w
    end
    local env = { "GOLDBERG_IN=" .. vars["in"], "GOLDBERG_WORK=" .. vars.work, "GOLDBERG_NAME=" .. vars.name, "GOLDBERG_STEM=" .. vars.stem }
    local keys = {}
    for k, v in pairs(params or {}) do if type(v) ~= "table" then keys[#keys + 1] = k end end
    table.sort(keys)
    for _, k in ipairs(keys) do env[#env + 1] = "GOLDBERG_META_" .. k:upper():gsub("[^%w]", "_") .. "=" .. tostring(params[k]) end
    return argv, env
end

-- patterns(conv, vars, params): the declared outputs with their names filled in: a list,
-- "scan", or nil when the converter declares none. Or nil and why.
function M.patterns(conv, vars, params)
    if type(conv.outputs) ~= "table" then return conv.outputs end
    local out = {}
    for i, p in ipairs(conv.outputs) do
        local w, bad = subst(p, vars, params or {})
        if not w then return nil, string.format('the converter "%s": in "outputs", %s (%s)', conv.name, bad, conv.file) end
        out[i] = w
    end
    return out
end

return M
